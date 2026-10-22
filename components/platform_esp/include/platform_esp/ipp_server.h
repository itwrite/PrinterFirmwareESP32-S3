// ============================================================================
// ipp_server.h - Minimal IPP/HTTP foundation for future AirPrint support.
//
// This server answers Get-Printer-Attributes and explicitly rejects Print-Job
// until a bounded PWG/Apple Raster document backend is installed. It must not
// be advertised as AirPrint-ready while Print-Job is unavailable.
// ============================================================================
#ifndef IJPD_PLATFORM_IPP_SERVER_H
#define IJPD_PLATFORM_IPP_SERVER_H

#include <cstdint>

#include "esp_http_server.h"
#include "core/errors.h"
#include "core/print_profile.h"

namespace ijpd {

class IppServer {
public:
    Err start(uint16_t port, const char* printer_name,
              const PrintProfile* profile) noexcept;
    void stop() noexcept;

    bool started() const noexcept { return server_ != nullptr; }

    IppServer() noexcept = default;
    ~IppServer();

private:
    static esp_err_t handler_(httpd_req_t* request) noexcept;
    esp_err_t handle_(httpd_req_t* request) noexcept;

    void* server_ = nullptr;  // httpd_handle_t
    const PrintProfile* profile_ = nullptr;
    char printer_name_[64] = {};
};

}  // namespace ijpd

#endif  // IJPD_PLATFORM_IPP_SERVER_H
