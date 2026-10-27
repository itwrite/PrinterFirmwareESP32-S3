// ============================================================================
// print_pipeline.h —— 流式打印流水线（Phase 6，core，可主机验证）
//
// 三泵模型（阻塞语义由宿主任务组合；泵本身非阻塞、可轮询）：
//   rx_pump     [rx 线程]     socket 字节 → Hello/帧校验 → 压缩缓冲
//   decode_pump [decode 线程] 压缩缓冲 → LZ4/拷贝 → 槽位（READY）
//   print_pump  [print 线程]  槽位 → 逐行 print_row 回调（跨包行重组）
//
// 状态机（任务书第八章）由本类单点驱动：
//   start(): StartAccepted → Preparing → PrepareOk → WAITING_DATA_CONN
//   rx hello ok: HandshakeOk → RECEIVING；收齐包: AllPacketsReceived → DRAINING
//   print 排空: AllPrinted → FINALIZING；finish(): FinishOk/FinishWarning
//   cancel()/致命错误: CANCELLING→CANCELLED / FatalError→ERROR
//
// 完成条件（8.1，全部满足才回报完成）：
//   packets_received==total && packets_decoded==total && bytes_decoded==total
//   && printed_bytes==total && 槽位全部回收
//
// 线程安全性：三泵各归其线程；cancel() 任意线程；内部跨线程状态均为原子
//             或无锁结构（SlotPool/Vyukov、JobStateMachine/CAS、Stats/原子）。
// 所有权：    全部内存由 Config 注入（PSRAM/主机堆），运行期零分配。
// 背压：      rx_should_read() 按高低水位滞回（TCP 窗口收缩）。
// ============================================================================
#ifndef IJPD_CORE_PRINT_PIPELINE_H
#define IJPD_CORE_PRINT_PIPELINE_H

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "core/bin_frame_parser.h"
#include "core/command_router.h"
#include "core/config_limits.h"
#include "core/errors.h"
#include "core/flow_control.h"
#include "core/job_state_machine.h"
#include "core/protocol_types.h"
#include "core/slot_pool.h"
#include "core/stats.h"

namespace ijpd {

class PrintPipeline {
public:
    // ---- 任务参数（由 CommandRouter::finalize_start_print 保证自洽） ----
    struct Config {
        const char* job_id = nullptr;  // 引用必须存活至任务结束
        uint32_t data_token = 0;
        uint32_t total_packets = 0;
        uint64_t total_bytes_uncompressed = 0;
        uint16_t nominal_packet_bytes = 0;
        uint16_t last_packet_bytes = 0;
        // raster 模式（mode=Raster 时使用；escpos 时为 0）
        uint32_t bytes_per_row = 0;
        uint32_t total_rows = 0;
        // escpos 模式：转发块大小（mode=EscPos 时使用）
        uint32_t raw_chunk_bytes = 512;
        // 数据模式
        StartPrintMode data_mode = StartPrintMode::Raster;  // 需 command_router.h
        Compression compression = Compression::None;
        Integrity integrity = Integrity::None;
        // 外部注入内存（PSRAM 或主机堆）
        uint8_t* comp_buf[cfg::COMPRESSED_BUF_COUNT] = {nullptr, nullptr};
        uint8_t* slot_memory = nullptr;
        size_t slot_count = 0;
        size_t slot_size = 0;
        uint8_t* row_carry = nullptr;  // 跨包行重组缓冲（>= bytes_per_row）
    };

    // ---- 事件（report 路径；print 线程之外的序列化由宿主完成） ----
    struct Event {
        enum class Kind : uint8_t { Progress, Completed, DoneWithWarning, Cancelled, Failed };
        Kind kind = Kind::Progress;
        Err code = Err::OK;  // Failed 携带致命码
        uint32_t generation = 0;
        JobState state = JobState::Idle;  // 事件产生时的任务状态
        Stats::Snapshot stats;
        uint64_t duration_ms = 0;
    };

    // ---- 宿主注入 ----
    struct Os {
        void* user = nullptr;
        uint64_t (*now_ms)(void* user) = nullptr;
        // raster 模式：逐行打印（返回非 OK -> 致命 HAL_FAULT）
        Err (*print_row)(void* user, const uint8_t* row, size_t row_len,
                         uint32_t row_index) = nullptr;
        // escpos 模式：按块转发原始流（返回非 OK -> 致命 HAL_FAULT）
        Err (*print_raw)(void* user, const uint8_t* data, size_t len) = nullptr;
        void (*on_event)(void* user, const Event& ev) = nullptr;
        // Optional transport callbacks appended to preserve legacy aggregate
        // initialization of Os in existing hosts and tests.
        void (*on_hello)(void* user) = nullptr;
        void (*on_packet)(void* user, uint16_t packet_index) = nullptr;
    };

