// ipp_http_server.cpp - Opt-in bounded HTTP transport for IPP.
#include "airprint_esp/ipp_http_server.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/select.h>

#include "esp_timer.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

namespace ijpd {
namespace airprint {
namespace {

constexpr uint32_t kClientIdleTimeoutMs = 15000;
constexpr uint32_t kSelectTimeoutMs = 100;
constexpr size_t kSocketBufferBytes = 2048;

uint64_t now_ms() noexcept {
    return static_cast<uint64_t>(esp_timer_get_time() / 1000);
}

bool view_equals(const HttpStringView& view, const char* value) noexcept {
    if (value == nullptr) return false;
    const size_t length = std::strlen(value);
    return length == view.length && std::memcmp(view.data, value, length) == 0;
}

bool content_type_is_ipp(const HttpStringView& view) noexcept {
    constexpr char kIpp[] = "application/ipp";
    constexpr char kOctet[] = "application/octet-stream";
    const bool ipp = view.length >= sizeof(kIpp) - 1 &&
                     std::memcmp(view.data, kIpp, sizeof(kIpp) - 1) == 0;
    const bool octet = view.length >= sizeof(kOctet) - 1 &&
                       std::memcmp(view.data, kOctet, sizeof(kOctet) - 1) == 0;
    if (!ipp && !octet) {
        return false;
    }
    const size_t type_length = ipp ? sizeof(kIpp) - 1 : sizeof(kOctet) - 1;
    return view.length == type_length || view.data[type_length] == ';' ||
           view.data[type_length] == ' ' || view.data[type_length] == '\t';
}

const char* default_reason(uint16_t status) noexcept {
    switch (status) {
        case 100: return "Continue";
        case 200: return "OK";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 411: return "Length Required";
        case 413: return "Payload Too Large";
        case 415: return "Unsupported Media Type";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 503: return "Service Unavailable";
        default: return "Error";
    }
}

}  // namespace

IppHttpServer::~IppHttpServer() { stop(); }

Err IppHttpServer::start(uint16_t port, const Host& host,
                         const HttpParserLimits& limits) noexcept {
    if (port == 0 || host.on_headers == nullptr || host.on_body == nullptr ||
        host.on_complete == nullptr || running_) {
        return Err::INVALID_ARGUMENT;
    }
    host_ = host;
    parser_ = HttpRequestParser(limits);
    port_ = port;
    listener_ = lwip_socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (listener_ < 0) return Err::OUT_OF_MEMORY;

    int reuse = 1;
    (void)lwip_setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);
    if (lwip_bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        lwip_listen(listener_, 2) != 0) {
        lwip_close(listener_);
        listener_ = -1;
        return Err::INTERNAL_ERROR;
    }

