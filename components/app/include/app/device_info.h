// ============================================================================
// device_info.h —— get_device_info 响应组装（app 层，Phase 2）
//
// 职责：把平台注入的运行时信息（DeviceInfo）序列化为协议 §4.2 的
// data.result。命令元数据遍历 core/command_schema 单一数据源，
// supported 标志与 CommandRouter::Capabilities 同值注入，
// 保证“上报的命令 = 实际路由的命令”。
//
// 喷头未确认前 printhead 全部为 null + is_configured:false，禁止编造。
//
// 线程安全性：纯函数（只读入参），线程安全。
// 所有权：    所有字符串指针由调用方保证生命周期（通常为静态字面量）。
// ============================================================================
#ifndef IJPD_APP_DEVICE_INFO_H
#define IJPD_APP_DEVICE_INFO_H

#include <cstdint>

#include "core/errors.h"
#include "core/json_lite.h"
#include "core/printer_config.h"
#include "core/print_profile.h"

namespace ijpd {

struct DeviceInfo {
    // device
    const char* device_id;    // 如 "IJPD-0001"（平台注入，静态字符串）
    const char* model;        // 如 "IJPD-P61-MOCK"
    const char* fw_version;   // 如 "0.1.0-phase2"
    const char* hal_mode;     // "mock" | "real"

    // memory（psram_sizes_valid=false 时总量/余量输出 null，不得编造）
    bool psram_available;
    bool psram_sizes_valid;
    uint32_t psram_total_bytes;
    uint32_t psram_free_bytes;

    // Logical print geometry reported by the injected profile.
    uint32_t print_width_um = 0;

    // capabilities（与 CommandRouter::Capabilities 保持一致）
    bool cap_paper_feed;
    bool cap_cutter;
    bool cap_mock_print;
    PrinterServices services{};
    uint16_t escpos_raw_tcp_port = 0;

    // Profile（中间件：打印机类型描述；null 时 fallback 到默认喷墨）
    const PrintProfile* profile = nullptr;
};

// 写出 result 对象（device/printhead/memory/capabilities/limits/commands）。
// 返回 Err::OK 且 w.ok() 才可用。
Err write_device_info_result(JsonWriter& w, const DeviceInfo& di) noexcept;

}  // namespace ijpd

#endif  // IJPD_APP_DEVICE_INFO_H
