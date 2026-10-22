// ============================================================================
// print_service.h —— 设备侧打印编排（Phase 6）
//
// 组装 PrintPipeline（core）+ Mock HAL（hal）+ FreeRTOS 任务（任务布局见
// memory_budget §4）+ 事件序列化（report_task）+ 超时看护。
//
// 任务：
//   rx_task     Core0 prio10 读数据 socket -> pipeline.rx_pump
//   decode_task Core1 prio12 pipeline.decode_pump
//   print_task  Core1 prio14 pipeline.print_pump -> Mock 喷头 -> finish
//   report_task Core0 prio3  事件队列 -> JSON -> ControlPlane 广播
// 超时（ctrl 空闲钩子驱动，100ms 周期）：
//   WAITING_DATA_CONN > DATA_CONNECT_TIMEOUT_MS -> JOB_TIMEOUT
//   RECEIVING 包间空闲 > PACKET_IDLE_TIMEOUT_MS  -> JOB_TIMEOUT
//   任务总时长 > JOB_TOTAL_TIMEOUT_MS            -> JOB_TIMEOUT
// ============================================================================
#ifndef IJPD_APP_PRINT_SERVICE_H
#define IJPD_APP_PRINT_SERVICE_H

#include <cstdint>

#include "app/control_plane.h"
#include "core/bin_frame_parser.h"
#include "core/command_router.h"
#include "core/crc32.h"
#include "core/mqtt_chunk_protocol.h"
#include "core/pwg_raster.h"
#include "core/print_pipeline.h"
#include "printer_hal/cutter_hal.h"
#include "printer_hal/printhead_hal.h"
#include "printer_hal/stepper_hal.h"
#include "platform_esp/psram_allocator.h"

namespace ijpd {

class PrintService {
public:
    static PrintService& instance() noexcept;

    // 创建任务与常驻资源（PSRAM 行缓冲等）
    Err start(PlatformMemory* mem, ControlPlane* plane) noexcept;

    // ctrl 空闲钩子（100ms 周期）：超时看护 + 数据连接清理
    void tick() noexcept;

    // 数据 TCP 接受策略（ControlTcpServer 空闲时调用? -> DataTcpServer 回调）
    static bool data_accept(void* user, int fd, const char* peer) noexcept;
    static bool raw_accept(void* user, int fd, const char* peer) noexcept;
    static void mqtt_chunk(void* user, const uint8_t* data, size_t len) noexcept;
    static void mqtt_disconnected(void* user) noexcept;
    static Err ipp_begin_page(void* user, const PwgRasterPage& page) noexcept;
    static Err ipp_write_row(void* user, const uint8_t* row, size_t len,
                             uint32_t row_index) noexcept;
    static Err ipp_end_page(void* user, Err status) noexcept;

    void set_mqtt_context(const char* base_topic, const char* device_id) noexcept;

    // Router 钩子
    static Err hook_start_print(void* user, const StartPrintParams& p,
                                JsonWriter& w) noexcept;
    static Err hook_cancel_print(void* user, const CancelPrintParams& p,
                                 JsonWriter& w) noexcept;
    static Err hook_print_status(void* user, JsonWriter& w) noexcept;
    static Err hook_feed_paper(void* user, const FeedPaperParams& p,
                               JsonWriter& w) noexcept;
    static Err hook_cut_paper(void* user, JsonWriter& w) noexcept;

private:
    PrintService() noexcept = default;

    // Os 适配
    static uint64_t os_now_ms(void* user) noexcept;
    static Err os_print_row(void* user, const uint8_t* row, size_t len,
                            uint32_t row_index) noexcept;
    static void os_on_event(void* user, const PrintPipeline::Event& ev) noexcept;
    static void mock_delay(void* user, uint32_t ms) noexcept;

