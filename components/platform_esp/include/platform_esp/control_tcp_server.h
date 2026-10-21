// ============================================================================
// control_tcp_server.h —— 控制通道 TCP 服务器（Phase 5，端口 9100）
//
// 单任务 select 复用循环（= ctrl_task，任务布局见 memory_budget §4）：
//   - 监听 + 最多 cfg::MAX_CTRL_TCP_CONNS 个连接（超出立即关闭新连接）；
//   - recv → Host.feed(channel, ...)（ControlPlane 拆帧路由）；
//   - 响应经 Host.register_sink 注册的 sink 发回（同任务发送，无并发写）；
//   - select 空闲（100ms）→ Host.tick()（BLE 队列排空 + 帧超时检查）。
//
// 与 app 层通过 Host 回调交互，避免组件反向依赖。
// 线程安全性：start/stop 与任务通信经 running_ 标志 + 任务句柄。
// ============================================================================
#ifndef IJPD_PLATFORM_CONTROL_TCP_SERVER_H
#define IJPD_PLATFORM_CONTROL_TCP_SERVER_H

#include <cstdint>

#include "core/config_limits.h"
#include "core/errors.h"

namespace ijpd {

class ControlTcpServer {
public:
    // 通道号：0..MAX_CTRL_TCP_CONNS-1 对应连接槽位
    using SendFn = Err (*)(void* user, const uint8_t* data, size_t len);

    struct Host {
        void* user = nullptr;
        void (*feed)(void* user, int channel, const uint8_t* data, size_t len) = nullptr;
        void (*tick)(void* user) = nullptr;  // select 空闲（约 100ms 周期）
        void (*on_channel_closed)(void* user, int channel) = nullptr;
        void (*register_sink)(void* user, int channel, void* sink_user,
                              SendFn send) = nullptr;
    };

    Err start(uint16_t port, const Host& host) noexcept;
    void stop() noexcept;

    ControlTcpServer() noexcept = default;
    ~ControlTcpServer();

private:
    void run_() noexcept;
    void close_channel_(int ch) noexcept;
    Err send_(int ch, const uint8_t* data, size_t len) noexcept;

    static constexpr int kNumConns = static_cast<int>(cfg::MAX_CTRL_TCP_CONNS);
    static_assert(cfg::MAX_CTRL_TCP_CONNS == 2, "sink ctx 布局依赖 2 连接");
    static constexpr int kSelectTimeoutMs = 100;

    int listener_ = -1;
    int fds_[kNumConns] = {-1, -1};
    uint64_t last_active_ms_[kNumConns] = {0, 0};  // 空闲看护:踢僵尸连接
    Host host_{};
    volatile bool running_ = false;
    void* task_ = nullptr;      // TaskHandle_t(避免头文件依赖)
    void* tx_lock_ = nullptr;   // SemaphoreHandle_t:串行化跨任务同 socket 写
    uint8_t rx_[2048];
};

}  // namespace ijpd

#endif  // IJPD_PLATFORM_CONTROL_TCP_SERVER_H
