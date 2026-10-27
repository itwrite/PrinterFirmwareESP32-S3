// device_info.cpp —— get_device_info result 组装（协议 §4.2）
#include "app/device_info.h"

#include <cstring>

#include "core/command_schema.h"
#include "core/config_limits.h"

namespace ijpd {
namespace {

bool name_eq(const CommandSpec& c, const char* name) noexcept {
    return std::strcmp(c.cmd, name) == 0;
}

// 命令的运行时 supported 标志（与 CommandRouter::Capabilities 同源注入）
bool command_supported(const DeviceInfo& di, const CommandSpec& c) noexcept {
    if (name_eq(c, "start_print")) return di.psram_available;
    if (name_eq(c, "feed_paper")) return di.cap_paper_feed;
    if (name_eq(c, "cut_paper")) return di.cap_cutter;
    return true;  // get_device_info / cancel_print / get_print_status 恒可用
}

bool write_device_section(JsonWriter& w, const DeviceInfo& di) noexcept {
    if (!w.key("device") || !w.begin_object()) return false;
    if (!w.kv_cstr("device_id", di.device_id)) return false;
    if (!w.kv_cstr("model", di.profile != nullptr ? di.profile->model_name
                                                    : di.model)) return false;
    if (!w.kv_cstr("fw_version", di.fw_version)) return false;
    if (!w.kv_cstr("protocol_version", cfg::PROTOCOL_VERSION)) return false;
    if (!w.key("connections") || !w.begin_array()) return false;
    if (!w.value_cstr("ble") || !w.value_cstr("wifi_tcp")) return false;
    if (!w.end_array()) return false;
    if (!w.kv_cstr("hal_mode", di.hal_mode)) return false;
    return w.end_object();
}

bool write_printhead_section(JsonWriter& w, const PrintProfile* pf) noexcept {
    if (!w.key("printhead") || !w.begin_object()) return false;
    if (pf != nullptr) {
        // Profile 提供：dpi / 宽度 / 像素格式（型号待硬件确认仍为 null）
        if (!w.kv_null("brand")) return false;
        if (!w.kv_null("model")) return false;  // 真实喷头型号 Phase 7 填入
        if (!w.kv_null("nozzle_count")) return false;
        if (!w.kv_i64("dpi", pf->dpi)) return false;
        if (!w.kv_i64("print_width_px", pf->print_width_px)) return false;
        if (!w.kv_i64("print_width_um", pf->print_width_um)) return false;
        if (!w.kv_cstr("pixel_format", pf->pixel_format_name)) return false;
        if (!w.kv_bool("is_configured", false)) return false;  // Mock 阶段
    } else {
        // 无 Profile:全部 null,禁止编造。
        // 字段名与文档 §4.1 / 有 Profile 分支保持一致(print_width_px / pixel_format),
        // 此前误用 print_width_mm/interface,App 按文档解析会缺字段。
        if (!w.kv_null("brand") || !w.kv_null("model") || !w.kv_null("nozzle_count") ||
             !w.kv_null("dpi") || !w.kv_null("print_width_px") ||
             !w.kv_null("print_width_um") ||
            !w.kv_null("pixel_format")) {
            return false;
        }
        if (!w.kv_bool("is_configured", false)) return false;
    }
    return w.end_object();
}

bool write_memory_section(JsonWriter& w, const DeviceInfo& di) noexcept {
    if (!w.key("memory") || !w.begin_object()) return false;
    if (!w.kv_bool("psram_available", di.psram_available)) return false;
    if (di.psram_sizes_valid) {
        if (!w.kv_i64("psram_total_bytes", di.psram_total_bytes)) return false;
        if (!w.kv_i64("psram_free_bytes", di.psram_free_bytes)) return false;
    } else {
        if (!w.kv_null("psram_total_bytes") || !w.kv_null("psram_free_bytes")) return false;
    }
    return w.end_object();
}

bool write_capabilities_section(JsonWriter& w, const DeviceInfo& di) noexcept {
    const PrintProfile* pf = di.profile;
    if (!w.key("capabilities") || !w.begin_object()) return false;
    if (!w.kv_cstr("printer_type", pf != nullptr ? pf->type_name : "inkjet")) return false;
    if (!w.kv_bool("color", pf != nullptr ? pf->pixel_format != PixelFormat::Mono1
                                          : true)) return false;
    if (!w.key("data_modes") || !w.begin_array()) return false;
    if (pf == nullptr || has_mode(pf->data_modes, DataMode::Raster)) {
        if (!w.value_cstr("raster")) return false;
    }
    if (pf != nullptr && has_mode(pf->data_modes, DataMode::EscPos)) {
        if (!w.value_cstr("escpos")) return false;
    }
    if (!w.end_array()) return false;
    if (!w.key("compressions") || !w.begin_array()) return false;
    if (!w.value_cstr("none")) return false;
    if (pf == nullptr || pf->supports_lz4) {
        if (!w.value_cstr("lz4_block")) return false;
    }
    if (!w.end_array()) return false;
    if (!w.key("integrity") || !w.begin_array()) return false;
    if (!w.value_cstr("none") || !w.value_cstr("crc32")) return false;
    if (!w.end_array()) return false;
    if (!w.kv_bool("paper_feed", di.cap_paper_feed)) return false;
    if (!w.kv_bool("cutter", di.cap_cutter)) return false;
    if (!w.kv_bool("mock_print", di.cap_mock_print)) return false;
    if (!w.kv_bool("crc32", true)) return false;
    if (!w.kv_bool("ble_data_transport", di.services.ble)) return false;
    if (!w.key("services") || !w.begin_object()) return false;
    if (!w.kv_bool("wifi", di.services.wifi) ||
        !w.kv_bool("ble", di.services.ble) ||
         !w.kv_bool("ijpd_tcp", di.services.ijpd_tcp) ||
         !w.kv_bool("escpos_raw_tcp", di.services.escpos_raw_tcp) ||
         !w.kv_bool("mqtt", di.services.mqtt) ||
#if defined(IJPD_AIRPRINT_ENABLED)
         !w.kv_bool("airprint", di.services.airprint) ||
#endif
         !w.end_object()) return false;
    if (!w.key("service_ports") || !w.begin_object()) return false;
    if (di.services.escpos_raw_tcp
            ? !w.kv_i64("escpos_raw_tcp", di.escpos_raw_tcp_port)
            : !w.kv_null("escpos_raw_tcp")) return false;
    if (!w.end_object()) return false;
    // Profile 传感器上报
    if (!w.key("sensors") || !w.begin_array()) return false;
    if (pf != nullptr) {
        if (has_sensor(pf->sensors, SensorFlags::PaperOut) && !w.value_cstr("paper_out"))
            return false;
        if (has_sensor(pf->sensors, SensorFlags::CoverOpen) && !w.value_cstr("cover_open"))
            return false;
        if (has_sensor(pf->sensors, SensorFlags::Overtemp) && !w.value_cstr("overtemp"))
            return false;
        if (has_sensor(pf->sensors, SensorFlags::PaperJam) && !w.value_cstr("paper_jam"))
            return false;
    }
    if (!w.end_array()) return false;
    return w.end_object();
}

bool write_limits_section(JsonWriter& w) noexcept {
    if (!w.key("limits") || !w.begin_object()) return false;
    if (!w.kv_i64("max_control_frame", static_cast<int64_t>(cfg::MAX_CONTROL_FRAME_SIZE))) return false;
    if (!w.kv_i64("max_packet_compressed", static_cast<int64_t>(cfg::MAX_PACKET_COMPRESSED_SIZE))) return false;
    if (!w.kv_i64("max_packet_uncompressed", static_cast<int64_t>(cfg::MAX_PACKET_UNCOMPRESSED_SIZE))) return false;
    if (!w.kv_i64("max_job_bytes", static_cast<int64_t>(cfg::MAX_JOB_BYTES))) return false;
    if (!w.kv_i64("max_total_packets", static_cast<int64_t>(cfg::MAX_TOTAL_PACKETS))) return false;
    if (!w.kv_i64("queue_depth", static_cast<int64_t>(cfg::SLOT_COUNT))) return false;
    if (!w.kv_i64("control_port", cfg::CONTROL_TCP_PORT)) return false;
    if (!w.kv_i64("data_port", cfg::DATA_TCP_PORT)) return false;
    return w.end_object();
}

bool write_commands_section(JsonWriter& w, const DeviceInfo& di) noexcept {
    if (!w.key("commands") || !w.begin_array()) return false;
    for (size_t i = 0; i < command_count(); ++i) {
        const CommandSpec* c = command_at(i);
        if (c == nullptr || !write_command_metadata(w, *c, command_supported(di, *c))) {
            return false;
        }
    }
    return w.end_array();
}

}  // namespace

Err write_device_info_result(JsonWriter& w, const DeviceInfo& di) noexcept {
    if (!w.begin_object()) return Err::INTERNAL_ERROR;
    if (!write_device_section(w, di)) return Err::INTERNAL_ERROR;
    if (!write_printhead_section(w, di.profile)) return Err::INTERNAL_ERROR;
    if (!write_memory_section(w, di)) return Err::INTERNAL_ERROR;
    if (!write_capabilities_section(w, di)) return Err::INTERNAL_ERROR;
    if (!write_limits_section(w)) return Err::INTERNAL_ERROR;
    if (!write_commands_section(w, di)) return Err::INTERNAL_ERROR;
    if (!w.end_object()) return Err::INTERNAL_ERROR;
    return w.ok() ? Err::OK : Err::INTERNAL_ERROR;
}

}  // namespace ijpd
