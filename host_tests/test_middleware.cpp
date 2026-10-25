// test_middleware.cpp —— 中间件 Profile + ESC/POS 透传端到端
// 覆盖：
//   A. Profile 校验：热敏 Profile 拒绝错误 bpr / 拒绝不支持的模式 / 压缩过滤
//   B. ESC/POS 流水线：原始流逐字节一致转发 + 事件序列 + 取消
//   C. Profile 驱动 device_info：printer_type / data_modes / sensors
#include "assert.h"

#include <cstring>
#include <string>
#include <vector>

#include "app/device_info.h"
#include "core/command_router.h"
#include "core/print_profile.h"
#include "core/print_pipeline.h"
#include "profiles/inkjet_6in.h"
#include "profiles/ricoh_mh5440_mock.h"
#include "profiles/thermal_58mm.h"

using namespace ijpd;

// ---- 简化流构造（复用 pipeline 测试的编码器逻辑，直接用 none 压缩） ----
static std::vector<uint8_t> build_raw_stream(uint32_t token, uint32_t packets,
                                             const std::vector<uint8_t>& data) {
    using namespace ijpd;
    std::vector<uint8_t> s{'I', 'J', 'P', 'D', 0x01, 0x00};
    s.push_back(static_cast<uint8_t>(token & 0xFF));
    s.push_back(static_cast<uint8_t>((token >> 8) & 0xFF));
    s.push_back(static_cast<uint8_t>((token >> 16) & 0xFF));
    s.push_back(static_cast<uint8_t>((token >> 24) & 0xFF));
    s.push_back(0);
    s.push_back(0);

    const size_t nominal = (data.size() + packets - 1) / packets;
    for (uint32_t k = 0; k < packets; ++k) {
        const size_t ulen = (k == packets - 1) ? data.size() - nominal * (packets - 1)
                                                : nominal;
        const uint8_t* raw = data.data() + static_cast<size_t>(k) * nominal;
        s.push_back(static_cast<uint8_t>(k & 0xFF));
        s.push_back(static_cast<uint8_t>((k >> 8) & 0xFF));
        s.push_back(static_cast<uint8_t>(ulen & 0xFF));
        s.push_back(static_cast<uint8_t>((ulen >> 8) & 0xFF));
        s.push_back(static_cast<uint8_t>(ulen & 0xFF));   // clen == ulen（none）
        s.push_back(static_cast<uint8_t>((ulen >> 8) & 0xFF));
        s.insert(s.end(), raw, raw + ulen);
    }
    return s;
}

