// ============================================================================
// ipp_http_server.h - Opt-in bounded HTTP transport for IPP.
//
// This transport intentionally has no IPP operation logic. It validates the
// HTTP envelope, streams body chunks to a Host, then serializes one response.
// It is not started by the default application.
// ============================================================================
#ifndef IJPD_AIRPRINT_ESP_IPP_HTTP_SERVER_H
#define IJPD_AIRPRINT_ESP_IPP_HTTP_SERVER_H

#include <cstddef>
#include <cstdint>

#include "airprint_core/http_request_parser.h"
#include "core/errors.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace ijpd {
namespace airprint {

struct HttpResponse {
    uint16_t status_code = 501;
    const char* reason = nullptr;
    const char* content_type = "text/plain; charset=utf-8";
    const uint8_t* body = nullptr;
    size_t body_length = 0;
};

enum class HttpAbortReason : uint8_t {
    PeerClosed = 0,
    IdleTimeout,
    ParserError,
    CallbackRejected,
    ServerStopping,
    SocketError,
};

class IppHttpServer {
public:
    struct Host {
        void* user = nullptr;
        // Views point into the parser buffer and are valid only during callback.
        bool (*on_headers)(void* user, const HttpRequestHead& head) = nullptr;
        // Data points into the caller's socket buffer and is valid only during callback.
        bool (*on_body)(void* user, const uint8_t* data, size_t length,
                        bool message_complete) = nullptr;
        HttpResponse (*on_complete)(void* user) = nullptr;
        void (*on_abort)(void* user, HttpAbortReason reason) = nullptr;
    };

    Err start(uint16_t port, const Host& host,
              const HttpParserLimits& limits = {}) noexcept;
    void stop() noexcept;

    bool running() const noexcept { return running_; }
    uint16_t port() const noexcept { return port_; }

    IppHttpServer() noexcept = default;
    ~IppHttpServer();

private:
    static void task_entry_(void* arg) noexcept;
    void run_() noexcept;
    void handle_client_(int fd) noexcept;

    bool wait_readable_(int fd, uint32_t timeout_ms) noexcept;
    bool wait_writable_(int fd, uint32_t timeout_ms) noexcept;
    bool send_all_(int fd, const uint8_t* data, size_t length) noexcept;
    bool send_response_(int fd, const HttpResponse& response) noexcept;
    bool send_simple_(int fd, uint16_t status, const char* reason,
                      const char* message) noexcept;
    bool valid_head_(const HttpRequestHead& head, uint16_t* status,
                     const char** reason) noexcept;

    int listener_ = -1;
    uint16_t port_ = 0;
    Host host_{};
    // Keep the bounded parser and socket buffer out of the 8 KiB HTTP task
    // stack. The parser owns an 8 KiB header buffer by design.
    HttpRequestParser parser_{};
    uint8_t socket_buffer_[2048] = {};
    volatile bool running_ = false;
    TaskHandle_t task_ = nullptr;
};

}  // namespace airprint
}  // namespace ijpd

#endif  // IJPD_AIRPRINT_ESP_IPP_HTTP_SERVER_H
