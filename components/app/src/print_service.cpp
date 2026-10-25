// print_service.cpp —— 设备侧打印编排实现
#include "app/print_service.h"

#include <cerrno>
#include <cstdio>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_system.h"

#include "core/crc32.h"
#include "core/lz4_block.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include "platform_esp/display_service.h"
#include "platform_esp/log_adapter.h"
#include "platform_esp/task_layout.h"

namespace ijpd {
namespace {
constexpr const char* kTag = "print";
constexpr size_t kEventQueueDepth = 16;

// 事件队列项
struct EventItem {
    PrintPipeline::Event ev;
    char job_id[cfg::JOB_ID_MAX + 1];
};

const char* status_text(const PrintPipeline::Event& ev) {
    switch (ev.kind) {
        case PrintPipeline::Event::Kind::Completed: return "completed";
        case PrintPipeline::Event::Kind::DoneWithWarning: return "done_with_warning";
        case PrintPipeline::Event::Kind::Cancelled: return "cancelled";
        case PrintPipeline::Event::Kind::Failed: return "error";
        default: return nullptr;
    }
}
}  // namespace

PrintService& PrintService::instance() noexcept {
    static PrintService s;
    return s;
}

bool PrintService::has_active_job() const noexcept {
    return pipeline_.active() || buffered_ || raw_active_ || data_fd_ >= 0 || raw_fd_ >= 0 ||
           close_req_ || raw_close_req_;
}

void PrintService::set_hal_bundle(const PrinterHalBundle& bundle) noexcept {
    if (running_ || pipeline_.active() || buffered_ || raw_active_) return;
    printhead_hal_ = bundle.printhead != nullptr ? bundle.printhead : &printhead_;
    stepper_hal_ = bundle.stepper != nullptr ? bundle.stepper : &stepper_;
    cutter_hal_ = bundle.cutter != nullptr ? bundle.cutter : &cutter_;
    sensors_hal_ = bundle.sensors != nullptr ? bundle.sensors : &sensors_;
}

void PrintService::set_mqtt_context(const char* base_topic, const char* device_id) noexcept {
    if (base_topic == nullptr) {
        mqtt_base_topic_[0] = '\0';
    } else {
        strncpy(mqtt_base_topic_, base_topic, sizeof(mqtt_base_topic_) - 1);
        mqtt_base_topic_[sizeof(mqtt_base_topic_) - 1] = '\0';
    }
    if (device_id == nullptr) {
        mqtt_device_id_[0] = '\0';
    } else {
        strncpy(mqtt_device_id_, device_id, sizeof(mqtt_device_id_) - 1);
        mqtt_device_id_[sizeof(mqtt_device_id_) - 1] = '\0';
    }
}

void PrintService::mock_delay(void*, uint32_t ms) noexcept {
    vTaskDelay(pdMS_TO_TICKS(ms));
}

uint64_t PrintService::os_now_ms(void*) noexcept { return task_layout::now_ms(); }

// 图片指纹:对实际送达打印头的每个字节做流式 CRC32(zlib 同源),
// 任务终态回传给 App 比对 —— 端到端证明"芯片收到的 == App 发出的"。
// 单写者(print 任务),report 任务读取;32 位对齐读原子。
Err PrintService::os_print_row(void* user, const uint8_t* row, size_t len,
                               uint32_t row_index) noexcept {
    auto* self = static_cast<PrintService*>(user);
    self->fp_state_ = crc32_update(self->fp_state_, row, len);
    self->fp_bytes_ += len;
    // 还原显示:行数据同步进屏幕页缓存(非活动任务为空操作)
    DisplayService::instance().feed_row(row_index, row, len);
    self->last_rx_ms_ = static_cast<uint32_t>(task_layout::now_ms());
    RasterMeta m{row_index};
    return self->printhead_hal_->submitRaster(row, len, m);
}

// ---------------------------------------------------------------- 事件

void PrintService::os_on_event(void* user, const PrintPipeline::Event& ev) noexcept {
    auto* self = static_cast<PrintService*>(user);
    self->enqueue_event_(ev);
    // 终态事件即刻请求数据连接关闭(不等 100ms tick):大幅收窄
    // [任务终态 → 新任务 start_print] 窗口内 data_fd_ 残留导致
    // 新任务数据连接被拒的竞态。
    if (ev.kind != PrintPipeline::Event::Kind::Progress) {
        IJPD_LOGW(kTag, "job terminal: kind=%d code=%s", static_cast<int>(ev.kind),
                  err_name(ev.code));
        DisplayService::instance().end_job();
        // 自动缓存审计:任务结束应归还全部槽位(PSRAM);异常驻留=泄漏,
        // 立即暴露以便定位。顺带打印堆水位。
        if (!self->pipeline_.all_slots_free()) {
            IJPD_LOGE(kTag, "slot leak after job (slots not all free)");
        }
        IJPD_LOGI(kTag, "mem after job: psram_free=%u heap_free=%u",
                  static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
                  static_cast<unsigned>(esp_get_free_heap_size()));
        self->request_close_data_();
    }
}

void PrintService::enqueue_event_(const PrintPipeline::Event& ev) noexcept {
    if (event_queue_ == nullptr) return;
    EventItem item;
    item.ev = ev;
    strncpy(item.job_id, job_id_, sizeof(item.job_id) - 1);
    item.job_id[sizeof(item.job_id) - 1] = '\0';
    if (xQueueSendToBack(static_cast<QueueHandle_t>(event_queue_), &item, 0) != pdTRUE) {
        IJPD_LOGW(kTag, "event queue full, dropped kind=%d", static_cast<int>(ev.kind));
    }
}

bool PrintService::serialize_event_(const PrintPipeline::Event& ev, char* buf,
                                    size_t cap, size_t* len) noexcept {
    const uint32_t seq = event_seq_ + 1;
    event_seq_ = seq;
    char msg_id[16];
    snprintf(msg_id, sizeof(msg_id), "evt-%04u", static_cast<unsigned>(seq));

    JsonWriter w(buf, cap);
    if (!w.begin_object()) return false;
    if (!w.kv_cstr("msg_id", msg_id)) return false;
    const bool terminal_ev = ev.kind != PrintPipeline::Event::Kind::Progress;
    if (!w.kv_cstr("cmd", terminal_ev ? "print_finished" : "print_progress")) return false;
    if (!w.key("data") || !w.begin_object()) return false;

    const bool ok = ev.kind == PrintPipeline::Event::Kind::Completed ||
                    ev.kind == PrintPipeline::Event::Kind::DoneWithWarning ||
                    ev.kind == PrintPipeline::Event::Kind::Progress;
    if (!w.kv_bool("success", ok)) return false;

    if (!w.key("result")) {
        return false;
    }
    if (!ok) {
        if (!w.value_null()) return false;
    } else if (!w.begin_object()) {
        return false;
    } else {
        const Stats::Snapshot& s = ev.stats;
        if (!w.kv_cstr("job_id", job_id_)) return false;
        if (!w.kv_cstr("status", terminal_ev ? status_text(ev) : job_state_name(ev.state)))
            return false;
        if (!w.kv_i64("packets_received", s.packets_received)) return false;
        if (!w.kv_i64("packets_decoded", s.packets_decoded)) return false;
        if (terminal_ev) {
            if (!w.kv_i64("bytes_printed", s.bytes_printed)) return false;
            if (!w.kv_i64("duration_ms", static_cast<int64_t>(ev.duration_ms))) return false;
            // 图片指纹:App 侧对发送载荷算同源 CRC32 比对,证明端到端一致
            if (!w.kv_i64("decoded_bytes", static_cast<int64_t>(fp_bytes_))) return false;
            if (!w.kv_i64("decoded_crc32",
                          static_cast<int64_t>(crc32_final(fp_state_)))) {
                return false;
            }
            if (!w.key("warning")) {
                return false;
            }
            if (ev.kind == PrintPipeline::Event::Kind::DoneWithWarning) {
                if (!w.value_cstr(err_name(ev.code))) return false;
            } else if (!w.value_null()) {
                return false;
            }
        } else {
            if (!w.kv_i64("bytes_decoded", s.bytes_decoded)) return false;
            if (!w.kv_i64("bytes_printed", s.bytes_printed)) return false;
            if (!w.kv_i64("queue_watermark", s.queue_watermark)) return false;
            if (!w.kv_i64("queue_watermark_max", s.queue_watermark_max)) return false;
            if (!w.kv_i64("throughput_bps", s.throughput_bps)) return false;
        }
        if (!w.end_object()) return false;
    }

    if (!w.key("error")) return false;
    if (ok) {
        if (!w.value_null()) return false;
    } else {
        if (!w.begin_object()) return false;
        if (!w.kv_cstr("code", err_name(ev.code))) return false;
        if (!w.kv_cstr("message", err_default_message(ev.code))) return false;
        if (!w.end_object()) return false;
    }
    if (!w.end_object()) return false;
    if (!w.end_object()) return false;
    if (!w.end_frame()) return false;
    *len = w.length();
    return w.ok();
}

// ---------------------------------------------------------------- 启动

Err PrintService::start(PlatformMemory* mem, ControlPlane* plane) noexcept {
    if (mem == nullptr || plane == nullptr) return Err::INVALID_ARGUMENT;
    mem_ = mem;
    plane_ = plane;

    row_carry_ = static_cast<uint8_t*>(heap_caps_malloc(cfg::SLOT_SIZE, MALLOC_CAP_SPIRAM));
    if (row_carry_ == nullptr) {
        IJPD_LOGE(kTag, "row carry alloc failed");
        return Err::PSRAM_UNAVAILABLE;
    }

    event_queue_ = xQueueCreate(kEventQueueDepth, sizeof(EventItem));
    if (event_queue_ == nullptr) return Err::OUT_OF_MEMORY;

    running_ = true;
    Err e = task_layout::spawn("rx", rx_task_, this, 0, task_layout::kPrioRx,
                               task_layout::kStackRx);
    if (e == Err::OK) {
        e = task_layout::spawn("decode", decode_task_, this, 1, task_layout::kPrioDecode,
                               task_layout::kStackDecode);
    }
    if (e == Err::OK) {
        e = task_layout::spawn("print", print_task_, this, 1, task_layout::kPrioPrint,
                               task_layout::kStackPrint);
    }
    if (e == Err::OK) {
        e = task_layout::spawn("report", report_task_, this, 0, task_layout::kPrioReport,
                               task_layout::kStackReport);
    }
    if (e != Err::OK) {
        running_ = false;
        return e;
    }

    (void)stepper_hal_->begin();
    (void)printhead_hal_->begin(PrintheadConfig{0, 200, 0});  // 由 start_print 重新 begin
    IJPD_LOGI(kTag, "print service started (mock hal)");
    return Err::OK;
}

// ---------------------------------------------------------------- Router 钩子

Err PrintService::hook_start_print(void* user, const StartPrintParams& p,
                                   JsonWriter& w) noexcept {
    auto* self = static_cast<PrintService*>(user);
    if (self->pipeline_.active() || self->raw_active_) {
        return Err::JOB_ALREADY_RUNNING;
    }
    // 上一任务已终态但数据 socket 清理尚未完成(rx_task 异步执行):
    // 此时接受新任务会在数据连接接入时被拒。直接回忙,客户端稍后重试即可
    // (清理通常在微秒~毫秒级完成)。
    if (self->data_fd_ >= 0 || self->raw_fd_ >= 0 || self->close_req_) {
        return Err::JOB_ALREADY_RUNNING;
    }
    if (!self->mem_->psram_available) {
        return Err::PSRAM_UNAVAILABLE;
    }
    if (p.transport == StartPrintTransport::Mqtt &&
        self->mqtt_base_topic_[0] == '\0') {
        return Err::UNSUPPORTED;
    }

    // 任务参数 -> pipeline 配置
    PrintPipeline::Config cfg;
    cfg.job_id = self->job_id_;
    strncpy(self->job_id_, p.job_id, sizeof(self->job_id_) - 1);
    self->job_id_[sizeof(self->job_id_) - 1] = '\0';
    cfg.job_id = self->job_id_;
    esp_fill_random(&cfg.data_token, sizeof(cfg.data_token));
    self->data_token_ = cfg.data_token;
    self->token_used_ = false;
    self->mqtt_transport_ = p.transport == StartPrintTransport::Mqtt;
    self->mqtt_next_chunk_ = 0;
    self->mqtt_total_chunks_ = 0;
    self->mqtt_sequence_.reset();
    self->cutter_after_finish_ = p.cutter_after_finish;
    cfg.total_packets = p.total_packets;
    cfg.total_bytes_uncompressed = p.total_bytes_uncompressed;
    cfg.nominal_packet_bytes = static_cast<uint16_t>(p.nominal_packet_bytes);
    cfg.last_packet_bytes = static_cast<uint16_t>(p.last_packet_bytes);
    cfg.bytes_per_row = p.bytes_per_row;
    cfg.total_rows = p.total_rows;
    cfg.compression = p.compression;
    cfg.integrity = p.integrity;
    cfg.data_mode = p.mode;
    cfg.raw_chunk_bytes = 512;  // escpos 转发块（Profile 细化后由 profile 提供）
    for (size_t i = 0; i < cfg::COMPRESSED_BUF_COUNT; ++i) {
        cfg.comp_buf[i] = self->mem_->comp[i];
    }
    cfg.slot_memory = self->mem_->slots;
    cfg.slot_count = cfg::SLOT_COUNT;
    cfg.slot_size = cfg::SLOT_SIZE;
    cfg.row_carry = self->row_carry_;

    // 页缓存直通判定:整页可放入常驻显示页缓存(与显示共享同一块 PSRAM)
    // → 接收全速、打印定速、边收边显,绕开槽池背压。
    const bool cmyk8_interleaved =
        self->profile_ != nullptr && self->profile_->pixel_format == PixelFormat::CMYK8 &&
        p.bytes_per_row == static_cast<uint32_t>(self->profile_->print_width_px) * 4;
    const uint32_t d_bits = cmyk8_interleaved ? 8 : (p.pixel_bits == 8 ? 8 : 1);
    const uint32_t d_planes = cmyk8_interleaved ? 4 : (p.image_planes == 0 ? 1 : p.image_planes);
    const uint32_t d_width = cmyk8_interleaved
                                 ? self->profile_->print_width_px
                                 : p.bytes_per_row * 8 / d_bits;
    RasterChannel cmyk_order[4] = {RasterChannel::Cyan, RasterChannel::Magenta,
                                   RasterChannel::Yellow, RasterChannel::Black};
    const RasterChannel* display_order = cmyk8_interleaved
                                             ? cmyk_order
                                             : p.channel_order;
    const uint32_t display_order_count = cmyk8_interleaved ? 4 : p.channel_order_count;
    const uint32_t display_bpp = cmyk8_interleaved ? 4 : 1;
    const uint32_t display_width_um = cmyk8_interleaved ? self->profile_->print_width_um : 0;
    const uint32_t display_dpi = self->profile_ != nullptr ? self->profile_->dpi : 0;
    const uint32_t display_rows = cmyk8_interleaved
                                      ? p.total_rows
                                      : p.total_rows / d_planes;
    const bool fits_page =
        d_planes <= 4 && p.bytes_per_row <= 384 &&
        display_rows <= 4000 && (cmyk8_interleaved || p.total_rows % d_planes == 0);
    self->buffered_ =
        fits_page && DisplayService::instance().begin_job(
                          d_width, display_rows, d_bits, d_planes,
                          p.bytes_per_row, display_order, display_order_count,
                          cmyk8_interleaved, display_bpp, display_width_um,
                          display_dpi) == Err::OK;
    if (self->buffered_) {
        // 直通模式:不走槽池流水线,本地帧解析 + 页缓存
        self->buf_cfg_ = BinFrameParser::JobConfig{};
        self->buf_cfg_.data_token = cfg.data_token;
        self->buf_cfg_.total_packets = p.total_packets;
        self->buf_cfg_.total_bytes_uncompressed = p.total_bytes_uncompressed;
        self->buf_cfg_.nominal_packet_bytes =
            static_cast<uint16_t>(p.nominal_packet_bytes);
        self->buf_cfg_.last_packet_bytes =
            static_cast<uint16_t>(p.last_packet_bytes);
        self->buf_cfg_.compression = p.compression;
        self->buf_cfg_.integrity = p.integrity;
        self->buf_carry_len_ = 0;
        self->buf_rows_stored_ = 0;
        self->buf_rows_printed_ = 0;
        self->buf_total_rows_ = p.total_rows;
        self->buf_recv_bytes_ = 0;
        self->buf_pkts_ = 0;
        self->buf_started_ms_ = task_layout::now_ms();
        self->buf_receiving_ = true;
        self->buf_printing_ = self->mqtt_transport_;
        self->buf_cancel_ = false;
        if (self->bfp_rx_.begin_job(self->buf_cfg_) != Err::OK ||
            self->bfp_rx_.set_payload_buffer(self->mem_->comp[0],
                                             cfg::MAX_PACKET_COMPRESSED_SIZE)
                != Err::OK) {
            self->buffered_ = false;
            self->buf_receiving_ = false;
            DisplayService::instance().end_job();
        } else {
            IJPD_LOGI(kTag, "buffered job: page-cache passthrough (rx unthrottled)");
        }
    } else {
        DisplayService::instance().end_job();  // 容量不满足:显示本任务不还原
    }

    PrintPipeline::Os os;
    os.user = self;
    os.now_ms = os_now_ms;
    os.print_row = os_print_row;
    os.on_event = os_on_event;

    PrintheadConfig ph{p.bytes_per_row, 2000, 0};  // Mock: 2000 行/秒
    (void)self->printhead_hal_->begin(ph);

    if (!self->buffered_) {
        // 还原显示(旧路径):像素宽 = 每行字节数×8/位深;像素高 = 总行数/平面数
        if (p.bytes_per_row > 0 && display_rows > 0) {
            (void)DisplayService::instance().begin_job(
                d_width, display_rows, d_bits, d_planes, p.bytes_per_row,
                display_order, display_order_count, cmyk8_interleaved, display_bpp,
                display_width_um, display_dpi);
        }
    }

    if (!self->buffered_) {
        const Err e = self->pipeline_.start(cfg, os);
        if (e != Err::OK) return e;
    }
    self->fp_state_ = CRC32_INIT_STATE;
    self->fp_bytes_ = 0;
    self->last_rx_ms_ = static_cast<uint32_t>(task_layout::now_ms());

    // 响应（协议 §4.3）
    if (!w.begin_object()) return Err::INTERNAL_ERROR;
    if (!w.kv_cstr("job_id", p.job_id)) return Err::INTERNAL_ERROR;
    if (!w.kv_cstr("status", self->mqtt_transport_ ? "waiting_mqtt_chunks"
                                                     : "waiting_data_conn")) {
        return Err::INTERNAL_ERROR;
    }
    if (self->mqtt_transport_) {
        const Err seq = self->mqtt_sequence_.begin(self->job_id_, std::strlen(self->job_id_));
        if (seq != Err::OK) return seq;
        char topic[192] = {};
        const int n = snprintf(topic, sizeof(topic), "%s/%s/job/%s/chunk",
                               self->mqtt_base_topic_, self->mqtt_device_id_, self->job_id_);
        if (n <= 0 || static_cast<size_t>(n) >= sizeof(topic) ||
            !w.kv_cstr("mqtt_chunk_topic", topic)) return Err::INTERNAL_ERROR;
    } else if (!w.kv_i64("data_port", cfg::DATA_TCP_PORT)) {
        return Err::INTERNAL_ERROR;
    }
    if (!w.kv_i64("data_token", static_cast<int64_t>(self->data_token_))) {
        return Err::INTERNAL_ERROR;
    }
    if (!w.kv_i64("nominal_packet_bytes", p.nominal_packet_bytes)) return Err::INTERNAL_ERROR;
    if (!w.kv_cstr("compression", p.compression == Compression::None ? "none" : "lz4_block")) {
        return Err::INTERNAL_ERROR;
    }
    if (!w.kv_cstr("integrity", p.integrity == Integrity::Crc32 ? "crc32" : "none")) {
        return Err::INTERNAL_ERROR;
    }
    if (!w.kv_cstr("mode", start_print_mode_name(p.mode))) {
        return Err::INTERNAL_ERROR;
    }
    if (!w.kv_i64("data_connect_timeout_ms", cfg::DATA_CONNECT_TIMEOUT_MS)) {
        return Err::INTERNAL_ERROR;
    }
    if (!w.key("limits") || !w.begin_object()) return Err::INTERNAL_ERROR;
    if (!w.kv_i64("max_packet_uncompressed", cfg::MAX_PACKET_UNCOMPRESSED_SIZE)) {
        return Err::INTERNAL_ERROR;
    }
    if (!w.kv_i64("max_job_bytes", cfg::MAX_JOB_BYTES)) return Err::INTERNAL_ERROR;
    if (!w.end_object()) return Err::INTERNAL_ERROR;
    return w.end_object() ? Err::OK : Err::INTERNAL_ERROR;
}

bool PrintService::raw_accept(void* user, int fd, const char* peer) noexcept {
    auto* self = static_cast<PrintService*>(user);
    if (!self->mem_->psram_available || self->raw_active_ ||
        self->pipeline_.active() || self->buffered_ || self->data_fd_ >= 0) {
        return false;
    }

    self->raw_fd_ = fd;
    self->raw_active_ = true;
    self->raw_close_req_ = false;
    self->raw_close_kind_ = static_cast<uint8_t>(PrintPipeline::Event::Kind::Completed);
    self->raw_close_code_ = Err::OK;
    self->raw_chunks_ = 0;
    self->raw_bytes_ = 0;
    self->raw_started_ms_ = task_layout::now_ms();
    self->last_rx_ms_ = static_cast<uint32_t>(self->raw_started_ms_);
    self->fp_state_ = CRC32_INIT_STATE;
    self->fp_bytes_ = 0;
    strncpy(self->job_id_, "escpos-raw", sizeof(self->job_id_) - 1);
    self->job_id_[sizeof(self->job_id_) - 1] = '\0';

    const PrintheadConfig ph{0, 1, 0, 512};
    if (self->printhead_hal_->begin(ph) != Err::OK) {
        self->raw_active_ = false;
        self->raw_fd_ = -1;
        return false;
    }
    IJPD_LOGI(kTag, "raw escpos conn accepted from %s", peer);
    return true;
}

void PrintService::mqtt_chunk(void* user, const uint8_t* data, size_t len) noexcept {
    auto* self = static_cast<PrintService*>(user);
    self->accept_mqtt_chunk_(data, len);
}

void PrintService::mqtt_disconnected(void* user) noexcept {
    auto* self = static_cast<PrintService*>(user);
    if (self->mqtt_transport_) self->fail_mqtt_job_(Err::DATA_CONNECTION_LOST);
}

void PrintService::fail_mqtt_job_(Err code) noexcept {
    if (buffered_) {
        finish_buffered_(PrintPipeline::Event::Kind::Failed, code);
    } else if (mqtt_transport_) {
        pipeline_.fail(code);
        mqtt_transport_ = false;
    }
}

void PrintService::accept_mqtt_chunk_(const uint8_t* data, size_t len) noexcept {
    if (!mqtt_transport_ || (!buffered_ && !pipeline_.active())) return;
    MqttChunkView view{};
    const Err parsed = parse_mqtt_chunk(data, len, &view);
    if (parsed != Err::OK || view.job_id_len != std::strlen(job_id_) ||
        std::memcmp(view.job_id, job_id_, view.job_id_len) != 0) {
        fail_mqtt_job_(parsed == Err::OK ? Err::INVALID_ARGUMENT : parsed);
        return;
    }
    last_rx_ms_ = static_cast<uint32_t>(task_layout::now_ms());
    const Err sequence_error = mqtt_sequence_.accept(view);
    if (sequence_error != Err::OK) {
        fail_mqtt_job_(sequence_error);
        return;
    }

    size_t offset = 0;
    while (offset < view.payload_len) {
        if (buffered_) {
            BinFrameParser::Events events{this, bfp_hello_cb_, bfp_frame_cb_};
            bfp_rx_.set_payload_buffer(mem_->comp[0], cfg::MAX_PACKET_COMPRESSED_SIZE + 1);
            const auto result = bfp_rx_.feed(view.payload + offset,
                                              view.payload_len - offset, events);
            offset += result.consumed;
            if (result.status == BinFrameParser::FeedStatus::Failed) {
                fail_mqtt_job_(result.error);
                return;
            }
            if (result.status == BinFrameParser::FeedStatus::NeedBuffer) continue;
            break;
        }
        size_t consumed = 0;
        const auto result = pipeline_.rx_pump(view.payload + offset,
                                               view.payload_len - offset, &consumed);
        offset += consumed;
        if (result == PrintPipeline::Pump::Failed) return;
        if (result == PrintPipeline::Pump::WouldBlock) return;
    }
    mqtt_next_chunk_ = mqtt_sequence_.next_index();
    mqtt_total_chunks_ = mqtt_sequence_.total_chunks();
}

void PrintService::close_raw_socket_() noexcept {
    const int fd = raw_fd_;
    raw_fd_ = -1;
    raw_close_req_ = false;
    raw_active_ = false;
    if (fd >= 0) {
        lwip_shutdown(fd, SHUT_RDWR);
        lwip_close(fd);
    }
}

void PrintService::finish_raw_(PrintPipeline::Event::Kind kind, Err code) noexcept {
    if (!raw_active_) return;
    raw_close_kind_ = static_cast<uint8_t>(kind);
    raw_close_code_ = code;
    raw_active_ = false;
    close_raw_socket_();

    PrintPipeline::Event ev{};
    ev.kind = kind;
    ev.code = code;
    ev.state = (kind == PrintPipeline::Event::Kind::Completed)
                   ? JobState::Completed
                   : (kind == PrintPipeline::Event::Kind::Cancelled
                          ? JobState::Cancelled
                          : JobState::Error);
    ev.stats.packets_received = raw_chunks_;
    ev.stats.packets_decoded = raw_chunks_;
    ev.stats.bytes_decoded = raw_bytes_;
    ev.stats.bytes_printed = raw_bytes_;
    ev.duration_ms = task_layout::now_ms() - raw_started_ms_;
    enqueue_event_(ev);
    IJPD_LOGI(kTag, "raw escpos terminal: kind=%d code=%s bytes=%llu",
              static_cast<int>(kind), err_name(code),
              static_cast<unsigned long long>(raw_bytes_));
}

Err PrintService::hook_cancel_print(void* user, const CancelPrintParams& p,
                                    JsonWriter& w) noexcept {
    auto* self = static_cast<PrintService*>(user);
    if (p.has_job_id && strncmp(p.job_id, self->job_id_, sizeof(self->job_id_)) != 0) {
        return Err::NO_ACTIVE_JOB;
    }
    if (self->buffered_) {
        if (!self->buf_receiving_ && !self->buf_printing_) {
            return Err::NO_ACTIVE_JOB;
        }
        self->buf_cancel_ = true;
        self->finish_buffered_(PrintPipeline::Event::Kind::Cancelled, Err::OK);
        if (!w.begin_object() || !w.kv_cstr("job_id", self->job_id_) ||
            !w.kv_cstr("status", "cancelling")) {
            return Err::INTERNAL_ERROR;
        }
        return w.end_object() ? Err::OK : Err::INTERNAL_ERROR;
    }
    if (self->raw_active_) {
        self->raw_close_kind_ = static_cast<uint8_t>(PrintPipeline::Event::Kind::Cancelled);
        self->raw_close_code_ = Err::JOB_CANCELLED;
        self->raw_close_req_ = true;
        if (!w.begin_object() || !w.kv_cstr("job_id", self->job_id_) ||
            !w.kv_cstr("status", "cancelling")) return Err::INTERNAL_ERROR;
        return w.end_object() ? Err::OK : Err::INTERNAL_ERROR;
    }
    if (!self->pipeline_.active()) return Err::NO_ACTIVE_JOB;
    self->pipeline_.cancel();
    if (!w.begin_object() || !w.kv_cstr("job_id", self->job_id_) ||
        !w.kv_cstr("status", "cancelling")) {
        return Err::INTERNAL_ERROR;
    }
    return w.end_object() ? Err::OK : Err::INTERNAL_ERROR;
}

Err PrintService::hook_print_status(void* user, JsonWriter& w) noexcept {
    auto* self = static_cast<PrintService*>(user);
    const auto s = self->pipeline_.snapshot();
    if (self->raw_active_) {
        if (!w.begin_object() || !w.kv_cstr("job_id", self->job_id_) ||
            !w.kv_cstr("status", "printing") || !w.kv_i64("generation", 0) ||
            !w.key("stats") || !w.begin_object() ||
            !w.kv_i64("packets_received", self->raw_chunks_) ||
            !w.kv_i64("packets_decoded", self->raw_chunks_) ||
            !w.kv_i64("bytes_decoded", self->raw_bytes_) ||
            !w.kv_i64("bytes_printed", self->raw_bytes_) ||
            !w.kv_i64("queue_watermark", 0) ||
            !w.kv_i64("queue_watermark_max", 0) ||
            !w.kv_i64("throughput_bps", 0) ||
            !w.end_object() || !w.end_object()) return Err::INTERNAL_ERROR;
        return Err::OK;
    }
    const bool has_job = self->job_id_[0] != '\0';
    if (!w.begin_object()) return Err::INTERNAL_ERROR;
    if (has_job ? !w.kv_cstr("job_id", self->job_id_) : !w.kv_null("job_id")) {
        return Err::INTERNAL_ERROR;
    }
    if (!w.kv_cstr("status", job_state_name(self->pipeline_.state()))) {
        return Err::INTERNAL_ERROR;
    }
    if (!w.kv_i64("generation", self->pipeline_.generation())) return Err::INTERNAL_ERROR;
    if (!w.key("stats") || !w.begin_object()) return Err::INTERNAL_ERROR;
    if (!w.kv_i64("packets_received", s.packets_received) ||
        !w.kv_i64("packets_decoded", s.packets_decoded) ||
        !w.kv_i64("bytes_decoded", s.bytes_decoded) ||
        !w.kv_i64("bytes_printed", s.bytes_printed) ||
        !w.kv_i64("queue_watermark", s.queue_watermark) ||
        !w.kv_i64("queue_watermark_max", s.queue_watermark_max) ||
        !w.kv_i64("throughput_bps", s.throughput_bps) ||
        !w.kv_i64("crc_errors", s.crc_errors) ||
        !w.kv_i64("decode_errors", s.decode_errors)) {
        return Err::INTERNAL_ERROR;
    }
    if (!w.end_object()) return Err::INTERNAL_ERROR;
    return w.end_object() ? Err::OK : Err::INTERNAL_ERROR;
}

Err PrintService::hook_feed_paper(void* user, const FeedPaperParams& p,
                                  JsonWriter& w) noexcept {
    auto* self = static_cast<PrintService*>(user);
    if (self->pipeline_.active() || self->buffered_ || self->raw_active_) {
        return Err::JOB_ALREADY_RUNNING;
    }
    uint32_t moved = 0;
    const Err e = self->stepper_hal_->feed(FeedRequest{p.distance_um, 50000}, &moved);
    if (e != Err::OK) return e;
    if (!w.begin_object() || !w.kv_i64("moved_um", moved)) return Err::INTERNAL_ERROR;
    return w.end_object() ? Err::OK : Err::INTERNAL_ERROR;
}

Err PrintService::hook_cut_paper(void* user, JsonWriter& w) noexcept {
    auto* self = static_cast<PrintService*>(user);
    const Err e = self->cutter_hal_->cut();
    if (e != Err::OK) return e;  // UNSUPPORTED 由能力门控前置拦截
    if (!w.begin_object() || !w.kv_bool("cut", true)) return Err::INTERNAL_ERROR;
    return w.end_object() ? Err::OK : Err::INTERNAL_ERROR;
}

// ---------------------------------------------------------------- 页缓存直通

void PrintService::bfp_hello_cb_(void* user) noexcept {
    IJPD_LOGI(kTag, "buffered: hello ok");
}

// 帧到达:载荷(按需解压)→ 行拼装 → 页缓存(显示同步可见)
void PrintService::bfp_frame_cb_(void* user, const BinFrameParser::Frame& f) noexcept {
    auto* self = static_cast<PrintService*>(user);
    if (!self->buffered_ || !self->buf_receiving_) return;

    // 载荷指针:压缩时解压到 PSRAM 乒乓第二缓冲(直通模式下槽池路径空闲,
    // 绝不能在回调栈上放大缓冲——rx 任务栈仅数 KB,会溢出)
    const uint8_t* payload = f.payload;
    size_t plen = f.compressed_len;
    if (self->buf_cfg_.compression == Compression::Lz4Block &&
        f.compressed_len < f.uncompressed_len) {
        uint8_t* dst = self->mem_->comp[1];
        const int n = lz4_block_decompress(f.payload, f.compressed_len, dst,
                                           cfg::COMPRESSED_BUF_SIZE,
                                           f.uncompressed_len);
        if (n != static_cast<int>(f.uncompressed_len)) {
            IJPD_LOGE(kTag, "buffered: lz4 fail (%d != %u)", n,
                      static_cast<unsigned>(f.uncompressed_len));
            self->buf_cancel_ = true;  // 以失败终态收尾
            return;
        }
        payload = dst;
        plen = f.uncompressed_len;
    }

    const uint32_t bpr = self->buf_cfg_.total_bytes_uncompressed /
                         self->buf_total_rows_;
    // 行拼装:跨包残留保存在 row_carry_(与旧路径互斥使用)
    size_t off = 0;
    while (off < plen) {
        const size_t need = bpr - self->buf_carry_len_;
        const size_t take = (plen - off < need) ? (plen - off) : need;
        std::memcpy(self->row_carry_ + self->buf_carry_len_, payload + off, take);
        self->buf_carry_len_ += static_cast<uint32_t>(take);
        off += take;
        if (self->buf_carry_len_ == bpr) {
            DisplayService::instance().store_row(self->buf_rows_stored_,
                                                 self->row_carry_, bpr);
            self->buf_rows_stored_++;
            self->buf_carry_len_ = 0;
        }
    }
    self->buf_recv_bytes_ += plen;
    self->buf_pkts_++;

    // 接收完成 → 停收相(打印相已在连接时开启,边传边打)
    if (self->buf_recv_bytes_ >= self->buf_cfg_.total_bytes_uncompressed) {
        self->buf_receiving_ = false;
        IJPD_LOGI(kTag, "buffered: rx done (%u pkts, %u rows), print continues",
                  static_cast<unsigned>(self->buf_pkts_),
                  static_cast<unsigned>(self->buf_rows_stored_));
        // 通知 App:传输完毕(打印可能已进行中)
        PrintPipeline::Event ev{};
        ev.kind = PrintPipeline::Event::Kind::Progress;
        ev.state = JobState::Draining;
        ev.stats.packets_received = self->buf_pkts_;
        ev.stats.packets_decoded = self->buf_pkts_;
        ev.stats.bytes_decoded = self->buf_recv_bytes_;
        ev.stats.bytes_printed = 0;  // 打印进度由独立事件持续上报
        self->enqueue_event_(ev);
    }
}

// 打印相(print 任务):从页缓存按喷头节拍出行,行进指纹。
// 边传边打:行到位即打,打印进度可落后于接收进度(喷头慢于网络)。
// 全部行打完且接收已完成 → 终态。
void PrintService::buffered_print_step_() noexcept {
    while (buf_printing_ && !buf_cancel_) {
        if (buf_rows_printed_ >= buf_rows_stored_) {
            // 行未到位:接收中(等更多行)或全部打完(终态)
            if (!buf_receiving_ && buf_rows_printed_ >= buf_total_rows_) {
                finish_buffered_(PrintPipeline::Event::Kind::Completed, Err::OK);
            }
            return;  // 等行(打印任务 2ms 后重试)
        }
        const uint32_t bpr = static_cast<uint32_t>(
            buf_cfg_.total_bytes_uncompressed / buf_total_rows_);
        // 行数据在常驻页缓存:直接从 DisplayService 取
        const uint8_t* row = DisplayService::instance().peek_row(buf_rows_printed_);
        if (row == nullptr) return;
        fp_state_ = crc32_update(fp_state_, row, bpr);
        fp_bytes_ += bpr;
        DisplayService::instance().feed_row(buf_rows_printed_, row, bpr);
        RasterMeta m{buf_rows_printed_};
        const Err e = printhead_hal_->submitRaster(row, bpr, m);
        if (e != Err::OK) {
            finish_buffered_(PrintPipeline::Event::Kind::Failed, Err::HAL_FAULT);
            return;
        }
        buf_rows_printed_++;
        // 打印相进度:每 256 行(约 1.3s @200rps)广播一次
        if ((buf_rows_printed_ & 0xFF) == 0) {
            PrintPipeline::Event ev{};
            ev.kind = PrintPipeline::Event::Kind::Progress;
            ev.state = JobState::Draining;
            ev.stats.packets_received = buf_pkts_;
            ev.stats.packets_decoded = buf_pkts_;
            ev.stats.bytes_decoded = buf_recv_bytes_;
            ev.stats.bytes_printed = buf_rows_printed_ *
                (buf_cfg_.total_bytes_uncompressed / buf_total_rows_);
            enqueue_event_(ev);
        }
        if (buf_rows_printed_ >= buf_total_rows_ && !buf_receiving_) {
            finish_buffered_(PrintPipeline::Event::Kind::Completed, Err::OK);
            return;
        }
    }
}

// 直通任务终态:事件 + 显示定格 + 数据连接关闭
void PrintService::finish_buffered_(PrintPipeline::Event::Kind kind,
                                     Err code) noexcept {
    if (!buffered_) return;
    buf_printing_ = false;
    buf_receiving_ = false;
    DisplayService::instance().end_job();
    if (data_fd_ >= 0 || close_req_) request_close_data_();
    PrintPipeline::Event ev{};
    ev.kind = kind;
    ev.code = code;
    ev.duration_ms = task_layout::now_ms() - buf_started_ms_;
    ev.stats.packets_received = buf_pkts_;
    ev.stats.packets_decoded = buf_pkts_;
    ev.stats.bytes_printed = buf_rows_printed_ *
        (buf_cfg_.total_bytes_uncompressed / buf_total_rows_);
    enqueue_event_(ev);
    IJPD_LOGW(kTag, "buffered terminal: kind=%d code=%s rows=%u/%u",
              static_cast<int>(kind), err_name(code),
              static_cast<unsigned>(buf_rows_printed_),
              static_cast<unsigned>(buf_total_rows_));
    buffered_ = false;
}

// ---------------------------------------------------------------- 数据连接

bool PrintService::data_accept(void* user, int fd, const char* peer) noexcept {
    auto* self = static_cast<PrintService*>(user);
    const bool waiting =
        self->pipeline_.state() == JobState::WaitingDataConn ||
        (self->buffered_ && self->buf_receiving_);
    if (!waiting || self->token_used_ || self->data_fd_ >= 0) {
        return false;  // 无任务等待 / 令牌已用 / 已有连接
    }
    self->data_fd_ = fd;      // rx_task 接管（唯一写者）
    self->close_req_ = false; // 新连接：撤销遗留的关闭请求
    // 边传边打:数据连接一建立即开打印相(喷头按行消费,行到位就打,
    // 传输与打印重叠;喷头速度(200行/s)远慢于网络,传输先完)
    if (self->buffered_) {
        self->buf_printing_ = true;
    }
    IJPD_LOGI(kTag, "data conn taken from %s", peer);
    return true;
}

// 仅 rx_task 调用：真正执行 shutdown/close（单属主，杜绝 double-close
// 与 fd 号被下一连接复用后误关的竞态）。
void PrintService::close_data_socket_() noexcept {
    const int fd = data_fd_;
    data_fd_ = -1;
    close_req_ = false;
    if (fd >= 0) {
        lwip_shutdown(fd, SHUT_RDWR);
        lwip_close(fd);
        IJPD_LOGI(kTag, "data socket closed");
    }
}

// 任意任务可调：请求 rx_task 关闭数据连接（不直接触碰 fd）。
void PrintService::request_close_data_() noexcept { close_req_ = true; }

// ---------------------------------------------------------------- 超时看护

void PrintService::check_timeouts_(uint64_t now) noexcept {
    if (raw_active_) {
        const uint32_t last = last_rx_ms_;
        if (raw_close_req_) return;
        if (now >= last && now - last >= cfg::PACKET_IDLE_TIMEOUT_MS) {
            raw_close_kind_ = static_cast<uint8_t>(PrintPipeline::Event::Kind::Failed);
            raw_close_code_ = Err::JOB_TIMEOUT;
            raw_close_req_ = true;
        } else if (now - raw_started_ms_ >= cfg::JOB_TOTAL_TIMEOUT_MS) {
            raw_close_kind_ = static_cast<uint8_t>(PrintPipeline::Event::Kind::Failed);
            raw_close_code_ = Err::JOB_TIMEOUT;
            raw_close_req_ = true;
        }
        return;
    }
    if (mqtt_transport_ && !buffered_) {
        if (pipeline_.terminal()) return;
        const uint32_t last = last_rx_ms_;
        if (now >= last && now - last >= cfg::PACKET_IDLE_TIMEOUT_MS) {
            pipeline_.fail(Err::JOB_TIMEOUT);
            mqtt_transport_ = false;
        } else if (now - pipeline_.started_ms() >= cfg::JOB_TOTAL_TIMEOUT_MS) {
            pipeline_.fail(Err::JOB_TIMEOUT);
            mqtt_transport_ = false;
        }
        return;
    }
    if (buffered_ && (buf_receiving_ || buf_printing_)) {
        const uint64_t started = buf_started_ms_;
        // 多核读序:last_rx 可能被 rx 任务(另一核)更新到比 now 更晚。
        // 先读 last_rx 再取 now,now < last 视为"刚收到"跳过(不下溢)。
        const uint32_t last = last_rx_ms_;
        if (buf_receiving_ && now >= last &&
            now - last >= cfg::PACKET_IDLE_TIMEOUT_MS) {
            IJPD_LOGW(kTag, "buffered rx idle timeout: now=%llu last_rx=%u "
                      "diff=%llu idle_ms=%u started=%llu",
                      static_cast<unsigned long long>(now),
                      static_cast<unsigned>(last_rx_ms_),
                      static_cast<unsigned long long>(now - last_rx_ms_),
                      static_cast<unsigned>(cfg::PACKET_IDLE_TIMEOUT_MS),
                      static_cast<unsigned long long>(started));
            finish_buffered_(PrintPipeline::Event::Kind::Failed, Err::JOB_TIMEOUT);
            token_used_ = true;
            return;
        }
        if (now - started >= cfg::JOB_TOTAL_TIMEOUT_MS) {
            IJPD_LOGW(kTag, "buffered total timeout");
            finish_buffered_(PrintPipeline::Event::Kind::Failed, Err::JOB_TIMEOUT);
            token_used_ = true;
            return;
        }
        return;  // 直通任务不走旧流水线超时分支
    }
    const JobState st = pipeline_.state();
    const uint32_t now32 = static_cast<uint32_t>(now);
    if (st == JobState::WaitingDataConn &&
        now - pipeline_.started_ms() >= cfg::DATA_CONNECT_TIMEOUT_MS) {
        IJPD_LOGW(kTag, "data connect timeout");
        pipeline_.fail(Err::JOB_TIMEOUT);
    } else if (st == JobState::Receiving &&
               now32 - last_rx_ms_ >= static_cast<uint32_t>(cfg::PACKET_IDLE_TIMEOUT_MS)) {
        IJPD_LOGW(kTag, "packet idle timeout");
        pipeline_.fail(Err::JOB_TIMEOUT);
        request_close_data_();
    } else if (pipeline_.active() &&
               now - pipeline_.started_ms() >= cfg::JOB_TOTAL_TIMEOUT_MS) {
        IJPD_LOGW(kTag, "job total timeout");
        pipeline_.fail(Err::JOB_TIMEOUT);
        request_close_data_();
    }
    if (pipeline_.terminal() && (data_fd_ >= 0 || close_req_)) {
        request_close_data_();  // rx_task 执行实际关闭
        token_used_ = true;
    }
}

void PrintService::tick() noexcept {
    if (!running_) return;
    check_timeouts_(task_layout::now_ms());
}

// ---------------------------------------------------------------- 任务

void PrintService::rx_task_(void* arg) noexcept {
    auto* self = static_cast<PrintService*>(arg);
    uint8_t buf[1024];
    while (self->running_) {
        // 关闭请求优先处理（本任务是 fd 唯一属主；fd<0 时仅清除标志）
        if (self->close_req_) {
            self->close_data_socket_();
        }
        if (self->raw_close_req_ && self->raw_active_) {
            self->finish_raw_(static_cast<PrintPipeline::Event::Kind>(self->raw_close_kind_),
                              self->raw_close_code_);
            continue;
        }
        if (self->raw_active_) {
            const int raw_fd = self->raw_fd_;
            if (raw_fd < 0) {
                self->finish_raw_(PrintPipeline::Event::Kind::Failed,
                                  Err::DATA_CONNECTION_LOST);
                continue;
            }
            const ssize_t n = lwip_recv(raw_fd, buf, sizeof(buf), MSG_DONTWAIT);
            if (n > 0) {
                self->last_rx_ms_ = static_cast<uint32_t>(task_layout::now_ms());
        const Err e = self->printhead_hal_->submitRaw(buf, static_cast<size_t>(n));
                if (e != Err::OK) {
                    self->finish_raw_(PrintPipeline::Event::Kind::Failed, Err::HAL_FAULT);
                } else {
                    self->fp_state_ = crc32_update(self->fp_state_, buf, static_cast<size_t>(n));
                    self->fp_bytes_ += static_cast<uint32_t>(n);
                    self->raw_chunks_++;
                    self->raw_bytes_ += static_cast<size_t>(n);
                }
            } else if (n == 0) {
                self->finish_raw_(PrintPipeline::Event::Kind::Completed, Err::OK);
            } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                self->finish_raw_(PrintPipeline::Event::Kind::Failed,
                                  Err::DATA_CONNECTION_LOST);
            } else {
                vTaskDelay(pdMS_TO_TICKS(2));
            }
            continue;
        }
        const int fd = self->data_fd_;
        // 直通模式下流水线未启动,读取条件须显式包含 buffered_
        if (fd < 0 || (!self->pipeline_.active() && !self->buffered_ && !self->raw_active_)) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        // 高水位背压仅适用于槽池流水线;直通模式页缓存即收即落,无背压
        if (!self->buffered_ && !self->pipeline_.rx_should_read()) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        const ssize_t n = lwip_recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
        if (n > 0) {
            self->last_rx_ms_ = static_cast<uint32_t>(task_layout::now_ms());
            if (self->buffered_) {
                // 页缓存直通:帧校验(Hello/CRC/顺序/长度)+行落缓存,无背压。
                // 粘包协议要求:每帧派发后重挂载荷缓冲再继续喂
                //(与流水线 rx_pump 同一模式,否则第二帧起 NeedBuffer 丢流)。
                BinFrameParser::Events bev{self, bfp_hello_cb_, bfp_frame_cb_};
                size_t off = 0;
                while (off < static_cast<size_t>(n)) {
                    self->bfp_rx_.set_payload_buffer(
                        self->mem_->comp[0], cfg::MAX_PACKET_COMPRESSED_SIZE + 1);
                    const auto r = self->bfp_rx_.feed(
                        buf + off, static_cast<size_t>(n) - off, bev);
                    if (r.status == BinFrameParser::FeedStatus::Failed) {
                        IJPD_LOGE(kTag, "buffered feed failed: %s",
                                  err_name(r.error));
                        self->buf_cancel_ = true;
                        break;
                    }
                    if (r.status == BinFrameParser::FeedStatus::NeedBuffer) {
                        off += r.consumed;
                        continue;  // 缓冲已在循环头重挂
                    }
                    break;  // Ok:本块全部消费
                }
            } else {
                size_t off = 0;
                while (off < static_cast<size_t>(n)) {
                    size_t consumed = 0;
                    const auto r = self->pipeline_.rx_pump(buf + off,
                                                           static_cast<size_t>(n) - off,
                                                           &consumed);
                    off += consumed;
                    if (r == PrintPipeline::Pump::Failed) {
                        self->close_data_socket_();
                        break;
                    }
                    if (r == PrintPipeline::Pump::WouldBlock) {
                        vTaskDelay(pdMS_TO_TICKS(2));  // 等待压缩缓冲（TCP 背压）
                    }
                }
            }
        } else if (n == 0) {
            // 对端关闭：活动任务 -> DATA_CONNECTION_LOST（协议 V1）
            if (self->pipeline_.active()) {
                IJPD_LOGW(kTag, "data conn lost");
                self->pipeline_.fail(Err::DATA_CONNECTION_LOST);
            }
            self->close_data_socket_();
        } else {
            const int err = errno;
            if (err != EAGAIN && err != EWOULDBLOCK) {
                if (self->pipeline_.active()) {
                    self->pipeline_.fail(Err::DATA_CONNECTION_LOST);
                }
                self->close_data_socket_();
            } else {
                vTaskDelay(pdMS_TO_TICKS(2));
            }
        }
    }
    vTaskDelete(nullptr);
}

