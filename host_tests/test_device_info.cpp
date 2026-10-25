// test_device_info.cpp —— get_device_info result 组装验证
// 说明：出站大响应（数百 token）不受入站解析器 64-token/深度 4 限制约束
//（协议 §3.4），固件解析器不用于回环解析自身响应；此处采用确定性
// 子串/窗口断言（写出器键序与格式固定，输出可复现）。
#include "app/device_info.h"
#include "assert.h"

#include <cstring>
#include <string>

#include "core/command_schema.h"
#include "core/config_limits.h"

using namespace ijpd;

namespace {

size_t count_of(const std::string& hay, const char* needle) {
    size_t n = 0;
    size_t pos = 0;
    const size_t nl = std::strlen(needle);
    while ((pos = hay.find(needle, pos)) != std::string::npos) {
        ++n;
        pos += nl;
    }
    return n;
}

// 在 from 之后 window 字节内查找 sub
bool near_after(const std::string& hay, const char* anchor, const char* sub,
                size_t window = 160) {
    const size_t at = hay.find(anchor);
    if (at == std::string::npos) return false;
    return hay.find(sub, at) != std::string::npos &&
           hay.find(sub, at) - at <= window;
}

DeviceInfo mock_di() {
    DeviceInfo di{};
    di.device_id = "IJPD-TEST-01";
    di.model = "IJPD-P61-MOCK";
    di.fw_version = "0.1.0-phase2";
    di.hal_mode = "mock";
    di.psram_available = true;
    di.psram_sizes_valid = true;
    di.psram_total_bytes = 8 * 1024 * 1024;
    di.psram_free_bytes = 7 * 1024 * 1024;
    di.cap_paper_feed = true;
    di.cap_cutter = false;
    di.cap_mock_print = true;
    return di;
}

std::string build(const DeviceInfo& di) {
    char buf[cfg::MAX_RESPONSE_FRAME_SIZE];
    JsonWriter w(buf, sizeof(buf));
    CHECK_TRUE(write_device_info_result(w, di) == Err::OK);
    CHECK_TRUE(w.ok());
    return std::string(w.data(), w.length());
}

}  // namespace

int main() {
    // ---- 1. 写出成功与量级 ----
    const std::string out = build(mock_di());
    CHECK_TRUE(out.size() > 2000u);

    // ---- 2. 各节确定性子串（键序与写出器实现一致） ----
    CHECK_TRUE(out.find(R"("device":{"device_id":"IJPD-TEST-01","model":"IJPD-P61-MOCK",)") !=
               std::string::npos);
    CHECK_TRUE(out.find(R"("fw_version":"0.1.0-phase2","protocol_version":"1.0")") !=
               std::string::npos);
    CHECK_TRUE(out.find(R"("connections":["ble","wifi_tcp"],"hal_mode":"mock")") !=
               std::string::npos);

    // printhead：未知 -> null + is_configured:false（禁止编造）
    CHECK_TRUE(out.find(R"("printhead":{"brand":null,"model":null,"nozzle_count":null,)"
                        R"("dpi":null,"print_width_px":null,"pixel_format":null,)"
                        R"("is_configured":false)") != std::string::npos);

    // memory
    CHECK_TRUE(out.find(R"("memory":{"psram_available":true,"psram_total_bytes":8388608,)"
                        R"("psram_free_bytes":7340032)") != std::string::npos);
    {
        DeviceInfo di = mock_di();
        di.psram_sizes_valid = false;
        const std::string o2 = build(di);
        CHECK_TRUE(o2.find(R"("psram_total_bytes":null)") != std::string::npos);
        CHECK_TRUE(o2.find(R"("psram_free_bytes":null)") != std::string::npos);
    }

    // capabilities（Profile 驱动：printer_type + data_modes + sensors）
    CHECK_TRUE(out.find(R"("printer_type":"inkjet")") != std::string::npos);
    CHECK_TRUE(out.find(R"("data_modes":[")") != std::string::npos);
    CHECK_TRUE(out.find(R"("paper_feed":true)") != std::string::npos);
    CHECK_TRUE(out.find(R"("cutter":false)") != std::string::npos);
    // sensors 无 Profile 时为空数组，不额外断言

    // limits（与 config_limits 同源）
    CHECK_TRUE(out.find(R"("limits":{"max_control_frame":2048,)"
                        R"("max_packet_compressed":65535,)"
                        R"("max_packet_uncompressed":65535,)"
                        R"("max_job_bytes":67108864,)"
                        R"("max_total_packets":65535,)"
                        R"("queue_depth":6,"control_port":9100,"data_port":9101)") !=
               std::string::npos);

    // ---- 3. commands 元数据与 schema 同源 ----
    CHECK_EQ(count_of(out, "\"cmd\":\""), 7u);
    CHECK_EQ(count_of(out, "\"key\":\""), 15u);  // start_print增加transport
    CHECK_TRUE(near_after(out, "\"cmd\":\"cut_paper\"", "\"supported\":false"));
    CHECK_TRUE(near_after(out, "\"cmd\":\"feed_paper\"", "\"supported\":true"));
    CHECK_TRUE(near_after(out, "\"cmd\":\"start_print\"", "\"supported\":true"));
    CHECK_TRUE(near_after(out, "\"cmd\":\"get_device_info\"", "\"supported\":true"));
    // start_print 首参数（schema 顺序）
    CHECK_TRUE(out.find(R"("key":"job_id","label":"任务 ID","type":"string","required":true)") !=
               std::string::npos);
    CHECK_TRUE(out.find(R"("key":"total_bytes_uncompressed")") != std::string::npos);
    CHECK_TRUE(out.find(R"("default_value":"none")") != std::string::npos);  // integrity 缺省
    CHECK_TRUE(out.find(R"("options":["none","lz4_block"])") != std::string::npos);
    CHECK_TRUE(out.find(R"("max":67108864)") != std::string::npos);

    // ---- 4. PSRAM 不可用：start_print supported=false（与门控同源） ----
    {
        DeviceInfo di = mock_di();
        di.psram_available = false;
        const std::string o3 = build(di);
        CHECK_TRUE(near_after(o3, "\"cmd\":\"start_print\"", "\"supported\":false"));
        CHECK_TRUE(o3.find(R"("psram_available":false)") != std::string::npos);
    }

    // ---- 5. 缓冲不足：失败且不截断 ----
    {
        char small[64];
        JsonWriter w(small, sizeof(small));
        CHECK_TRUE(write_device_info_result(w, mock_di()) == Err::INTERNAL_ERROR);
        CHECK_TRUE(!w.ok());
    }

    return ::ijpd::test::summary("test_device_info");
}
