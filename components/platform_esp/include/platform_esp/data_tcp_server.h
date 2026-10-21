// ============================================================================
// data_tcp_server.h —— 数据通道 TCP 服务器（Phase 5，端口 9101）
//
// 单连接：仅当有活动打印任务等待数据连接时接受（AcceptPolicy 返回 true，
// Phase 6 由 rx 侧接管 socket）；否则立即关闭并记录。
// Phase 5：无任务管理，policy 恒为拒绝（连接被立即关闭——通道就绪，等待
// Phase 6 接入 rx_task）。
// ============================================================================
#ifndef IJPD_PLATFORM_DATA_TCP_SERVER_H
#define IJPD_PLATFORM_DATA_TCP_SERVER_H

#include <cstdint>

#include "core/errors.h"

namespace ijpd {

class DataTcpServer {
public:
    // 返回 true：保留连接（接管 socket；调用方负责关闭）
    // 返回 false：立即关闭（无任务等待）
    using AcceptPolicy = bool (*)(void* user, int fd, const char* peer);

    Err start(uint16_t port, AcceptPolicy policy, void* user) noexcept;
    void stop() noexcept;

    DataTcpServer() noexcept = default;
    ~DataTcpServer();

private:
    void run_() noexcept;

    int listener_ = -1;
    AcceptPolicy policy_ = nullptr;
    void* user_ = nullptr;
    volatile bool running_ = false;
};

}  // namespace ijpd

#endif  // IJPD_PLATFORM_DATA_TCP_SERVER_H
