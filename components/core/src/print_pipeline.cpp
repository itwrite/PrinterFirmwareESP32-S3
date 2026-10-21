// print_pipeline.cpp —— 流式打印流水线实现
#include "core/print_pipeline.h"

#include <cstdio>
#include <cstring>

#include "core/lz4_block.h"

namespace ijpd {

Err PrintPipeline::start(const Config& cfg, const Os& os) noexcept {
    if (cfg.job_id == nullptr || os.now_ms == nullptr || os.on_event == nullptr) {
        return Err::INVALID_ARGUMENT;
    }
    // 打印回调按模式二选一
    if (cfg.data_mode == StartPrintMode::Raster && os.print_row == nullptr) {
        return Err::INVALID_ARGUMENT;
    }
    if (cfg.data_mode == StartPrintMode::EscPos && os.print_raw == nullptr) {
        return Err::INVALID_ARGUMENT;
    }
    if (cfg.slot_memory == nullptr || cfg.row_carry == nullptr ||
        cfg.slot_count < 2 || cfg.slot_count > SlotPool::kMaxSlotCount) {
        return Err::INVALID_ARGUMENT;
    }
    for (size_t i = 0; i < cfg::COMPRESSED_BUF_COUNT; ++i) {
        if (cfg.comp_buf[i] == nullptr) return Err::INVALID_ARGUMENT;
    }
    // 行参数自洽（router 已保证，防御；escpos 模式跳过行结构）
    if (cfg.data_mode == StartPrintMode::Raster) {
        if (cfg.bytes_per_row == 0 || cfg.total_rows == 0 ||
            static_cast<uint64_t>(cfg.bytes_per_row) * cfg.total_rows !=
                cfg.total_bytes_uncompressed) {
            return Err::INVALID_ARGUMENT;
        }
        if (os.print_row == nullptr) return Err::INVALID_ARGUMENT;
    } else {
        if (cfg.raw_chunk_bytes == 0) return Err::INVALID_ARGUMENT;
        if (os.print_raw == nullptr) return Err::INVALID_ARGUMENT;
    }

    const Err se = sm_.fire(JobEvent::StartAccepted);
    if (se != Err::OK) return se;  // 非空闲（JOB_ALREADY_RUNNING）

    cfg_ = cfg;
    os_ = os;
    last_error_ = Err::OK;
    started_ms_ = os.now_ms(os_.user);
    last_progress_ms_.store(started_ms_, std::memory_order_relaxed);
    cancel_requested_.store(false, std::memory_order_release);
    fatal_fired_.store(false, std::memory_order_release);
    cancel_done_.store(false, std::memory_order_release);
    all_rx_fired_.store(false, std::memory_order_release);
    all_printed_fired_.store(false, std::memory_order_release);
    decoded_packets_.store(0, std::memory_order_release);
    carry_len_ = 0;
    printed_rows_ = 0;
    printed_bytes_ = 0;
    finishing_warning_ = false;

    for (auto& b : bufs_) {
        b.st.store(static_cast<uint8_t>(BufSt::Empty), std::memory_order_relaxed);
    }
    rx_buf_ = 0xFFFFFFFF;

    // 内存与子模块初始化（此时无并发访问）
    if (pool_.reinit(cfg.slot_memory, cfg.slot_count, cfg.slot_size) != Err::OK) {
        (void)sm_.fire(JobEvent::PrepareFailed, Err::INVALID_ARGUMENT);
        return Err::INVALID_ARGUMENT;
    }
    pool_.bump_generation();  // 新任务代次（丢弃可能的历史残留）
    if (FlowControl::make(cfg::HIGH_WATERMARK, cfg::LOW_WATERMARK, &flow_) != Err::OK) {
        (void)sm_.fire(JobEvent::PrepareFailed, Err::INTERNAL_ERROR);
        return Err::INTERNAL_ERROR;
    }
    stats_.begin(started_ms_);
    stats_.on_queue_watermark(0);

    BinFrameParser::JobConfig pc{};
    pc.data_token = cfg.data_token;
    pc.total_packets = cfg.total_packets;
    pc.total_bytes_uncompressed = cfg.total_bytes_uncompressed;
    pc.nominal_packet_bytes = cfg.nominal_packet_bytes;
    pc.last_packet_bytes = cfg.last_packet_bytes;
    pc.compression = cfg.compression;
    pc.integrity = cfg.integrity;
    if (parser_.begin_job(pc) != Err::OK) {
        (void)sm_.fire(JobEvent::PrepareFailed, Err::INVALID_ARGUMENT);
        return Err::INVALID_ARGUMENT;
    }

    (void)sm_.fire(JobEvent::PrepareOk);
    return Err::OK;
}

// ===========================================================================
// rx
// ===========================================================================

void PrintPipeline::on_hello_ok_() noexcept {
    (void)sm_.fire(JobEvent::HandshakeOk);  // WAITING_DATA_CONN -> RECEIVING
}

void PrintPipeline::on_frame_(const BinFrameParser::Frame& f) noexcept {
    // 帧已通过全部校验（顺序/长度/CRC），载荷已在压缩缓冲中
    stats_.on_packet_received();
    CompBuf& b = bufs_[rx_buf_];
    b.ulen = f.uncompressed_len;
    b.clen = f.compressed_len;
    b.seq = f.index;
    b.fill_seq.store(next_fill_seq_++, std::memory_order_relaxed);  // rx 线程单写
    b.st.store(static_cast<uint8_t>(BufSt::Ready), std::memory_order_release);
    rx_buf_ = 0xFFFFFFFF;

    if (static_cast<uint32_t>(stats_.snapshot().packets_received) >=
        cfg_.total_packets) {
        if (!all_rx_fired_.exchange(true, std::memory_order_acq_rel)) {
            (void)sm_.fire(JobEvent::AllPacketsReceived);  // RECEIVING -> DRAINING
        }
    }
}

PrintPipeline::Pump PrintPipeline::rx_pump(const uint8_t* data, size_t len,
                                           size_t* consumed) noexcept {
    if (consumed != nullptr) *consumed = 0;
    if (fatal_fired_.load(std::memory_order_acquire) || terminal()) {
        return Pump::Failed;
    }
    if (cancel_requested_.load(std::memory_order_acquire)) {
        return Pump::Failed;
    }

    // 确保解析器有载荷缓冲：领取一个 Empty 压缩缓冲
    if (rx_buf_ == 0xFFFFFFFF) {
        for (size_t i = 0; i < cfg::COMPRESSED_BUF_COUNT; ++i) {
            uint8_t expect = static_cast<uint8_t>(BufSt::Empty);
            if (bufs_[i].st.compare_exchange_strong(
                    expect, static_cast<uint8_t>(BufSt::RxWriting),
                    std::memory_order_acq_rel)) {
                rx_buf_ = static_cast<uint32_t>(i);
                parser_.set_payload_buffer(cfg_.comp_buf[i],
                                           cfg::MAX_PACKET_COMPRESSED_SIZE + 1);
                break;
            }
        }
        if (rx_buf_ == 0xFFFFFFFF) {
            return Pump::WouldBlock;  // 两缓冲均忙：停止读取（TCP 背压）
        }
    }

    BinFrameParser::Events ev;
    struct Ctx {
        PrintPipeline* self;
    } ctx{this};
    ev.user = &ctx;
    ev.on_hello_ok = [](void* u) { static_cast<Ctx*>(u)->self->on_hello_ok_(); };
    ev.on_frame = [](void* u, const BinFrameParser::Frame& f) {
        static_cast<Ctx*>(u)->self->on_frame_(f);
    };

    const auto r = parser_.feed(data, len, ev);
    if (r.status == BinFrameParser::FeedStatus::Failed) {
        if (consumed != nullptr) *consumed = r.consumed;
        fail_(r.error);
        return Pump::Failed;
    }
    if (r.status == BinFrameParser::FeedStatus::NeedBuffer) {
        // 当前帧载荷跨越本块且缓冲已就绪（rx_buf_ 有效）：不应发生——
        // NeedBuffer 仅在派发后未重设缓冲时出现，此处直接补绑重试
        // （保持语义：返回已消费量，调用方重喂剩余）
        if (consumed != nullptr) *consumed = r.consumed;
        rx_buf_ = 0xFFFFFFFF;  // 下次进入重新领取（含刚派发后的新缓冲）
        return (r.consumed < len) ? Pump::WouldBlock : Pump::Progress;
    }
    if (consumed != nullptr) *consumed = len;
    return Pump::Progress;
}

bool PrintPipeline::rx_should_read() noexcept {
    return flow_.update(pool_.busy_count());
}

// ===========================================================================
// decode
// ===========================================================================

PrintPipeline::Pump PrintPipeline::decode_pump() noexcept {
    if (fatal_fired_.load(std::memory_order_acquire) ||
        cancel_requested_.load(std::memory_order_acquire) || terminal()) {
        return Pump::Failed;
    }

    // 1) 领取“最早就绪”的压缩缓冲（到达序，防止乱序解码）
    uint32_t bi = 0xFFFFFFFF;
    uint32_t best_seq = 0xFFFFFFFF;
    for (size_t i = 0; i < cfg::COMPRESSED_BUF_COUNT; ++i) {
        if (bufs_[i].st.load(std::memory_order_acquire) != static_cast<uint8_t>(BufSt::Ready)) {
            continue;
        }
        const uint32_t fs = bufs_[i].fill_seq.load(std::memory_order_acquire);
        if (fs < best_seq) {
            best_seq = fs;
            bi = static_cast<uint32_t>(i);
        }
    }
    if (bi == 0xFFFFFFFF) return Pump::WouldBlock;
    {
        uint8_t expect = static_cast<uint8_t>(BufSt::Ready);
        if (!bufs_[bi].st.compare_exchange_strong(
                expect, static_cast<uint8_t>(BufSt::Decoding), std::memory_order_acq_rel)) {
            return Pump::WouldBlock;  // 单 decode 线程下不可达（防御）
        }
    }

    // 2) 领取空闲槽位（无则归还缓冲并等待）
    const uint32_t slot = pool_.try_acquire_free();
    if (slot == SlotPool::kInvalidIndex) {
        bufs_[bi].st.store(static_cast<uint8_t>(BufSt::Ready), std::memory_order_release);
        return Pump::WouldBlock;
    }
    // 填充前捕获代次：若期间发生取消/致命（代次推进），本提交将以旧代次
    // 入队，被消费端自动丢弃（避免“新代次残留”永不回收）
    const uint32_t fill_gen = pool_.generation();
    if (cancel_requested_.load(std::memory_order_acquire) ||
        fatal_fired_.load(std::memory_order_acquire)) {
        // 取槽后才发现取消/致命：放弃本槽，旧代次入队也会被丢弃
        (void)pool_.abort_fill(slot);
        bufs_[bi].st.store(static_cast<uint8_t>(BufSt::Empty), std::memory_order_release);
        return Pump::Failed;
    }

    const CompBuf& b = bufs_[bi];
    uint8_t* dst = pool_.slot_data(slot);
    const uint8_t* src = cfg_.comp_buf[bi];

    if (cfg_.compression == Compression::None || b.clen == b.ulen) {
        std::memcpy(dst, src, b.clen);  // 存储兜底 / 无压缩
    } else {
        const int r = lz4_block_decompress(src, b.clen, dst, pool_.slot_size(), b.ulen);
        if (r < 0) {
            stats_.on_decode_error();
            pool_.abort_fill(slot);
            bufs_[bi].st.store(static_cast<uint8_t>(BufSt::Empty), std::memory_order_release);
            fail_(lz4_err_of(r));
            return Pump::Failed;
        }
    }

    // 全部普通字段读取必须在释放缓冲（store Empty）之前：此后 rx 可并发覆写
    const uint16_t ulen = b.ulen;
    const Err cfe = pool_.commit_filled(slot, b.seq, ulen, fill_gen);
    if (cfe == Err::STALE_GENERATION) {
        // 提交时代次已过期（取消/致命已发生）：槽位已被池回收，本包作废；
        // 不计错误、不再 fail_（终结流程已在进行）
        bufs_[bi].st.store(static_cast<uint8_t>(BufSt::Empty), std::memory_order_release);
        return Pump::Failed;
    }
    if (cfe != Err::OK) {
        stats_.on_decode_error();
        (void)pool_.abort_fill(slot);
        bufs_[bi].st.store(static_cast<uint8_t>(BufSt::Empty), std::memory_order_release);
        fail_(Err::INTERNAL_ERROR);  // 槽位容量 < 包长（配置错误，不应发生）
        return Pump::Failed;
    }
    bufs_[bi].st.store(static_cast<uint8_t>(BufSt::Empty), std::memory_order_release);

    const uint32_t n = decoded_packets_.fetch_add(1, std::memory_order_relaxed) + 1;
    stats_.on_packet_decoded(ulen);
    stats_.on_queue_watermark(pool_.busy_count());
    if (n % cfg::PROGRESS_EVERY_PACKETS == 0) {
        // 包粒度进度节流（时间粒度在 tick）
        last_progress_ms_.store(0, std::memory_order_relaxed);  // 强制下次 tick 立即上报
    }
    return Pump::Progress;
}

// ===========================================================================
// print
// ===========================================================================

PrintPipeline::Pump PrintPipeline::print_pump() noexcept {
    if (fatal_fired_.load(std::memory_order_acquire) || terminal()) {
        // 错误/取消终态：排空残留。注意 try_acquire_ready 对"当前代次"
        // 的条目会 CAS 成 PROCESSING 并返回 true——必须 release,
        // 否则槽位泄漏(fatal 标志可见早于 bump_generation 的窗口内,
        // 队列中的条目仍是当前代次)。
        drain_ready_();
        return Pump::Failed;
    }

    // 取消路径：cancel() 先置标志再触发 CancelRequested；本线程等待状态
    // 已进入 Cancelling 后才收尾（否则下一轮重查），避免 TOCTOU 烧掉一次标志
    if (cancel_requested_.load(std::memory_order_acquire)) {
        // 排空：取消过程中可能有 decode 已提交的槽位需要回收(同样必须 release)
        drain_ready_();
        if (sm_.state() == JobState::Cancelling &&
            !cancel_done_.exchange(true, std::memory_order_acq_rel)) {
            (void)sm_.fire(JobEvent::CancelDone, Err::JOB_CANCELLED);
            last_error_ = Err::JOB_CANCELLED;
            Event ev;
            ev.kind = Event::Kind::Cancelled;
            ev.code = Err::JOB_CANCELLED;
            ev.state = sm_.state();
            ev.generation = pool_.generation();
            ev.stats = stats_.snapshot();
            ev.duration_ms = os_.now_ms(os_.user) - started_ms_;
            os_.on_event(os_.user, ev);
        }
        return Pump::Failed;
    }
    if (terminal()) return Pump::Failed;

    // 排空判定（8.1）：收齐 + 解齐 + 印完 + 无残留
    if (!all_printed_fired_.load(std::memory_order_acquire) &&
        all_rx_fired_.load(std::memory_order_acquire) &&
        decoded_packets_.load(std::memory_order_acquire) == cfg_.total_packets &&
        printed_bytes_ == cfg_.total_bytes_uncompressed) {
        // 数据面条件齐备,但状态机可能仍在 RECEIVING(AllPacketsReceived
        // 在 rx 线程尚未落地)——此时触发 AllPrinted 会被状态机拒绝,
        // 且一次性标志会导致永不再试 → 卡死 DRAINING。等待状态到位再发。
        if (sm_.state() != JobState::Draining) {
            return Pump::WouldBlock;  // 下轮重查(rx 的状态迁移在途)
        }
        if (all_printed_fired_.exchange(true, std::memory_order_acq_rel)) {
            return Pump::WouldBlock;
        }
        if (carry_len_ != 0) {  // 理论不可达（total = bpr*rows 保证）
            fail_(Err::INTERNAL_ERROR);
            return Pump::Failed;
        }
        (void)sm_.fire(JobEvent::AllPrinted);  // DRAINING -> FINALIZING
        return Pump::Done;
    }
    if (all_printed_fired_.load(std::memory_order_acquire)) {
        return Pump::Done;  // 等待宿主 finish()
    }

    SlotPool::ReadyItem item;
    if (!pool_.try_acquire_ready(&item)) {
        return Pump::WouldBlock;
    }

    const uint8_t* src = pool_.slot_data(item.slot_index);
    size_t off = 0;
    const size_t len = item.valid_len;

    // ---- escpos 模式：按块转发原始流（无行结构） ----
    if (cfg_.data_mode == StartPrintMode::EscPos) {
        while (off < len) {
            if (cancel_requested_.load(std::memory_order_acquire)) {
                (void)pool_.release(item.slot_index);
                return Pump::Progress;  // 下轮取消收尾
            }
            const size_t chunk = (len - off < cfg_.raw_chunk_bytes)
                                     ? (len - off)
                                     : cfg_.raw_chunk_bytes;
            const Err pr = os_.print_raw(os_.user, src + off, chunk);
            if (pr != Err::OK) {
                (void)pool_.release(item.slot_index);
                fail_(Err::HAL_FAULT);
                return Pump::Failed;
            }
            off += chunk;
        }
        (void)pool_.release(item.slot_index);
        printed_bytes_ += len;
        stats_.on_bytes_printed(len);
        stats_.on_queue_watermark(pool_.busy_count());
        return Pump::Progress;
    }

    // ---- raster 模式：跨包行重组（carry 保留不足一行的尾部） ----
    const uint32_t bpr = cfg_.bytes_per_row;

    while (off < len) {
        if (cancel_requested_.load(std::memory_order_acquire)) {
            (void)pool_.release(item.slot_index);
            return Pump::Progress;  // 下轮进入取消收尾
        }
        const size_t need = bpr - carry_len_;
        const size_t take = (len - off < need) ? (len - off) : need;
        std::memcpy(cfg_.row_carry + carry_len_, src + off, take);
        carry_len_ += take;
        off += take;
        if (carry_len_ == bpr) {
            const Err pr = os_.print_row(os_.user, cfg_.row_carry, bpr, printed_rows_);
            if (pr != Err::OK) {
                (void)pool_.release(item.slot_index);
                fail_(Err::HAL_FAULT);
                return Pump::Failed;
            }
            ++printed_rows_;
            carry_len_ = 0;
        }
    }
    (void)pool_.release(item.slot_index);
    printed_bytes_ += len;
    stats_.on_bytes_printed(len);
    stats_.on_queue_watermark(pool_.busy_count());
    return Pump::Progress;
}

// ===========================================================================
// finish / cancel / tick / fail
// ===========================================================================

Err PrintPipeline::finish(bool warning, Err warn_code) noexcept {
    if (sm_.state() != JobState::Finalizing) return Err::INVALID_ARGUMENT;
    const JobEvent ev = warning ? JobEvent::FinishWarning : JobEvent::FinishOk;
    if (sm_.fire(ev, warn_code) != Err::OK) return Err::INVALID_ARGUMENT;

    Event e;
    e.kind = warning ? Event::Kind::DoneWithWarning : Event::Kind::Completed;
    e.code = warning ? warn_code : Err::OK;
    e.state = sm_.state();
    e.generation = pool_.generation();
    e.stats = stats_.snapshot();
    e.duration_ms = os_.now_ms(os_.user) - started_ms_;
    os_.on_event(os_.user, e);
    return Err::OK;
}

void PrintPipeline::cancel() noexcept {
    if (terminal() || !job_state_is_active(sm_.state())) return;
    if (cancel_requested_.exchange(true, std::memory_order_acq_rel)) return;  // 幂等
    pool_.bump_generation();  // 旧代次数据在消费端丢弃
    // 与 fail_() 同理：主动排空 READY 残留（不依赖 print 泵被再次调度）
    drain_ready_();
    (void)sm_.fire(JobEvent::CancelRequested, Err::JOB_CANCELLED);
}

// try_acquire_ready 的契约：返回 true 时条目已 CAS 为 PROCESSING，
// 调用方必须 release；旧代次条目由其内部丢弃回收，不会返回。
// 排空场景（终态/取消）无论代次一律取出即释放，杜绝槽位泄漏。
void PrintPipeline::drain_ready_() noexcept {
    SlotPool::ReadyItem item;
    while (pool_.try_acquire_ready(&item)) {
        (void)pool_.release(item.slot_index);
    }
}

void PrintPipeline::fail(Err code) noexcept { fail_(code); }

void PrintPipeline::tick(uint64_t now_ms) noexcept {
    if (!active()) return;
    const uint64_t last = last_progress_ms_.load(std::memory_order_relaxed);
    const bool force = last == 0;
    if (force || now_ms - last >= cfg::PROGRESS_INTERVAL_MS) {
        last_progress_ms_.store(now_ms, std::memory_order_relaxed);
        Event ev;
        ev.kind = Event::Kind::Progress;
        ev.state = sm_.state();
        ev.generation = pool_.generation();
        ev.stats = stats_.snapshot();
        ev.duration_ms = now_ms - started_ms_;
        os_.on_event(os_.user, ev);
    }
}

void PrintPipeline::fail_(Err code) noexcept {
    if (fatal_fired_.exchange(true, std::memory_order_acq_rel)) return;
    last_error_ = code;
    pool_.bump_generation();  // 残留 READY 项变为旧代次
    // 终态不依赖 print 泵被再次调度：此处主动排空(fatal_fired_ 保证单线程进入)
    drain_ready_();
    (void)sm_.fire(JobEvent::FatalError, code);
    Event ev;
    ev.kind = Event::Kind::Failed;
    ev.code = code;
    ev.state = sm_.state();
    ev.generation = pool_.generation();
    ev.stats = stats_.snapshot();
    ev.duration_ms = os_.now_ms(os_.user) - started_ms_;
    os_.on_event(os_.user, ev);
}

}  // namespace ijpd
