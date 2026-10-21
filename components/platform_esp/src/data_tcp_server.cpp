// data_tcp_server.cpp —— 数据通道接受循环
#include "platform_esp/data_tcp_server.h"

#include <cstring>
#include <sys/select.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include "platform_esp/log_adapter.h"
#include "platform_esp/task_layout.h"

namespace ijpd {
namespace {
constexpr const char* kTag = "dat_tcp";
}  // namespace

DataTcpServer::~DataTcpServer() { stop(); }

Err DataTcpServer::start(uint16_t port, AcceptPolicy policy, void* user) noexcept {
    if (policy == nullptr) return Err::INVALID_ARGUMENT;
    policy_ = policy;
    user_ = user;

    listener_ = lwip_socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (listener_ < 0) return Err::OUT_OF_MEMORY;
    int opt = 1;
    lwip_setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (lwip_bind(listener_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        lwip_listen(listener_, 1) != 0) {
        lwip_close(listener_);
        listener_ = -1;
        return Err::INTERNAL_ERROR;
    }

    running_ = true;
    const Err e = task_layout::spawn(
        "dat_tcp", [](void* arg) { static_cast<DataTcpServer*>(arg)->run_(); }, this,
        0, task_layout::kPrioCtrl, 6144);
    if (e != Err::OK) {
        running_ = false;
        lwip_close(listener_);
        listener_ = -1;
        return e;
    }
    IJPD_LOGI(kTag, "listening on %u", port);
    return Err::OK;
}

void DataTcpServer::stop() noexcept {
    running_ = false;
    if (listener_ >= 0) {
        lwip_close(listener_);
        listener_ = -1;
    }
}

void DataTcpServer::run_() noexcept {
    while (running_) {
        // select 超时前置：避免无限期驻留在 lwip_accept 的阻塞路径
        // (该路径在内核队列锁上的自旋曾触发 CPU0 中断看门狗)
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(listener_, &rfds);
        timeval tv{};
        tv.tv_sec = 0;
        tv.tv_usec = 100 * 1000;  // 100ms
        const int nready = lwip_select(listener_ + 1, &rfds, nullptr, nullptr, &tv);
        if (!running_) break;
        if (nready <= 0) continue;
        if (!FD_ISSET(listener_, &rfds)) continue;

        sockaddr_in peer{};
        socklen_t plen = sizeof(peer);
        const int fd = lwip_accept(listener_, reinterpret_cast<sockaddr*>(&peer), &plen);
        if (!running_) {
            if (fd >= 0) lwip_close(fd);
            break;
        }
        if (fd < 0) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        char peer_buf[24] = {0};
        strncpy(peer_buf, inet_ntoa(peer.sin_addr), sizeof(peer_buf) - 1);
        if (policy_(user_, fd, peer_buf)) {
            // 连接被任务接管：本服务器不再监听该 fd（单活动连接约定）
            IJPD_LOGI(kTag, "conn accepted from %s", peer_buf);
        } else {
            IJPD_LOGW(kTag, "conn rejected (no job waiting) from %s", peer_buf);
            lwip_shutdown(fd, SHUT_RDWR);
            lwip_close(fd);
        }
    }
    IJPD_LOGI(kTag, "server stopped");
}

}  // namespace ijpd
