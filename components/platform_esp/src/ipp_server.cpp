// ipp_server.cpp - Minimal IPP attribute-query foundation.
#include "platform_esp/ipp_server.h"

#include <cstring>

#include "esp_http_server.h"

#include "platform_esp/log_adapter.h"

namespace ijpd {
namespace {
constexpr const char* kTag = "ipp";
constexpr uint16_t kIppVersionMajor = 2;
constexpr uint16_t kIppVersionMinor = 0;
constexpr uint16_t kOpGetPrinterAttributes = 0x000B;
constexpr uint16_t kStatusOk = 0x0000;
constexpr uint16_t kStatusOperationUnsupported = 0x0501;
constexpr uint16_t kStatusDocumentFormatUnsupported = 0x040D;

constexpr uint8_t kOperationAttributes = 0x01;
constexpr uint8_t kEndOfAttributes = 0x03;
constexpr uint8_t kPrinterAttributes = 0x04;

constexpr uint8_t kTagCharset = 0x47;
constexpr uint8_t kTagNaturalLanguage = 0x48;
constexpr uint8_t kTagUri = 0x45;
constexpr uint8_t kTagName = 0x42;
constexpr uint8_t kTagKeyword = 0x44;
constexpr uint8_t kTagEnum = 0x23;

void put_u16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v >> 8);
    p[1] = static_cast<uint8_t>(v);
}

void put_u32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v);
}

uint16_t get_u16(const uint8_t* p) {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

bool append_bytes(uint8_t* out, size_t cap, size_t* len, const void* data, size_t n) {
    if (*len + n > cap) return false;
    std::memcpy(out + *len, data, n);
    *len += n;
    return true;
}

bool append_attr(uint8_t* out, size_t cap, size_t* len, uint8_t tag,
                 const char* name, const char* value) {
    const size_t name_len = std::strlen(name);
    const size_t value_len = std::strlen(value);
    if (name_len > UINT16_MAX || value_len > UINT16_MAX) return false;
    const size_t need = 1 + 2 + name_len + 2 + value_len;
    if (*len + need > cap) return false;
    out[(*len)++] = tag;
    put_u16(out + *len, static_cast<uint16_t>(name_len)); *len += 2;
    if (!append_bytes(out, cap, len, name, name_len)) return false;
    put_u16(out + *len, static_cast<uint16_t>(value_len)); *len += 2;
    return append_bytes(out, cap, len, value, value_len);
}

bool append_enum(uint8_t* out, size_t cap, size_t* len, const char* name, uint32_t value) {
    const size_t name_len = std::strlen(name);
    if (name_len > UINT16_MAX || *len + 1 + 2 + name_len + 2 + 4 > cap) return false;
    out[(*len)++] = kTagEnum;
    put_u16(out + *len, static_cast<uint16_t>(name_len)); *len += 2;
    if (!append_bytes(out, cap, len, name, name_len)) return false;
    put_u16(out + *len, 4); *len += 2;
    put_u32(out + *len, value); *len += 4;
    return true;
}

}  // namespace

IppServer::~IppServer() { stop(); }

Err IppServer::start(uint16_t port, const char* printer_name,
                     const PrintProfile* profile) noexcept {
    if (server_ != nullptr || printer_name == nullptr || printer_name[0] == '\0') {
        return server_ != nullptr ? Err::OK : Err::INVALID_ARGUMENT;
    }
    if (std::strlen(printer_name) >= sizeof(printer_name_)) return Err::INVALID_ARGUMENT;
    std::strcpy(printer_name_, printer_name);
    profile_ = profile;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = port;
    config.max_uri_handlers = 4;
    config.max_open_sockets = 4;
    httpd_handle_t handle = nullptr;
    if (httpd_start(&handle, &config) != ESP_OK) return Err::INTERNAL_ERROR;

    static httpd_uri_t uri{};
    uri.uri = "/ipp/print";
    uri.method = static_cast<httpd_method_t>(HTTP_ANY);
    uri.handler = &IppServer::handler_;
    uri.user_ctx = this;
    if (httpd_register_uri_handler(handle, &uri) != ESP_OK) {
        httpd_stop(handle);
        return Err::INTERNAL_ERROR;
    }
    server_ = handle;
    IJPD_LOGI(kTag, "IPP foundation listening on %u (Print-Job disabled)", port);
    return Err::OK;
}

