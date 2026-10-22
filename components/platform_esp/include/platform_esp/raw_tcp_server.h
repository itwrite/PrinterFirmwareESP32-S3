// ============================================================================
// raw_tcp_server.h - Standard RAW/ESC-POS TCP ingress.
//
// This is deliberately separate from IJPD control/data ports. A compatible
// thermal profile may expose it as a plain byte stream (default port 9102).
// ============================================================================
#ifndef IJPD_PLATFORM_RAW_TCP_SERVER_H
#define IJPD_PLATFORM_RAW_TCP_SERVER_H

#include <cstdint>

#include "core/errors.h"

namespace ijpd {

class RawTcpServer {
public:
    using AcceptPolicy = bool (*)(void* user, int fd, const char* peer);

    Err start(uint16_t port, AcceptPolicy policy, void* user) noexcept;
    void stop() noexcept;

    RawTcpServer() noexcept = default;
    ~RawTcpServer();

private:
    void run_() noexcept;

    int listener_ = -1;
    AcceptPolicy policy_ = nullptr;
    void* user_ = nullptr;
    volatile bool running_ = false;
};

}  // namespace ijpd

#endif  // IJPD_PLATFORM_RAW_TCP_SERVER_H
