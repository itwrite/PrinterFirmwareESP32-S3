// app_main.cpp —— 固件入口（Phase 5）
#include "app/printer_app.h"
#include "core/printer_config.h"
#include "printer_hal/ricoh_mh5440_mock_hal.h"
#include "platform_esp/log_adapter.h"
#include "profiles/ricoh_mh5440_mock.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

void mock_delay(void*, uint32_t ms) noexcept {
    if (ms != 0) vTaskDelay(pdMS_TO_TICKS(ms));
}

}  // namespace

extern "C" void app_main(void) {
    static constexpr const char* kTag = "main";
    IJPD_LOGI(kTag, "IJPD printer firmware starting");

    // Default image: A4 logical RICOH MH5440 CMYK8 Mock. Unlisted transports stay off.
    static ijpd::RicohMh5440MockConfig mock_config{};
#if defined(IJPD_AIRPRINT_ENABLED)
    // AirPrint is a logical Mock path; do not make the client wait for the
    // default physical-row simulation delay before receiving the IPP response.
    mock_config.row_period_us = 0;
#endif
    static ijpd::MockRicohMh5440Hal ricoh_mh5440_mock(mock_config, mock_delay, nullptr);
    static const ijpd::PrinterHalBundle hal_bundle = {
        .printhead = &ricoh_mh5440_mock,
    };
    static constexpr ijpd::PrinterConfig kVariant = {
        .profile = &ijpd::profiles::kRicohMh5440Mock,
        .services = {
            .wifi = true,
            .ble = true,
            .ijpd_tcp = true,
#if defined(IJPD_AIRPRINT_ENABLED)
            .airprint = true,
#endif
        },
        .device_model = "RICOH-MH5440-MOCK",
        .fw_version = "0.3.0",
        .ble_name = "IJPD-Printer",
#if defined(IJPD_AIRPRINT_ENABLED)
        .airprint = {
            .port = 631,
            .accept_print_jobs = true,
            .printer_name = "RICOH MH5440 Mock",
            .make_and_model = "RICOH MH5440 logical CMYK8 Mock",
            .printer_info = "logical Mock HAL; image/urf Print-Job enabled",
            .printer_location = "Lab",
            .media_keyword = "iso_a4_210x297mm",
            .document_format = "image/urf",
            .urf_supported = "V1.4,CP1,RS300,W8,SRGB24,DM1,IS1,MT1,FN3",
            .dpi_x = 300,
            .dpi_y = 300,
            .color_supported = true,
            .duplex_supported = false,
        },
#endif
    };
    ijpd::PrinterApp::instance().set_config(&kVariant);
    ijpd::PrinterApp::instance().set_hal_bundle(hal_bundle);
    const ijpd::Err e = ijpd::PrinterApp::instance().start();
    if (e != ijpd::Err::OK) {
        IJPD_LOGE(kTag, "start failed: %s", ijpd::err_name(e));
    }
    // 应用任务自持运行；app_main 返回
}