    enum class Pump : uint8_t { Progress, WouldBlock, Done, Failed };

    PrintPipeline() noexcept = default;

    // 启动任务（IDLE/终态 -> WAITING_DATA_CONN）。配置非法 -> PrepareFailed。
    Err start(const Config& cfg, const Os& os) noexcept;

    // ---- 三泵 ----
    // rx：consumed 语义同 BinFrameParser（WouldBlock 时重喂 data+consumed）
    Pump rx_pump(const uint8_t* data, size_t len, size_t* consumed) noexcept;
    bool rx_should_read() noexcept;  // 流控滞回
    Pump decode_pump() noexcept;
    Pump print_pump() noexcept;

    // FINALIZING 后由宿主评估切纸并收尾（warning=true -> DONE_WITH_WARNING）
    Err finish(bool warning, Err warn_code) noexcept;

    // 取消（任意线程；幂等）。推进代次 -> 旧数据在消费端被丢弃。
    void cancel() noexcept;

    // 外部致命错误（数据连接断开 / 超时等；任意线程，幂等）
    void fail(Err code) noexcept;

    // 周期调用（report 任务）：进度事件节流（PROGRESS_INTERVAL_MS / 每 N 包）
    void tick(uint64_t now_ms) noexcept;

    // ---- 观测 ----
    JobState state() const noexcept { return sm_.state(); }
    bool active() const noexcept { return job_state_is_active(sm_.state()); }
    bool terminal() const noexcept { return job_state_is_terminal(sm_.state()); }
    Err last_error() const noexcept { return last_error_; }
    uint32_t generation() const noexcept { return pool_.generation(); }
    const char* job_id() const noexcept { return cfg_.job_id; }
    Stats::Snapshot snapshot() const noexcept { return stats_.snapshot(); }
    bool all_slots_free() const noexcept { return pool_.all_free(); }
    uint64_t started_ms() const noexcept { return started_ms_; }

private:
    enum class BufSt : uint8_t { Empty = 0, RxWriting, Ready, Decoding };
    struct CompBuf {
        std::atomic<uint8_t> st{static_cast<uint8_t>(BufSt::Empty)};
        std::atomic<uint32_t> fill_seq{0};  // 到达序（rx 线程单写；decode 取最小）
        uint16_t ulen = 0;
        uint16_t clen = 0;
        uint32_t seq = 0;
    };

    // rx 线程内回调
    void on_hello_ok_() noexcept;
    void on_frame_(const BinFrameParser::Frame& f) noexcept;
    // 致命路径（单次）：状态机 + 事件
    void fail_(Err code) noexcept;
    void finish_cancel_() noexcept;  // CANCELLING -> CANCELLED + 事件
    // 排空就绪队列并回收全部槽位（终态/取消路径专用）：
    // try_acquire_ready 返回 true 的条目已被置为 PROCESSING，必须 release。
    void drain_ready_() noexcept;

    Config cfg_{};
    Os os_{};

    BinFrameParser parser_;
    SlotPool pool_;
    FlowControl flow_{};
    JobStateMachine sm_;
    Stats stats_;

    CompBuf bufs_[cfg::COMPRESSED_BUF_COUNT];
    uint32_t rx_buf_ = 0;    // rx 线程私有：当前绑定解析器的缓冲索引
    uint32_t next_fill_seq_ = 0;  // rx 线程私有：压缩缓冲到达序计数

    // print 线程私有（行重组）
    size_t carry_len_ = 0;
    uint32_t printed_rows_ = 0;
    uint64_t printed_bytes_ = 0;

    std::atomic<bool> cancel_requested_{false};
    std::atomic<bool> fatal_fired_{false};
    std::atomic<bool> cancel_done_{false};
    std::atomic<bool> all_rx_fired_{false};
    std::atomic<bool> all_printed_fired_{false};
    std::atomic<uint32_t> decoded_packets_{0};

    Err last_error_ = Err::OK;
    uint64_t started_ms_ = 0;                    // start() 前设置，线程启动后只读
    std::atomic<uint64_t> last_progress_ms_{0};  // decode 写 0 / tick 读写（宽松序）
    bool finishing_warning_ = false;
};

}  // namespace ijpd

#endif  // IJPD_CORE_PRINT_PIPELINE_H
