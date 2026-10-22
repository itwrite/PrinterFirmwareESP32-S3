// app_main.cpp —— 固件入口（Phase 5）
#include "app/printer_app.h"
#include "core/printer_config.h"
#include "platform_esp/log_adapter.h"

extern "C" void app_main(void) {
    static constexpr const char* kTag = "main";
    IJPD_LOGI(kTag, "IJPD printer firmware starting");

    // Select one variant here for a firmware image. Unlisted transports stay off.
    static constexpr ijpd::PrinterConfig kVariant = {
        // Keep the current color IJPD behavior. Replace this with a concrete
        // thermal/inkjet profile for another hardware variant.
        .profile = nullptr,
        .services = {.wifi = true, .ble = true, .ijpd_tcp = true},
        .device_model = "IJPD-DEV",
        .fw_version = "0.3.0",
        .ble_name = "IJPD-Printer",
    };
    ijpd::PrinterApp::instance().set_config(&kVariant);
    const ijpd::Err e = ijpd::PrinterApp::instance().start();
    if (e != ijpd::Err::OK) {
        IJPD_LOGE(kTag, "start failed: %s", ijpd::err_name(e));
    }
    // 应用任务自持运行；app_main 返回
}