    running_ = true;
    if (xTaskCreatePinnedToCore(&IppHttpServer::task_entry_, "ipp_http", 8192, this,
                                4, &task_, 0) != pdPASS) {
        running_ = false;
        lwip_close(listener_);
        listener_ = -1;
        return Err::OUT_OF_MEMORY;
    }
    return Err::OK;
}

void IppHttpServer::stop() noexcept {
    running_ = false;
    if (listener_ >= 0) {
        lwip_close(listener_);
        listener_ = -1;
    }
}

void IppHttpServer::task_entry_(void* arg) noexcept {
    auto* self = static_cast<IppHttpServer*>(arg);
    self->run_();
    self->task_ = nullptr;
    vTaskDelete(nullptr);
}

void IppHttpServer::run_() noexcept {
    while (running_) {
        fd_set readable;
        FD_ZERO(&readable);
        if (listener_ < 0) break;
        FD_SET(listener_, &readable);
        timeval timeout{};
        timeout.tv_usec = static_cast<suseconds_t>(kSelectTimeoutMs * 1000);
        const int ready = lwip_select(listener_ + 1, &readable, nullptr, nullptr, &timeout);
        if (!running_) break;
        if (ready <= 0 || !FD_ISSET(listener_, &readable)) continue;

        sockaddr_in peer{};
        socklen_t peer_length = sizeof(peer);
        const int fd = lwip_accept(listener_, reinterpret_cast<sockaddr*>(&peer),
                                   &peer_length);
        if (fd < 0) continue;
        const int flags = fcntl(fd, F_GETFL, 0);
        (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        handle_client_(fd);
        lwip_shutdown(fd, SHUT_RDWR);
        lwip_close(fd);
    }
}

void IppHttpServer::handle_client_(int fd) noexcept {
    parser_.reset();
    HttpRequestParser& parser = parser_;
    uint8_t* buffer = socket_buffer_;
    uint64_t last_activity = now_ms();
    bool headers_delivered = false;
    bool request_complete = false;
    bool aborted = false;
    const auto abort_request = [&](HttpAbortReason reason) {
        if (!request_complete && !aborted && headers_delivered && host_.on_abort != nullptr) {
            aborted = true;
            host_.on_abort(host_.user, reason);
        }
    };

    while (running_ && !parser.complete()) {
        const uint64_t elapsed = now_ms() - last_activity;
        if (elapsed >= kClientIdleTimeoutMs ||
            !wait_readable_(fd, kClientIdleTimeoutMs - static_cast<uint32_t>(elapsed))) {
            abort_request(elapsed >= kClientIdleTimeoutMs
                              ? HttpAbortReason::IdleTimeout
                              : HttpAbortReason::SocketError);
            (void)send_simple_(fd, 408, nullptr, "request timeout");
            return;
        }
        const ssize_t received = lwip_recv(fd, buffer, kSocketBufferBytes, MSG_DONTWAIT);
        if (received == 0) {
            abort_request(HttpAbortReason::PeerClosed);
            return;
        }
        if (received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            abort_request(HttpAbortReason::SocketError);
            return;
        }
        last_activity = now_ms();
        size_t offset = 0;
        while (offset < static_cast<size_t>(received) && !parser.complete()) {
            const HttpFeedResult result = parser.feed(buffer + offset,
                                                      static_cast<size_t>(received) - offset);
            if (result.event == HttpFeedEvent::Error) {
                abort_request(HttpAbortReason::ParserError);
                (void)send_simple_(fd, 400, nullptr, http_parser_error_name(result.error));
                return;
            }
            if (result.event == HttpFeedEvent::NeedMore) {
                offset += result.consumed;
                break;
            }
            if (result.event == HttpFeedEvent::HeadersComplete) {
                offset += result.consumed;
                uint16_t status = 200;
                const char* reason = nullptr;
                if (!valid_head_(parser.head(), &status, &reason)) {
                    abort_request(HttpAbortReason::ParserError);
                    (void)send_simple_(fd, status, reason, "invalid ipp http request");
                    return;
                }
                if (!host_.on_headers(host_.user, parser.head())) {
                    headers_delivered = true;
                    abort_request(HttpAbortReason::CallbackRejected);
                    (void)send_simple_(fd, 400, nullptr, "request rejected");
                    return;
                }
                headers_delivered = true;
                if (parser.head().expect_continue && !parser.complete()) {
                    static constexpr char kContinue[] =
                        "HTTP/1.1 100 Continue\r\n\r\n";
                    if (!send_all_(fd, reinterpret_cast<const uint8_t*>(kContinue),
                                   sizeof(kContinue) - 1)) {
                        abort_request(HttpAbortReason::SocketError);
                        return;
                    }
                }
                if (parser.complete()) break;
                continue;
            }
            if (result.event == HttpFeedEvent::BodyChunk) {
                offset += result.consumed;
                if (!headers_delivered ||
                    !host_.on_body(host_.user, result.body, result.body_length,
                                   result.message_complete)) {
                    abort_request(HttpAbortReason::CallbackRejected);
                    (void)send_simple_(fd, 500, nullptr, "body rejected");
                    return;
                }
                continue;
            }
        }
    }

    if (!parser.complete()) {
        abort_request(HttpAbortReason::ServerStopping);
        return;
    }
    request_complete = true;
    const HttpResponse response = host_.on_complete(host_.user);
    (void)send_response_(fd, response);
}

bool IppHttpServer::wait_readable_(int fd, uint32_t timeout_ms) noexcept {
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(fd, &readable);
    timeval timeout{};
    timeout.tv_sec = static_cast<time_t>(timeout_ms / 1000);
    timeout.tv_usec = static_cast<suseconds_t>((timeout_ms % 1000) * 1000);
    return lwip_select(fd + 1, &readable, nullptr, nullptr, &timeout) > 0 &&
           FD_ISSET(fd, &readable);
}

bool IppHttpServer::wait_writable_(int fd, uint32_t timeout_ms) noexcept {
    fd_set writable;
    FD_ZERO(&writable);
    FD_SET(fd, &writable);
    timeval timeout{};
    timeout.tv_sec = static_cast<time_t>(timeout_ms / 1000);
    timeout.tv_usec = static_cast<suseconds_t>((timeout_ms % 1000) * 1000);
    return lwip_select(fd + 1, nullptr, &writable, nullptr, &timeout) > 0 &&
           FD_ISSET(fd, &writable);
}

bool IppHttpServer::send_all_(int fd, const uint8_t* data, size_t length) noexcept {
    size_t sent = 0;
    while (sent < length) {
        const ssize_t result = lwip_send(fd, data + sent, length - sent, MSG_DONTWAIT);
        if (result > 0) {
            sent += static_cast<size_t>(result);
            continue;
        }
        if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (!wait_writable_(fd, 1000)) return false;
            continue;
        }
        return false;
    }
    return true;
}

bool IppHttpServer::send_response_(int fd, const HttpResponse& response) noexcept {
    const char* reason = response.reason != nullptr ? response.reason
                                                      : default_reason(response.status_code);
    const char* content_type = response.content_type != nullptr
                                   ? response.content_type
                                   : "text/plain; charset=utf-8";
    char header[512] = {};
    const int length = snprintf(header, sizeof(header),
                                "HTTP/1.1 %u %s\r\n"
                                "Content-Type: %s\r\n"
                                "Content-Length: %zu\r\n"
                                "Connection: close\r\n\r\n",
                                static_cast<unsigned>(response.status_code), reason,
                                content_type, response.body_length);
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(header)) return false;
    if (!send_all_(fd, reinterpret_cast<const uint8_t*>(header), static_cast<size_t>(length))) {
        return false;
    }
    return send_all_(fd, response.body, response.body_length);
}

bool IppHttpServer::send_simple_(int fd, uint16_t status, const char* reason,
                                 const char* message) noexcept {
    const char* text = message != nullptr ? message : "";
    HttpResponse response;
    response.status_code = status;
    response.reason = reason;
    response.content_type = "text/plain; charset=utf-8";
    response.body = reinterpret_cast<const uint8_t*>(text);
    response.body_length = std::strlen(text);
    return send_response_(fd, response);
}

bool IppHttpServer::valid_head_(const HttpRequestHead& head, uint16_t* status,
                                const char** reason) noexcept {
    if (status == nullptr || reason == nullptr) return false;
    if (!view_equals(head.method, "POST")) {
        *status = 405;
        *reason = "Method Not Allowed";
        return false;
    }
    if (!view_equals(head.target, "/ipp/print")) {
        *status = 404;
        *reason = "Not Found";
        return false;
    }
    if (!head.has_content_length && !head.chunked) {
        *status = 411;
        *reason = "Length Required";
        return false;
    }
    if (!content_type_is_ipp(head.content_type)) {
        *status = 415;
        *reason = "Unsupported Media Type";
        return false;
    }
    return true;
}

}  // namespace airprint
}  // namespace ijpd
