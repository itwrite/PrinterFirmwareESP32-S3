// app_main.cpp —— 固件入口（Phase 5）
#include "app/printer_app.h"
#include "platform_esp/log_adapter.h"

extern "C" void app_main(void) {
    static constexpr const char* kTag = "main";
    IJPD_LOGI(kTag, "IJPD printer firmware starting");


    const ijpd::Err e = ijpd::PrinterApp::instance().start();
    if (e != ijpd::Err::OK) {
        IJPD_LOGE(kTag, "start failed: %s", ijpd::err_name(e));
    }
    // 应用任务自持运行；app_main 返回
}
