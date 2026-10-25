// ============================================================================
// task_layout.h —— 应用任务布局（Phase 5，依据 docs/memory_budget.md §4）
//
// 初值（Phase 6 实测 uxTaskGetStackHighWaterMark 后固定并留 30% 余量）：
//   ctrl_task   Core0 prio 4  stack 16384
//   rx_task     Core0 prio 10 stack 4096（Phase 6）
//   decode_task Core1 prio 12 stack 6144（Phase 6）
//   print_task  Core1 prio 14 stack 4096（Phase 6）
//   report_task Core0 prio 3  stack 4096（Phase 6）
// 全部低于系统任务优先级（>=18）。
// ============================================================================
#ifndef IJPD_PLATFORM_TASK_LAYOUT_H
#define IJPD_PLATFORM_TASK_LAYOUT_H

#include <cstdint>

#include "core/errors.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace ijpd {
namespace task_layout {

// 优先级（相对，应用层内部）
inline constexpr UBaseType_t kPrioCtrl = 4;
inline constexpr UBaseType_t kPrioRx = 10;
inline constexpr UBaseType_t kPrioDecode = 12;
inline constexpr UBaseType_t kPrioPrint = 14;
inline constexpr UBaseType_t kPrioReport = 3;

inline constexpr uint32_t kStackCtrl = 16384;  // AirPrint/mDNS/control responses need margin
inline constexpr uint32_t kStackAirPrint = 65536;
inline constexpr uint32_t kStackRx = 4096;
inline constexpr uint32_t kStackDecode = 6144;
inline constexpr uint32_t kStackPrint = 4096;
inline constexpr uint32_t kStackReport = 4096;

// 创建绑核任务；失败返回 OUT_OF_MEMORY
Err spawn(const char* name, TaskFunction_t fn, void* arg, BaseType_t core,
          UBaseType_t prio, uint32_t stack_bytes) noexcept;

Err spawn_with_caps(const char* name, TaskFunction_t fn, void* arg, BaseType_t core,
                    UBaseType_t prio, uint32_t stack_bytes,
                    uint32_t memory_caps) noexcept;

// 当前毫秒（esp_timer）
uint64_t now_ms() noexcept;

}  // namespace task_layout
}  // namespace ijpd

#endif  // IJPD_PLATFORM_TASK_LAYOUT_H