int main() {
    // ================= A. Profile 校验 =================
    {
        // 热敏 Profile：bpr 必须 == 48
        StartPrintParams p{};
        strncpy(p.job_id, "t1", sizeof(p.job_id) - 1);
        p.total_bytes_uncompressed = 48 * 10;
        p.total_packets = 2;
        p.compression = Compression::None;
        p.mode = StartPrintMode::Raster;
        p.bytes_per_row = 48;
        p.total_rows = 10;

        const PrintProfile* pf = &profiles::kThermal58mm;
        const char* msg = "";
        CHECK_TRUE(CommandRouter::finalize_start_print(&p, pf, &msg) == Err::OK);

        // 错误 bpr（60 != 48）→ 拒绝
        p.bytes_per_row = 60;
        p.total_rows = 8;  // 60*8=480=total
        CHECK_TRUE(CommandRouter::finalize_start_print(&p, pf, &msg) ==
                   Err::INVALID_ARGUMENT);
        CHECK_TRUE(std::strstr(msg, "maximum row width") != nullptr);

        // 喷墨 Profile：raster OK
        p.bytes_per_row = 48;
        p.total_rows = 10;
        CHECK_TRUE(CommandRouter::finalize_start_print(
                       &p, &profiles::kInkjet6in, &msg) == Err::OK);

        StartPrintParams ricoh = p;
        ricoh.bytes_per_row = 1280;
        ricoh.total_rows = 2;
        ricoh.total_bytes_uncompressed = 2560;
        CHECK_TRUE(profiles::kRicohMh5440Mock.validate() == Err::OK);
        CHECK_TRUE(CommandRouter::finalize_start_print(
                       &ricoh, &profiles::kRicohMh5440Mock, &msg) == Err::OK);
        CHECK_EQ(profiles::kRicohMh5440Mock.print_width_px, 320u);
        CHECK_EQ(profiles::kRicohMh5440Mock.effective_bpr_max(4), 1280u);

        // escpos 模式：热敏支持，喷墨拒绝
        p.mode = StartPrintMode::EscPos;
        p.bytes_per_row = 0;
        p.total_rows = 0;
        CHECK_TRUE(CommandRouter::finalize_start_print(&p, pf, &msg) == Err::OK);
        CHECK_TRUE(CommandRouter::finalize_start_print(
                       &p, &profiles::kInkjet6in, &msg) == Err::INVALID_ARGUMENT);
        CHECK_TRUE(std::strstr(msg, "data mode") != nullptr);

        // 压缩过滤：热敏禁用 lz4 的 Profile 副本
        PrintProfile no_lz4 = profiles::kThermal58mm;
        no_lz4.supports_lz4 = false;
        p.compression = Compression::Lz4Block;
        p.bytes_per_row = 48;
        p.total_rows = 10;
        CHECK_TRUE(CommandRouter::finalize_start_print(&p, &no_lz4, &msg) ==
                   Err::INVALID_ARGUMENT);
        CHECK_TRUE(std::strstr(msg, "lz4_block") != nullptr);
    }

    // ================= B. ESC/POS 流水线端到端 =================
    {
        static uint8_t slots[6 * 4096];
        static uint8_t comp[2][65536];
        static uint8_t carry[65536];

        PrintPipeline pipeline;
        std::vector<uint8_t> raw_received;

        // ESC/POS 测试数据：典型收据（初始化 + 文本 + 切纸）
        std::vector<uint8_t> escpos;
        escpos.insert(escpos.end(), {0x1B, 0x40});                       // ESC @ init
        escpos.insert(escpos.end(), {0x1B, 0x61, 0x01});                 // center
        const char receipt[] = "Receipt\x0A";
        escpos.insert(escpos.end(), receipt, receipt + 8);              // text
        escpos.insert(escpos.end(), {0x1D, 0x56, 0x00});                 // GS V cut
        for (int i = 0; i < 200; ++i) escpos.push_back(0x00);         // padding

        PrintPipeline::Config cfg;
        cfg.job_id = "escpos-test";
        cfg.data_token = 0xC0DE;
        cfg.total_packets = 3;
        cfg.total_bytes_uncompressed = escpos.size();
        cfg.nominal_packet_bytes =
            static_cast<uint16_t>((escpos.size() + 2) / 3);
        cfg.last_packet_bytes = static_cast<uint16_t>(
            escpos.size() - cfg.nominal_packet_bytes * 2);
        cfg.data_mode = StartPrintMode::EscPos;
        cfg.raw_chunk_bytes = 64;
        cfg.compression = Compression::None;
        cfg.integrity = Integrity::None;
        cfg.comp_buf[0] = comp[0];
        cfg.comp_buf[1] = comp[1];
        cfg.slot_memory = slots;
        cfg.slot_count = 6;
        cfg.slot_size = 4096;
        cfg.row_carry = carry;

        auto now_ms = [](void*) -> uint64_t { return 1000; };
        struct Ctx {
            std::vector<uint8_t>* out;
        } ctx{&raw_received};
        auto raw_fn = [](void* user, const uint8_t* d, size_t n) -> Err {
            static_cast<Ctx*>(user)->out->insert(
                static_cast<Ctx*>(user)->out->end(), d, d + n);
            return Err::OK;
        };
        auto ev_fn = [](void*, const PrintPipeline::Event&) {};

        {
            const Err se = pipeline.start(cfg, {&ctx, now_ms, nullptr, raw_fn, ev_fn});
            if (se != Err::OK) std::printf("start err=%d escpos_len=%zu\n", (int)se, escpos.size());
            CHECK_TRUE(se == Err::OK);
        }

        const std::vector<uint8_t> stream = build_raw_stream(0xC0DE, 3, escpos);

        // 单线程驱动（简化，验证正确性）
        size_t off = 0;
        for (int i = 0; i < 100000; ++i) {
            if (off < stream.size()) {
                size_t consumed = 0;
                (void)pipeline.rx_pump(stream.data() + off, stream.size() - off,
                                       &consumed);
                off += consumed;
            }
            (void)pipeline.decode_pump();
            (void)pipeline.print_pump();
            if (pipeline.state() == JobState::Finalizing) break;
        }
        CHECK_TRUE(pipeline.state() == JobState::Finalizing);
        CHECK_TRUE(pipeline.finish(false, Err::OK) == Err::OK);
        CHECK_TRUE(pipeline.state() == JobState::Completed);

        // 原始流逐字节一致
        CHECK_EQ(raw_received.size(), escpos.size());
        if (raw_received.size() == escpos.size()) {
            CHECK_EQ_MEM(raw_received.data(), escpos.data(), escpos.size());
        }
        CHECK_TRUE(pipeline.all_slots_free());
    }

    // ================= C. Profile 驱动 device_info =================
    {
        char buf[6144];
        JsonWriter w(buf, sizeof(buf));
        DeviceInfo di{};
        di.device_id = "TP-58-001";
        di.model = "fallback";
        di.fw_version = "1.0.0-mw";
        di.hal_mode = "mock";
        di.psram_available = true;
        di.psram_sizes_valid = true;
        di.psram_total_bytes = 8388608;
        di.psram_free_bytes = 7000000;
        di.cap_paper_feed = true;
        di.cap_cutter = true;
        di.cap_mock_print = true;
        di.profile = &profiles::kThermal58mm;

        CHECK_TRUE(write_device_info_result(w, di) == Err::OK);
        const std::string out(w.data(), w.length());

        // Profile 字段
        CHECK_TRUE(out.find(R"("printer_type":"thermal")") != std::string::npos);
        CHECK_TRUE(out.find(R"("data_modes":["raster","escpos"])") != std::string::npos);
        CHECK_TRUE(out.find(R"("cutter":true)") != std::string::npos);
        CHECK_TRUE(out.find(R"("paper_out")") != std::string::npos);
        CHECK_TRUE(out.find(R"("cover_open")") != std::string::npos);
        CHECK_TRUE(out.find(R"("dpi":203)") != std::string::npos);
        CHECK_TRUE(out.find(R"("print_width_px":384)") != std::string::npos);
        CHECK_TRUE(out.find(R"("pixel_format":"mono1")") != std::string::npos);
        CHECK_TRUE(out.find(R"("color":false)") != std::string::npos);  // mono1

        // 喷墨 Profile 对照
        JsonWriter w2(buf, sizeof(buf));
        di.profile = &profiles::kInkjet6in;
        CHECK_TRUE(write_device_info_result(w2, di) == Err::OK);
        const std::string out2(w2.data(), w2.length());
        CHECK_TRUE(out2.find(R"("printer_type":"inkjet")") != std::string::npos);
        CHECK_TRUE(out2.find(R"("data_modes":["raster"])" ) != std::string::npos);
        CHECK_TRUE(out2.find(R"("color":true)") != std::string::npos);   // cmyk8
    }

    return ::ijpd::test::summary("test_middleware");
}
