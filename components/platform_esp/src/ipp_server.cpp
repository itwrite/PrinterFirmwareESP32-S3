// ipp_server.cpp - Minimal IPP attribute-query foundation.
#include "platform_esp/ipp_server.h"

#include <cstring>

#include "esp_heap_caps.h"
#include "esp_http_server.h"

#include "platform_esp/log_adapter.h"

namespace ijpd {
namespace {
constexpr const char* kTag = "ipp";
constexpr uint16_t kIppVersionMajor = 2;
constexpr uint16_t kIppVersionMinor = 0;
constexpr uint16_t kOpGetPrinterAttributes = 0x000B;
constexpr uint16_t kOpValidateJob = 0x0004;
constexpr uint16_t kOpPrintJob = 0x0002;
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
constexpr uint8_t kEndCollection = 0x04;

void put_u16(uint8_t* p, uint16_t v);
void put_u32(uint8_t* p, uint32_t v);
bool append_attr(uint8_t* out, size_t cap, size_t* len, uint8_t tag,
                 const char* name, const char* value);

struct BodyReader {
    httpd_req_t* req = nullptr;
    size_t remaining = 0;
    uint8_t buffer[1024] = {};
    size_t used = 0;
    size_t pos = 0;

    bool refill() noexcept {
        if (remaining == 0) return false;
        const size_t ask = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
        const int n = httpd_req_recv(req, reinterpret_cast<char*>(buffer), ask);
        if (n <= 0) return false;
        used = static_cast<size_t>(n);
        pos = 0;
        remaining -= used;
        return true;
    }

    bool read_u8(uint8_t* out) noexcept {
        if (pos == used && !refill()) return false;
        *out = buffer[pos++];
        return true;
    }

    bool read(void* out, size_t len) noexcept {
        auto* dst = static_cast<uint8_t*>(out);
        while (len != 0) {
            if (pos == used && !refill()) return false;
            const size_t n = (used - pos < len) ? used - pos : len;
            std::memcpy(dst, buffer + pos, n);
            dst += n;
            pos += n;
            len -= n;
        }
        return true;
    }
};

uint16_t be16(const uint8_t* p) {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

bool ipp_group_tag(uint8_t tag) {
    return tag == 0x01 || tag == 0x02 || tag == 0x04 || tag == 0x05;
}

bool read_ipp_attributes(BodyReader* reader, char* document_format,
                         size_t format_cap) {
    document_format[0] = '\0';
    for (;;) {
        uint8_t tag = 0;
        if (!reader->read_u8(&tag)) return false;
        if (tag == kEndOfAttributes) return true;
        if (ipp_group_tag(tag)) continue;

        uint8_t name_len_buf[2] = {};
        uint8_t value_len_buf[2] = {};
        if (!reader->read(name_len_buf, sizeof(name_len_buf))) return false;
        const uint16_t name_len = be16(name_len_buf);
        char name[80] = {};
        if (name_len >= sizeof(name)) {
            for (uint16_t i = 0; i < name_len; ++i) {
                uint8_t ignored = 0;
                if (!reader->read_u8(&ignored)) return false;
            }
        } else if (!reader->read(name, name_len)) {
            return false;
        } else {
            name[name_len] = '\0';
        }
        if (!reader->read(value_len_buf, sizeof(value_len_buf))) return false;
        const uint16_t value_len = be16(value_len_buf);
        const bool wanted = name_len < sizeof(name) &&
                            std::strcmp(name, "document-format") == 0 &&
                            value_len > 0 && value_len < format_cap;
        if (wanted) {
            if (!reader->read(document_format, value_len)) return false;
            document_format[value_len] = '\0';
        } else {
            for (uint16_t i = 0; i < value_len; ++i) {
                uint8_t ignored = 0;
                if (!reader->read_u8(&ignored)) return false;
            }
        }
    }
}

void put_response_header(uint8_t* response, size_t* len, uint16_t status,
                         uint32_t request_id) {
    put_u16(response + *len, kIppVersionMajor); *len += 2;
    put_u16(response + *len, status); *len += 2;
    put_u32(response + *len, request_id); *len += 4;
}

esp_err_t send_ipp_status(httpd_req_t* req, uint16_t status, uint32_t request_id) {
    uint8_t response[256] = {};
    size_t len = 0;
    put_response_header(response, &len, status, request_id);
    response[len++] = kOperationAttributes;
    if (!append_attr(response, sizeof(response), &len, kTagCharset,
                     "attributes-charset", "utf-8") ||
        !append_attr(response, sizeof(response), &len, kTagNaturalLanguage,
                     "attributes-natural-language", "en")) return ESP_FAIL;
    response[len++] = kEndOfAttributes;
    httpd_resp_set_type(req, "application/ipp");
    return httpd_resp_send(req, reinterpret_cast<const char*>(response), len);
}

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

bool find_format(const uint8_t* data, size_t len, char* out, size_t cap) {
    constexpr const char* kPrefix = "document-format";
    for (size_t i = 8; i + std::strlen(kPrefix) < len; ++i) {
        if (std::memcmp(data + i, kPrefix, std::strlen(kPrefix)) != 0) continue;
        for (size_t j = i + std::strlen(kPrefix); j + 2 < len; ++j) {
            if (data[j] == 'a' || data[j] == 'i' || data[j] == 'p') {
                size_t n = 0;
                while (j + n < len && data[j + n] >= 0x20 && data[j + n] <= 0x7e) {
                    if (n + 1 >= cap) return false;
                    out[n] = static_cast<char>(data[j + n]);
                    ++n;
                }
                if (n > 0) { out[n] = '\0'; return true; }
            }
        }
    }
    return false;
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
                     const PrintProfile* profile, const Host& host) noexcept {
    if (server_ != nullptr || printer_name == nullptr || printer_name[0] == '\0') {
        return server_ != nullptr ? Err::OK : Err::INVALID_ARGUMENT;
    }
    if (std::strlen(printer_name) >= sizeof(printer_name_)) return Err::INVALID_ARGUMENT;
    std::strcpy(printer_name_, printer_name);
    profile_ = profile;
    host_ = host;

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
    if (req->method != HTTP_POST || req->content_len < 8 ||
        req->content_len > cfg::MAX_JOB_BYTES + 8192) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "IPP request is invalid");
        return ESP_OK;
    }

