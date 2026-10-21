// ============================================================================
// control_plane.h —— 控制面汇入层（app 层，Phase 5）
//
// 职责：把多个控制通道（TCP×2 + BLE）的字节流统一接入 FrameSplitter →
// CommandRouter，并把响应经各通道 Sink 发回。
//   - 每通道独立拆帧缓冲（半包/粘包/BLE 分片重组）；
//   - 帧重组超时（CTRL_FRAME_TIMEOUT_MS）：tick() 周期检查，丢弃半帧并回
//     FRAME_TIMEOUT（msg_id=null）；
//   - 超长帧：丢弃并逐帧回 FRAME_TOO_LARGE；
//   - 帧正文经暂存缓冲复制后交给 Router（Router 就地改写输入）。
//
// 线程安全性：非线程安全。全部调用必须来自同一线程（Phase 5 为 ctrl_task，
// 即 ControlTcpServer 的复用循环；BLE 字节经队列转入该线程）。
// 所有权：    Router 由外部拥有；Sink 为回调无所有权。
// 最大耗时：  feed O(len) + 路由 O(帧长)。
// ============================================================================
#ifndef IJPD_APP_CONTROL_PLANE_H
#define IJPD_APP_CONTROL_PLANE_H

#include <cstddef>
#include <cstdint>

#include "core/command_router.h"
#include "core/config_limits.h"
#include "core/errors.h"
#include "core/frame_splitter.h"

namespace ijpd {

class ControlPlane {
public:
    static constexpr int kMaxChannels = 3;   // TCP conn0 / TCP conn1 / BLE
    static constexpr int kChannelBle = 2;

    using SendFn = Err (*)(void* user, const uint8_t* data, size_t len);
    struct Sink {
        void* user = nullptr;
        SendFn send = nullptr;  // 返回 OK 视为送达；失败由调用方记录（不重试）
    };

    ControlPlane() noexcept = default;

    Err init(CommandRouter* router) noexcept;

    void set_sink(int ch, const Sink& sink) noexcept;

    // 通道断开：清空拆帧缓冲与 Sink（连接关闭时调用）
    void detach(int ch) noexcept;

    // 广播：发送到所有已注册 Sink（事件通道；失败仅计数不重试）
    void broadcast(const uint8_t* data, size_t len) noexcept;

    // 通道字节输入（任意切块）。now_ms 为当前毫秒时间戳（平台注入）。
    void feed(int ch, const uint8_t* data, size_t len, uint64_t now_ms) noexcept;

    // 周期调用（建议 100ms）：帧重组超时检查
    void tick(uint64_t now_ms) noexcept;

    // 诊断：通道是否存在半帧
    bool has_partial(int ch) const noexcept;

private:
    struct FrameCtx {
        ControlPlane* self;
        int ch;
    };

    void on_frame_(int ch, const char* frame, size_t len) noexcept;
    void respond_(int ch, Err code, const char* message) noexcept;

    CommandRouter* router_ = nullptr;
    Sink sinks_[kMaxChannels] = {};
    FrameSplitter splitters_[kMaxChannels];
    uint64_t last_rx_ms_[kMaxChannels] = {0, 0, 0};
    char frame_buf_[cfg::MAX_CONTROL_FRAME_SIZE];  // Router 输入暂存（Router 就地改写）
    char resp_buf_[cfg::MAX_RESPONSE_FRAME_SIZE];  // 响应暂存
};

}  // namespace ijpd

#endif  // IJPD_APP_CONTROL_PLANE_H