void PrintService::decode_task_(void* arg) noexcept {
    auto* self = static_cast<PrintService*>(arg);
    while (self->running_) {
        const auto r = self->pipeline_.decode_pump();
        if (r == PrintPipeline::Pump::WouldBlock) {
            vTaskDelay(pdMS_TO_TICKS(2));
        } else if (r == PrintPipeline::Pump::Failed) {
            vTaskDelay(pdMS_TO_TICKS(50));  // 已失败：低速待命
        }
    }
    vTaskDelete(nullptr);
}

void PrintService::print_task_(void* arg) noexcept {
    auto* self = static_cast<PrintService*>(arg);
    while (self->running_) {
        if (self->buf_printing_) {
            self->buffered_print_step_();
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        const auto r = self->pipeline_.print_pump();
        if (r == PrintPipeline::Pump::Done) {
            // FINALIZING：切纸评估（默认不支持 -> 警告完成）
            const bool want_cut = self->cutter_after_finish_;
            const bool can_cut = self->cutter_hal_->supported();
            Err warn = Err::OK;
            bool warning = false;
            if (want_cut && can_cut) {
                const Err ce = self->cutter_hal_->cut();
                if (ce != Err::OK) {
                    warning = true;
                    warn = ce;
                }
            } else if (want_cut && !can_cut) {
                warning = true;
                warn = Err::UNSUPPORTED;
            }
            (void)self->pipeline_.finish(warning, warn);
            vTaskDelay(pdMS_TO_TICKS(10));
        } else if (r == PrintPipeline::Pump::WouldBlock) {
            vTaskDelay(pdMS_TO_TICKS(2));
        } else if (r == PrintPipeline::Pump::Failed) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
    vTaskDelete(nullptr);
}

void PrintService::report_task_(void* arg) noexcept {
    auto* self = static_cast<PrintService*>(arg);
    static char evbuf[cfg::MAX_RESPONSE_FRAME_SIZE];
    EventItem item;
    while (self->running_) {
        if (xQueueReceive(static_cast<QueueHandle_t>(self->event_queue_), &item,
                          pdMS_TO_TICKS(100)) == pdTRUE) {
            size_t len = 0;
            if (self->serialize_event_(item.ev, evbuf, sizeof(evbuf), &len)) {
                self->plane_->broadcast(reinterpret_cast<const uint8_t*>(evbuf), len);
            } else {
                IJPD_LOGW(kTag, "event serialize failed");
            }
        }
        self->pipeline_.tick(task_layout::now_ms());  // 进度节流
    }
    vTaskDelete(nullptr);
}

}  // namespace ijpd