    uint8_t operation_header[8] = {};
    size_t received = 0;
    while (received < sizeof(operation_header)) {
        const int n = httpd_req_recv(req, reinterpret_cast<char*>(operation_header + received),
                                     sizeof(operation_header) - received);
        if (n <= 0) return ESP_FAIL;
        received += static_cast<size_t>(n);
    }
    const uint16_t operation = get_u16(operation_header + 2);
    const uint32_t request_id = (static_cast<uint32_t>(operation_header[4]) << 24) |
                                (static_cast<uint32_t>(operation_header[5]) << 16) |
                                (static_cast<uint32_t>(operation_header[6]) << 8) |
                                operation_header[7];
    if (operation == kOpPrintJob) {
        return handle_print_job_(req, req->content_len - sizeof(operation_header), request_id);
    }
    if (req->content_len > 1024) {
        httpd_resp_send_err(req, HTTPD_413_CONTENT_TOO_LARGE, "IPP request is too large");
        return ESP_OK;
    }
    uint8_t input[1024] = {};
    std::memcpy(input, operation_header, sizeof(operation_header));
    size_t body_received = sizeof(operation_header);
    while (body_received < req->content_len) {
        const int n = httpd_req_recv(req, reinterpret_cast<char*>(input + body_received),
                                     req->content_len - body_received);
        if (n <= 0) return ESP_FAIL;
        body_received += static_cast<size_t>(n);
    }
    uint8_t response[2048] = {};
    size_t len = 0;
    put_u16(response + len, kIppVersionMajor); len += 2;
    const bool supported_validation = operation == kOpGetPrinterAttributes ||
                                      operation == kOpValidateJob;
    char document_format[64] = {};
    const bool has_document_format = find_format(input, body_received, document_format,
                                                  sizeof(document_format));
    const bool validate_ok = operation == kOpValidateJob &&
                             has_document_format &&
                             std::strcmp(document_format, "image/pwg-raster") == 0;
    put_u16(response + len, operation == kOpGetPrinterAttributes || validate_ok
                               ? kStatusOk
                               : (supported_validation ? kStatusDocumentFormatUnsupported
                                                        : kStatusOperationUnsupported));
    len += 2;
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
        put_u16(response + 2, operation == kOpPrintJob ? kStatusDocumentFormatUnsupported
                                                  : (operation == kOpValidateJob && validate_ok
                                                         ? kStatusOk
                                                         : (supported_validation
                                                                ? kStatusDocumentFormatUnsupported
                                                                : kStatusOperationUnsupported)));
    }
    return httpd_resp_send(req, reinterpret_cast<const char*>(response), len);
}

esp_err_t IppServer::handle_print_job_(httpd_req_t* req, size_t body_len,
                                       uint32_t request_id) noexcept {
    BodyReader reader{req, body_len};
    char document_format[64] = {};
    if (!read_ipp_attributes(&reader, document_format, sizeof(document_format)) ||
        std::strcmp(document_format, "image/pwg-raster") != 0) {
        return send_ipp_status(req, kStatusDocumentFormatUnsupported, request_id);
    }

    uint8_t header[PWG_RASTER_HEADER_BYTES] = {};
    if (!reader.read(header, sizeof(header))) {
        return send_ipp_status(req, kStatusOperationUnsupported, request_id);
    }
    PwgRasterPage page{};
    if (parse_pwg_raster_header(header, sizeof(header), &page) != Err::OK ||
        profile_ == nullptr || profile_->pixel_format != PixelFormat::Mono1 ||
        page.bits_per_color != 1 || page.bits_per_pixel != 1 ||
        page.color_space != 3 || page.compression != 0 ||
        page.width != profile_->print_width_px || page.dpi_x != profile_->dpi ||
        page.dpi_y != profile_->dpi || page.bytes_per_line != profile_->mono1_row_bytes() ||
        host_.begin_page == nullptr || host_.write_row == nullptr ||
        host_.end_page == nullptr) {
        return send_ipp_status(req, kStatusDocumentFormatUnsupported, request_id);
    }

    const Err begin = host_.begin_page(host_.user, page);
    if (begin != Err::OK) return send_ipp_status(req, kStatusOperationUnsupported, request_id);
    uint8_t* row = static_cast<uint8_t*>(heap_caps_malloc(page.bytes_per_line,
                                                          MALLOC_CAP_SPIRAM));
    if (row == nullptr) {
        (void)host_.end_page(host_.user, Err::OUT_OF_MEMORY);
        return send_ipp_status(req, kStatusOperationUnsupported, request_id);
    }
    Err status = Err::OK;
    for (uint32_t y = 0; y < page.height; ++y) {
        if (!reader.read(row, page.bytes_per_line) ||
            host_.write_row(host_.user, row, page.bytes_per_line, y) != Err::OK) {
            status = Err::HAL_FAULT;
            break;
        }
    }
    if (status == Err::OK && reader.remaining != 0) status = Err::PACKET_LENGTH_INVALID;
    heap_caps_free(row);
    const Err end_status = host_.end_page(host_.user, status);
    return send_ipp_status(req, end_status == Err::OK ? kStatusOk
                                                        : kStatusOperationUnsupported,
                           request_id);
}

}  // namespace ijpd