void IppServer::stop() noexcept {
    if (server_ != nullptr) {
        (void)httpd_stop(static_cast<httpd_handle_t>(server_));
        server_ = nullptr;
    }
}

esp_err_t IppServer::handler_(httpd_req_t* req) noexcept {
    if (req == nullptr || req->user_ctx == nullptr) return ESP_FAIL;
    return static_cast<IppServer*>(req->user_ctx)->handle_(req);
}

esp_err_t IppServer::handle_(httpd_req_t* req) noexcept {
    if (req->method != HTTP_POST || req->content_len < 8 || req->content_len > 1024) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "IPP request is invalid");
        return ESP_OK;
    }

    uint8_t input[1024] = {};
    size_t received = 0;
    while (received < req->content_len) {
        const int n = httpd_req_recv(req, reinterpret_cast<char*>(input + received),
                                     req->content_len - received);
        if (n <= 0) return ESP_FAIL;
        received += static_cast<size_t>(n);
    }
    const uint16_t operation = get_u16(input + 2);
    const uint32_t request_id = (static_cast<uint32_t>(input[4]) << 24) |
                                (static_cast<uint32_t>(input[5]) << 16) |
                                (static_cast<uint32_t>(input[6]) << 8) | input[7];
    uint8_t response[2048] = {};
    size_t len = 0;
    put_u16(response + len, kIppVersionMajor); len += 2;
    put_u16(response + len, operation == kOpGetPrinterAttributes
                               ? kStatusOk : kStatusOperationUnsupported); len += 2;
    put_u32(response + len, request_id); len += 4;

    if (operation == kOpGetPrinterAttributes) {
        response[len++] = kOperationAttributes;
        if (!append_attr(response, sizeof(response), &len, kTagCharset,
                         "attributes-charset", "utf-8") ||
            !append_attr(response, sizeof(response), &len, kTagNaturalLanguage,
                         "attributes-natural-language", "en")) {
            return ESP_FAIL;
        }
        response[len++] = kPrinterAttributes;
        if (!append_attr(response, sizeof(response), &len, kTagUri,
                         "printer-uri-supported", "ipp://localhost/ipp/print") ||
            !append_attr(response, sizeof(response), &len, kTagName,
                         "printer-name", printer_name_) ||
            !append_attr(response, sizeof(response), &len, kTagKeyword,
                         "printer-state-reasons", "none") ||
            !append_enum(response, sizeof(response), &len, "printer-state", 3) ||
            !append_attr(response, sizeof(response), &len, kTagKeyword,
                         "ipp-versions-supported", "2.0")) {
            return ESP_FAIL;
        }
        if (profile_ != nullptr &&
            !append_attr(response, sizeof(response), &len, kTagName,
                         "printer-make-and-model", profile_->model_name)) {
            return ESP_FAIL;
        }
    } else {
        response[len++] = kOperationAttributes;
        if (!append_attr(response, sizeof(response), &len, kTagCharset,
                         "attributes-charset", "utf-8") ||
            !append_attr(response, sizeof(response), &len, kTagNaturalLanguage,
                         "attributes-natural-language", "en")) {
            return ESP_FAIL;
        }
    }
    response[len++] = kEndOfAttributes;

    httpd_resp_set_type(req, "application/ipp");
    if (operation != kOpGetPrinterAttributes) {
        // This foundation deliberately rejects Print-Job until a document
        // parser and bounded raster backend are installed.
        put_u16(response + 2, operation == 0x0002 ? kStatusDocumentFormatUnsupported
                                                  : kStatusOperationUnsupported);
    }
    return httpd_resp_send(req, reinterpret_cast<const char*>(response), len);
}

}  // namespace ijpd