    void enqueue_event_(const PrintPipeline::Event& ev) noexcept;
    static void bfp_hello_cb_(void* user) noexcept;
    static void bfp_frame_cb_(void* user, const BinFrameParser::Frame& f) noexcept;
    void buffered_print_step_() noexcept;   // 打印相:缓存出行(print 任务)
    void finish_buffered_(PrintPipeline::Event::Kind kind, Err code) noexcept;
    void close_data_socket_() noexcept;
    void close_raw_socket_() noexcept;
    void finish_raw_(PrintPipeline::Event::Kind kind, Err code) noexcept;
    void accept_mqtt_chunk_(const uint8_t* data, size_t len) noexcept;
    void fail_mqtt_job_(Err code) noexcept;
    void request_close_data_() noexcept;
    void check_timeouts_(uint64_t now) noexcept;
    bool serialize_event_(const PrintPipeline::Event& ev, char* buf, size_t cap,
                          size_t* len) noexcept;

    static void rx_task_(void* arg) noexcept;
    static void decode_task_(void* arg) noexcept;
    static void print_task_(void* arg) noexcept;
    static void report_task_(void* arg) noexcept;

    PrintPipeline pipeline_;
    MockPrintheadHal printhead_{&mock_delay, nullptr};
    MockStepperHal stepper_{&mock_delay, nullptr};
    MockCutterHal cutter_{};

    PlatformMemory* mem_ = nullptr;
    ControlPlane* plane_ = nullptr;
    uint8_t* row_carry_ = nullptr;  // PSRAM，start 时分配

    // 数据 socket 单属主约定（防并发 double-close / fd 复用后误操作导致崩溃）：
    //   data_fd_  仅 dat_tcp 任务（data_accept）写、rx 任务读；
    //   fd 的 shutdown/close 仅由 rx_task 执行；
    //   其他任务（ctrl 侧 tick/超时/终态清理）只置 close_req_ 请求标志。
    volatile int data_fd_ = -1;
    volatile bool token_used_ = false;
    volatile bool close_req_ = false;
    // uint32 毫秒（回绕安全差值比较；rx 与 ctrl 跨任务访问）
    volatile uint32_t last_rx_ms_ = 0;
    volatile int raw_fd_ = -1;
    volatile bool raw_active_ = false;
    volatile bool raw_close_req_ = false;
    volatile uint8_t raw_close_kind_ =
        static_cast<uint8_t>(PrintPipeline::Event::Kind::Failed);
    volatile Err raw_close_code_ = Err::DATA_CONNECTION_LOST;
    volatile uint32_t raw_chunks_ = 0;
    volatile uint64_t raw_bytes_ = 0;
    uint64_t raw_started_ms_ = 0;
    bool mqtt_transport_ = false;
    uint32_t mqtt_next_chunk_ = 0;
    uint32_t mqtt_total_chunks_ = 0;
    MqttChunkSequence mqtt_sequence_{};
    char mqtt_base_topic_[128] = {};
    char mqtt_device_id_[32] = {};
    void* ipp_done_sem_ = nullptr;  // SemaphoreHandle_t
    volatile bool ipp_active_ = false;
    volatile Err ipp_result_ = Err::OK;
    // 图片指纹（端到端校验）：print 任务写，report 任务读
    volatile uint32_t fp_state_ = CRC32_INIT_STATE;
    volatile uint32_t fp_bytes_ = 0;

    // ---- 页缓存直通模式(整页缓冲,接收全速/打印定速/边收边显) ----
    bool buffered_ = false;
    BinFrameParser bfp_rx_;             // 直通模式帧解析器
    volatile bool buf_receiving_ = false;
    volatile bool buf_printing_ = false;
    volatile bool buf_cancel_ = false;
    BinFrameParser::JobConfig buf_cfg_{};
    uint32_t buf_carry_len_ = 0;
    volatile uint32_t buf_rows_stored_ = 0;
    volatile uint32_t buf_rows_printed_ = 0;
    uint32_t buf_total_rows_ = 0;
    volatile uint64_t buf_recv_bytes_ = 0;
    volatile uint32_t buf_pkts_ = 0;
    uint64_t buf_started_ms_ = 0;
    char job_id_[cfg::JOB_ID_MAX + 1] = {0};
    uint32_t data_token_ = 0;
    bool cutter_after_finish_ = false;
    volatile uint32_t event_seq_ = 0;
    void* event_queue_ = nullptr;  // QueueHandle_t
    volatile bool running_ = false;
};

}  // namespace ijpd

#endif  // IJPD_APP_PRINT_SERVICE_H
