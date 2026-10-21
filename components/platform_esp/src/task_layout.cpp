// task_layout.cpp —— 任务创建与时间源
#include "platform_esp/task_layout.h"

#include "esp_timer.h"

#include "platform_esp/log_adapter.h"

namespace ijpd {
namespace task_layout {

Err spawn(const char* name, TaskFunction_t fn, void* arg, BaseType_t core,
          UBaseType_t prio, uint32_t stack_bytes) noexcept {
    // 注意:ESP-IDF 的 xTaskCreatePinnedToCore 栈深度单位是"字节"
    // (与原生 FreeRTOS 的"字"不同),直接传字节;此前误除以 4,
    // 所有任务实际栈只有 1/4,导致栈溢出与内存腐蚀(IWDT/崩溃)。
    BaseType_t ok = xTaskCreatePinnedToCore(fn, name, stack_bytes, arg, prio,
                                            nullptr, core);
    if (ok != pdPASS) {
        IJPD_LOGE("task", "spawn %s failed", name);
        return Err::OUT_OF_MEMORY;
    }
    return Err::OK;
}

uint64_t now_ms() noexcept {
    return static_cast<uint64_t>(esp_timer_get_time() / 1000);
}

}  // namespace task_layout
}  // namespace ijpd
