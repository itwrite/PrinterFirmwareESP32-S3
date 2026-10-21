// control_tcp_server.cpp —— select 复用控制服务器（ctrl_task）
#include "platform_esp/control_tcp_server.h"

#include <fcntl.h>
#include <sys/select.h>
#include <unistd.h>

#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include "platform_esp/log_adapter.h"
#include "platform_esp/task_layout.h"

namespace ijpd {
namespace {
constexpr const char* kTag = "ctl_tcp";
// EAGAIN 重试上限(每次 1ms,共 3s):慢速/拥塞 Wi-Fi(RTT 可达 200ms+)下
// TCP 发送缓冲排空需要数个 RTT,200ms 会在高峰期误放弃。
constexpr int kSendAttempts = 3000;
}  // namespace

ControlTcpServer::~ControlTcpServer() { stop(); }

void ControlTcpServer::close_channel_(int ch) noexcept {
    if (ch < 0 || ch >= static_cast<int>(sizeof(fds_) / sizeof(fds_[0]))) return;
    // 与 send_ 互斥:毒化断连(report 任务)与 select 收包错误(ctrl 任务)
    // 可能并发触发;持锁保证 fd 只被关闭一次(双重 lwip_close 会破坏 lwip
    // socket 表,且 fd 复用后可能误杀新连接)。
    const SemaphoreHandle_t lock = static_cast<SemaphoreHandle_t>(tx_lock_);
    if (lock != nullptr) xSemaphoreTake(lock, portMAX_DELAY);
    if (fds_[ch] >= 0) {
        const int fd = fds_[ch];
        fds_[ch] = -1;
        lwip_close(fd);
        if (host_.on_channel_closed != nullptr) host_.on_channel_closed(host_.user, ch);
        IJPD_LOGI(kTag, "conn ch%d closed", ch);
    }
    if (lock != nullptr) xSemaphoreGive(lock);
}

Err ControlTcpServer::start(uint16_t port, const Host& host) noexcept {
    if (host.feed == nullptr || host.register_sink == nullptr) {
        return Err::INVALID_ARGUMENT;
    }
    host_ = host;

    tx_lock_ = xSemaphoreCreateMutex();
    if (tx_lock_ == nullptr) return Err::OUT_OF_MEMORY;

    listener_ = lwip_socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (listener_ < 0) return Err::OUT_OF_MEMORY;

    int opt = 1;
    lwip_setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (lwip_bind(listener_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        IJPD_LOGE(kTag, "bind %u failed", port);
        lwip_close(listener_);
        listener_ = -1;
        return Err::INTERNAL_ERROR;
    }
    if (lwip_listen(listener_, 2) != 0) {
        lwip_close(listener_);
        listener_ = -1;
        return Err::INTERNAL_ERROR;
    }

    running_ = true;
    const Err e = task_layout::spawn(
        "ctl_tcp", [](void* arg) { static_cast<ControlTcpServer*>(arg)->run_(); },
        this, 0, task_layout::kPrioCtrl, task_layout::kStackCtrl);
    if (e != Err::OK) {
        running_ = false;
        lwip_close(listener_);
        listener_ = -1;
        return e;
    }
    IJPD_LOGI(kTag, "listening on %u (max %d conns)", port,
              static_cast<int>(sizeof(fds_) / sizeof(fds_[0])));
    return Err::OK;
}

void ControlTcpServer::stop() noexcept {
    running_ = false;
    if (listener_ >= 0) {
        lwip_close(listener_);
        listener_ = -1;
    }
    const int n = static_cast<int>(sizeof(fds_) / sizeof(fds_[0]));
    for (int ch = 0; ch < n; ++ch) close_channel_(ch);
    if (tx_lock_ != nullptr) {
        vSemaphoreDelete(static_cast<SemaphoreHandle_t>(tx_lock_));
        tx_lock_ = nullptr;
    }
}

void ControlTcpServer::run_() noexcept {
    // Sink:发送回调(ctrl 任务上下文,读当前 fd)。
    // 注意:注册必须在每次 accept 后进行——通道关闭时 ControlPlane.detach()
    // 会清掉该通道的 sink,若只在启动时注册一次,客户端重连后响应/事件
    // 将被静默丢弃(只收不发)。
    struct SinkCtx {
        ControlTcpServer* self;
        int ch;
    };
    static SinkCtx sink_ctx[kNumConns] = {{this, 0}, {this, 1}};
    auto register_sink = [&](int ch) {
        sink_ctx[ch] = SinkCtx{this, ch};
        host_.register_sink(host_.user, ch, &sink_ctx[ch],
                            [](void* user, const uint8_t* data, size_t len) -> Err {
                                auto* c = static_cast<SinkCtx*>(user);
                                return c->self->send_(c->ch, data, len);
                            });
    };

    while (running_) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(listener_, &rfds);
        int maxfd = listener_;
        for (int ch = 0; ch < kNumConns; ++ch) {
            if (fds_[ch] >= 0) {
                FD_SET(fds_[ch], &rfds);
                if (fds_[ch] > maxfd) maxfd = fds_[ch];
            }
        }
        timeval tv{};
        tv.tv_sec = 0;
        tv.tv_usec = kSelectTimeoutMs * 1000;

        // 空闲看护:300s 无任何收发活动的连接视为僵尸(半开),踢掉释放槽位
        for (int ch = 0; ch < kNumConns; ++ch) {
            if (fds_[ch] >= 0 &&
                task_layout::now_ms() - last_active_ms_[ch] > 300 * 1000) {
                IJPD_LOGW(kTag, "conn ch%d idle >300s, kicking zombie", ch);
                close_channel_(ch);
            }
        }

        const int nready = lwip_select(maxfd + 1, &rfds, nullptr, nullptr, &tv);
        if (!running_) break;
        if (nready < 0) {
            IJPD_LOGW(kTag, "select error");
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        // 新连接
        if (nready > 0 && FD_ISSET(listener_, &rfds)) {
            sockaddr_in peer{};
            socklen_t plen = sizeof(peer);
            const int fd = lwip_accept(listener_, reinterpret_cast<sockaddr*>(&peer),
                                       &plen);
            if (fd >= 0) {
                int slot = -1;
                for (int ch = 0; ch < kNumConns; ++ch) {
                    if (fds_[ch] < 0) {
                        slot = ch;
                        break;
                    }
                }
                if (slot < 0) {
                    lwip_close(fd);  // 超出连接上限：立即关闭
                    IJPD_LOGW(kTag, "too many conns, refused");
                } else {
                    const int flags = fcntl(fd, F_GETFL, 0);
                    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
                    // TCP keepalive:半开连接(对端断网/杀 App 无 FIN)探测
                    int keep = 1;
                    lwip_setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &keep,
                                    sizeof(keep));
                    fds_[slot] = fd;
                    last_active_ms_[slot] = task_layout::now_ms();
                    register_sink(slot);  // 重连后恢复 sink(关闭时被 detach 清除)
                    IJPD_LOGI(kTag, "conn ch%d from %s", slot,
                              inet_ntoa(peer.sin_addr));
                }
            }
        }

        // 数据
        for (int ch = 0; ch < kNumConns; ++ch) {
            const int fd = fds_[ch];
            if (fd < 0 || !FD_ISSET(fd, &rfds)) continue;
            const ssize_t n = lwip_recv(fd, rx_, sizeof(rx_), MSG_DONTWAIT);
            if (n > 0) {
                last_active_ms_[ch] = task_layout::now_ms();
                host_.feed(host_.user, ch, rx_, static_cast<size_t>(n));
            } else if (n == 0) {
                close_channel_(ch);
            } else {
                const int err = errno;
                if (err != EAGAIN && err != EWOULDBLOCK) {
                    close_channel_(ch);
                }
            }
        }

        // 空闲钩子（BLE 队列 + 帧超时）
        if (host_.tick != nullptr) host_.tick(host_.user);
    }

    // 退出前注销 sink
    for (int ch = 0; ch < kNumConns; ++ch) {
        host_.register_sink(host_.user, ch, nullptr, nullptr);
    }
    IJPD_LOGI(kTag, "server stopped");
}

Err ControlTcpServer::send_(int ch, const uint8_t* data, size_t len) {
    if (ch < 0 || ch >= static_cast<int>(sizeof(fds_) / sizeof(fds_[0]))) {
        return Err::INVALID_ARGUMENT;
    }
    // 串行化跨任务写:命令响应(ctrl 任务)与事件广播(report 任务)可能并发
    // 写同一 socket;lwip_send 部分写+EAGAIN 重试会让两帧在 TCP 流中交错,
    // 客户端收到损坏 JSON。整帧发送期间持锁。
    const SemaphoreHandle_t lock = static_cast<SemaphoreHandle_t>(tx_lock_);
    if (lock != nullptr) xSemaphoreTake(lock, portMAX_DELAY);
    const int fd = fds_[ch];
    if (fd < 0) {
        if (lock != nullptr) xSemaphoreGive(lock);
        return Err::DATA_CONNECTION_LOST;
    }
    size_t sent = 0;
    int attempts = 0;
    while (sent < len) {
        const ssize_t n = lwip_send(fd, data + sent, len - sent, MSG_DONTWAIT);
        if (n > 0) {
            sent += static_cast<size_t>(n);
            attempts = 0;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOMEM ||
                      errno == ENOBUFS)) {
            if (++attempts > kSendAttempts) {
                IJPD_LOGW(kTag, "send ch%d stalled, %u/%u bytes", ch,
                          static_cast<unsigned>(sent), static_cast<unsigned>(len));
                if (lock != nullptr) xSemaphoreGive(lock);
                if (sent > 0) {
                    // 半帧已写入 socket:字节流从此损坏(后续帧会拼接在残帧后),
                    // 唯一正确处置是断开该通道,客户端重连后重新同步。
                    close_channel_(ch);
                    return Err::DATA_CONNECTION_LOST;
                }
                // 零字节未发:流仍完好,仅丢弃本帧(协议不重传控制消息)。
                return Err::QUEUE_TIMEOUT;
            }
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        IJPD_LOGW(kTag, "send ch%d failed", ch);
        if (lock != nullptr) xSemaphoreGive(lock);
        close_channel_(ch);
        return Err::DATA_CONNECTION_LOST;
    }
    if (lock != nullptr) xSemaphoreGive(lock);
    return Err::OK;
}

}  // namespace ijpd
