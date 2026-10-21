// test_print_pipeline.cpp —— 打印流水线端到端（主机侧，3 线程驱动三泵）
// 覆盖（任务书 12.3 / Phase 6 验收的主机可验证部分）：
//   A. 正常闭环：事件序列、终态统计、槽位全回收、打印内容逐字节一致
//      （含跨包行边界、lz4 与存储兜底、CRC、末包短包）
//   B. 取消：接收中途取消 -> CANCELLED + 槽位回收
//   C. 致命：CRC 损坏 -> PACKET_CRC_MISMATCH；行回调故障 -> HAL_FAULT
//   D. 连续 100 次任务：无泄漏（槽位/缓冲全回收）
#include "core/print_pipeline.h"
#include "assert.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "core/byte_order.h"
#include "core/crc32.h"

using namespace ijpd;

namespace {

// ---- 测试用 LZ4 编码器（与 test_lz4_block 相同算法，简化副本） ----
struct Enc {
    static constexpr size_t kH = 1 << 12;
    static uint32_t h4(const uint8_t* p) {
        const uint32_t v = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                           (static_cast<uint32_t>(p[2]) << 16) |
                           (static_cast<uint32_t>(p[3]) << 24);
        return (v * 2654435761u) & (kH - 1);
    }
    static std::vector<uint8_t> encode(const uint8_t* src, size_t n) {
        std::vector<uint8_t> out;
        std::vector<int32_t> tab(kH, -1);
        size_t anchor = 0, ip = 0;
        const size_t lim = (n >= 12) ? n - 12 : 0;
        auto emit = [&](const uint8_t* lit, size_t llen, size_t off, size_t m4) {
            const uint8_t ln = llen < 15 ? static_cast<uint8_t>(llen) : 15;
            const uint8_t mn = m4 < 15 ? static_cast<uint8_t>(m4) : 15;
            out.push_back(static_cast<uint8_t>((ln << 4) | mn));
            if (ln == 15) {
                size_t r = llen - 15;
                while (r >= 255) { out.push_back(255); r -= 255; }
                out.push_back(static_cast<uint8_t>(r));
            }
            out.insert(out.end(), lit, lit + llen);
            if (off != 0) {
                const size_t at = out.size();
                out.resize(at + 2);
                write_u16_le(out.data() + at, static_cast<uint16_t>(off));
                if (mn == 15) {
                    size_t r = m4 - 15;
                    while (r >= 255) { out.push_back(255); r -= 255; }
                    out.push_back(static_cast<uint8_t>(r));
                }
            }
        };
        while (ip < lim) {
            const uint32_t h = h4(src + ip);
            const int32_t ref = tab[h];
            tab[h] = static_cast<int32_t>(ip);
            if (ref >= 0 && ip - static_cast<size_t>(ref) <= 0xFFFF &&
                std::memcmp(src + ref, src + ip, 4) == 0) {
                size_t ml = 4;
                while (ip + ml + 1 < n && src[ref + ml] == src[ip + ml]) ++ml;
                emit(src + anchor, ip - anchor, ip - static_cast<size_t>(ref), ml - 4);
                ip += ml;
                anchor = ip;
            } else {
                ++ip;
            }
        }
        emit(src + anchor, n - anchor, 0, 0);
        return out;
    }
};

// ---- 任务流构造（Hello + 帧） ----
struct JobSpec {
    size_t total_bytes;
    uint32_t packets;
    size_t bpr;
    Compression comp;
    Integrity integ;
    uint32_t token;
};

std::vector<uint8_t> build_stream(const JobSpec& js, const std::vector<uint8_t>& data,
                                  bool corrupt_last_crc) {
    const uint32_t packets = js.packets;
    const size_t nominal = (js.total_bytes + packets - 1) / packets;
    const size_t last = js.total_bytes - nominal * (packets - 1);

    std::vector<uint8_t> s{'I', 'J', 'P', 'D'};
    uint8_t t[4];
    write_u16_le(t, cfg::PROTOCOL_HELLO_VERSION);
    s.insert(s.end(), t, t + 2);
    write_u32_le(t, js.token);
    s.insert(s.end(), t, t + 4);
    s.push_back(0);
    s.push_back(0);

    for (uint32_t k = 0; k < packets; ++k) {
        const size_t ulen = (k == packets - 1) ? last : nominal;
        const uint8_t* raw = data.data() + static_cast<size_t>(k) * nominal;
        std::vector<uint8_t> comp;
        if (js.comp == Compression::Lz4Block) {
            comp = Enc::encode(raw, ulen);
            if (comp.size() >= ulen) comp.assign(raw, raw + ulen);  // 存储兜底
        } else {
            comp.assign(raw, raw + ulen);
        }
        std::vector<uint8_t> f(6);
        write_u16_le(f.data(), static_cast<uint16_t>(k));
        write_u16_le(f.data() + 2, static_cast<uint16_t>(ulen));
        write_u16_le(f.data() + 4, static_cast<uint16_t>(comp.size()));
        f.insert(f.end(), comp.begin(), comp.end());
        if (js.integ == Integrity::Crc32) {
            if (corrupt_last_crc && k == packets - 1) {
                uint8_t c[4];
                write_u32_le(c, crc32(f.data(), f.size()) ^ 0x1);  // 单比特损坏
                f.insert(f.end(), c, c + 4);
            } else {
                uint8_t c[4];
                write_u32_le(c, crc32(f.data(), f.size()));
                f.insert(f.end(), c, c + 4);
            }
        }
        s.insert(s.end(), f.begin(), f.end());
    }
    return s;
}

// ---- 测试环境 ----
struct Env {
    uint8_t slots[cfg::SLOT_COUNT * 256];   // 测试槽 256B（乘积小于 64KB 即可）
    uint8_t comp[2][cfg::COMPRESSED_BUF_SIZE > 4096 ? 4096 : cfg::COMPRESSED_BUF_SIZE];
    // 压缩测试缓冲只需容纳测试载荷（<= 帧长），pipeline 校验 cap >= MAX+1 —— 注意！
};

// pipeline 要求 set_payload_buffer cap >= MAX_PACKET_COMPRESSED_SIZE —— 直接给全尺寸
struct BigEnv {
    static constexpr size_t kSlotSize = 4096;
    uint8_t slots[cfg::SLOT_COUNT * kSlotSize];
    uint8_t comp[2][cfg::MAX_PACKET_COMPRESSED_SIZE + 1];
    uint8_t carry[65536];
};

struct Harness {
    BigEnv env;
    PrintPipeline pipeline;
    std::vector<uint8_t> printed;      // 行内容按序拼接（print 线程单写）
    std::vector<PrintPipeline::Event> events;  // 互斥保护（事件来自多线程）
    std::mutex events_mu;
    std::atomic<int> print_row_err_after{-1};  // 故障注入：N 行后回调报错
    std::atomic<uint64_t> now{1000};    // 假时钟（多线程读/主线程写）
    std::atomic<uint32_t> rows{0};

    static uint64_t now_fn(void* user) {
        return static_cast<Harness*>(user)->now.load(std::memory_order_relaxed);
    }
    static Err row_fn(void* user, const uint8_t* row, size_t len, uint32_t idx) {
        auto* h = static_cast<Harness*>(user);
        (void)idx;
        if (h->print_row_err_after >= 0 && h->rows.load() >=
            static_cast<uint32_t>(h->print_row_err_after.load())) {
            return Err::HAL_FAULT;
        }
        h->printed.insert(h->printed.end(), row, row + len);
        h->rows.fetch_add(1);
        return Err::OK;
    }
    static void event_fn(void* user, const PrintPipeline::Event& ev) {
        auto* h = static_cast<Harness*>(user);
        std::lock_guard<std::mutex> lk(h->events_mu);
        h->events.push_back(ev);
    }

    // escpos 模式：收集原始流
    std::vector<uint8_t> raw_out;
    static Err raw_fn(void* user, const uint8_t* data, size_t len) {
        auto* h = static_cast<Harness*>(user);
        h->raw_out.insert(h->raw_out.end(), data, data + len);
        return Err::OK;
    }

    PrintPipeline::Config make_cfg(const JobSpec& js, const char* job_id) {
        PrintPipeline::Config cfg;
        cfg.job_id = job_id;
        cfg.data_token = js.token;
        cfg.total_packets = js.packets;
        cfg.total_bytes_uncompressed = js.total_bytes;
        const uint32_t p = js.packets;
        const uint64_t tb = js.total_bytes;
        cfg.nominal_packet_bytes =
            static_cast<uint16_t>((tb + p - 1) / p);
        cfg.last_packet_bytes =
            static_cast<uint16_t>(tb - cfg.nominal_packet_bytes * (p - 1));
        cfg.bytes_per_row = js.bpr;
        cfg.total_rows = static_cast<uint32_t>(tb / js.bpr);
        cfg.compression = js.comp;
        cfg.integrity = js.integ;
        cfg.data_mode = StartPrintMode::Raster;  // 默认；escpos 测试单独设
        cfg.comp_buf[0] = env.comp[0];
        cfg.comp_buf[1] = env.comp[1];
        cfg.slot_memory = env.slots;
        cfg.slot_count = cfg::SLOT_COUNT;
        cfg.slot_size = BigEnv::kSlotSize;
        cfg.row_carry = env.carry;
        return cfg;
    }
};

// 三线程驱动直至终态（或超时）
void drive(Harness& h, const std::vector<uint8_t>& stream,
           std::function<void(Harness&)> mid = nullptr) {
    std::atomic<bool> stop{false};
    std::atomic<size_t> rx_off{0};

    std::thread rx([&]() {
        while (!stop && rx_off < stream.size()) {
            size_t chunk = 1 + (rx_off * 7 + 13) % 97;  // 伪随机切块
            if (rx_off + chunk > stream.size()) chunk = stream.size() - rx_off;
            size_t consumed = 0;
            const auto r = h.pipeline.rx_pump(stream.data() + rx_off, chunk, &consumed);
            if (r == PrintPipeline::Pump::Failed) {
                rx_off = stream.size();  // 吸收
                break;
            }
            rx_off += consumed;
            if (consumed == 0) std::this_thread::yield();
        }
    });
    std::thread dec([&]() {
        while (!stop) {
            const auto r = h.pipeline.decode_pump();
            if (r != PrintPipeline::Pump::Progress) std::this_thread::yield();
        }
    });
    std::thread prt([&]() {
        while (!stop) {
            const auto r = h.pipeline.print_pump();
            if (r != PrintPipeline::Pump::Progress) std::this_thread::yield();
        }
    });

    // 等待终态（假时钟由主线程步进）
    for (int i = 0; i < 20000; ++i) {
        if (mid) mid(h);
        h.now.fetch_add(1, std::memory_order_relaxed);
        h.pipeline.tick(h.now);
        if (h.pipeline.terminal() || h.pipeline.state() == JobState::Finalizing) break;
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    stop = true;
    rx.join();
    dec.join();
    prt.join();
}

std::vector<uint8_t> gen_data(size_t n, int kind) {
    std::vector<uint8_t> d(n);
    uint32_t st = kind * 7919 + 13;
    for (size_t i = 0; i < n; ++i) {
        st = st * 1664525u + 1013904223u;
        if (kind == 0) {
            d[i] = static_cast<uint8_t>(st >> 24);
        } else {
            d[i] = static_cast<uint8_t>((kind == 1) ? (i / 37) & 0xFF
                                                    : (st >> 28) ^ (i & 0x0F));
        }
    }
    return d;
}

}  // namespace

int main() {
    static Harness h;

    // ================= A. 正常闭环（多参数组合） =================
    {
        struct Case {
            size_t bytes;
            uint32_t packets;
            size_t bpr;
            Compression comp;
            Integrity integ;
            int kind;
        };
        const std::vector<Case> cases = {
            // 跨包行边界：nominal 333 不是 bpr 的倍数
            {999, 3, 37, Compression::None, Integrity::Crc32, 0},
            {999, 3, 37, Compression::Lz4Block, Integrity::Crc32, 1},
            {1000, 1, 8, Compression::None, Integrity::None, 2},     // 单包
            {4096, 8, 128, Compression::Lz4Block, Integrity::None, 1},  // 可压缩
            {300, 7, 10, Compression::Lz4Block, Integrity::Crc32, 0},   // 随机->存储兜底
            {61 * 51, 9, 61, Compression::None, Integrity::Crc32, 2},  // 3111B/51 行
        };
        int idx = 0;
        for (const auto& c : cases) {
            h.printed.clear();
            h.events.clear();
            h.rows = 0;
            h.print_row_err_after = -1;
            h.now.store(1000 + idx, std::memory_order_relaxed);

            const std::vector<uint8_t> data = gen_data(c.bytes, c.kind);
            JobSpec js{c.bytes, c.packets, c.bpr, c.comp, c.integ,
                       0xAA000000u + static_cast<uint32_t>(idx)};
            const std::vector<uint8_t> stream = build_stream(js, data, false);

            CHECK_TRUE(h.pipeline.start(h.make_cfg(js, "job-e2e"), {&h, Harness::now_fn,
                                                                    Harness::row_fn,
                                                                    Harness::raw_fn, Harness::event_fn}) ==
                       Err::OK);
            drive(h, stream);

            CHECK_TRUE(h.pipeline.state() == JobState::Finalizing);
            CHECK_TRUE(h.pipeline.finish(false, Err::OK) == Err::OK);
            CHECK_TRUE(h.pipeline.state() == JobState::Completed);
            CHECK_TRUE(h.pipeline.all_slots_free());

            // 内容逐字节一致（跨包行重组正确性）
            CHECK_EQ(h.printed.size(), c.bytes);
            CHECK_EQ_MEM(h.printed.data(), data.data(), c.bytes);
            CHECK_EQ(h.rows.load(), static_cast<uint32_t>(c.bytes / c.bpr));

            // 终态事件
            bool has_finish = false;
            for (const auto& ev : h.events) {
                if (ev.kind == PrintPipeline::Event::Kind::Completed) {
                    has_finish = true;
                    CHECK_EQ(ev.stats.packets_received, c.packets);
                    CHECK_EQ(ev.stats.packets_decoded, c.packets);
                    CHECK_EQ(ev.stats.bytes_printed,
                             static_cast<uint64_t>(c.bytes));
                }
            }
            CHECK_TRUE(has_finish);
            ++idx;
        }
    }

    // ================= B. 接收中途取消 =================
    {
        h.printed.clear();
        h.events.clear();
        h.rows = 0;
        const std::vector<uint8_t> data = gen_data(4000, 1);
        JobSpec js{4000, 10, 40, Compression::Lz4Block, Integrity::Crc32, 0x1234};
        const std::vector<uint8_t> stream = build_stream(js, data, false);

        CHECK_TRUE(h.pipeline.start(h.make_cfg(js, "job-cancel"), {&h, Harness::now_fn,
                                                                   Harness::row_fn,
                                                                   Harness::raw_fn, Harness::event_fn}) ==
                   Err::OK);
        std::atomic<bool> cancelled{false};
        drive(h, stream, [&](Harness& hh) {
            if (!cancelled && hh.pipeline.snapshot().packets_received >= 3) {
                hh.pipeline.cancel();
                cancelled = true;
            }
        });
        CHECK_TRUE(cancelled);
        CHECK_TRUE(h.pipeline.state() == JobState::Cancelled);
        CHECK_TRUE(h.pipeline.last_error() == Err::JOB_CANCELLED);
        CHECK_TRUE(h.pipeline.all_slots_free());
        bool has_cancel_ev = false;
        for (const auto& ev : h.events) {
            if (ev.kind == PrintPipeline::Event::Kind::Cancelled) has_cancel_ev = true;
        }
        CHECK_TRUE(has_cancel_ev);
    }

    // ================= C. 致命错误路径 =================
    {
        // CRC 损坏（末包）
        h.printed.clear();
        h.events.clear();
        const std::vector<uint8_t> data = gen_data(600, 0);
        JobSpec js{600, 3, 20, Compression::None, Integrity::Crc32, 0x77};
        const std::vector<uint8_t> stream = build_stream(js, data, true);
        CHECK_TRUE(h.pipeline.start(h.make_cfg(js, "job-crc"), {&h, Harness::now_fn,
                                                                Harness::row_fn,
                                                                Harness::raw_fn, Harness::event_fn}) ==
                   Err::OK);
        drive(h, stream);
        CHECK_TRUE(h.pipeline.state() == JobState::Error);
        CHECK_TRUE(h.pipeline.last_error() == Err::PACKET_CRC_MISMATCH);
        CHECK_TRUE(h.pipeline.all_slots_free());

        // 行回调故障 -> HAL_FAULT
        h.printed.clear();
        h.events.clear();
        h.rows = 0;
        h.print_row_err_after = 3;
        const std::vector<uint8_t> data2 = gen_data(500, 0);
        JobSpec js2{500, 1, 10, Compression::None, Integrity::None, 0x88};
        const std::vector<uint8_t> stream2 = build_stream(js2, data2, false);
        CHECK_TRUE(h.pipeline.start(h.make_cfg(js2, "job-hal"), {&h, Harness::now_fn,
                                                                 Harness::row_fn,
                                                                 Harness::raw_fn, Harness::event_fn}) ==
                   Err::OK);
        drive(h, stream2);
        CHECK_TRUE(h.pipeline.state() == JobState::Error);
        CHECK_TRUE(h.pipeline.last_error() == Err::HAL_FAULT);
        CHECK_TRUE(h.pipeline.all_slots_free());
        h.print_row_err_after = -1;
    }

    // ================= D. 连续 100 次任务（无泄漏） =================
    {
        for (int i = 0; i < 100; ++i) {
            h.printed.clear();
            h.events.clear();
            h.rows = 0;
            const size_t bpr = static_cast<size_t>(10 + (i % 7));
            const size_t rows = 12 + static_cast<size_t>(i % 9);
            const size_t bytes = bpr * rows;  // 协议要求 bpr*rows == total
            const std::vector<uint8_t> data = gen_data(bytes, i % 3);
            JobSpec js{bytes, static_cast<uint32_t>(2 + (i % 4)), bpr,
                       (i % 2) ? Compression::Lz4Block : Compression::None,
                       Integrity::Crc32, 0xB0000000u + static_cast<uint32_t>(i)};
            const std::vector<uint8_t> stream = build_stream(js, data, false);
            const Err dse = h.pipeline.start(h.make_cfg(js, "loop"), {&h, Harness::now_fn,
                                                                     Harness::row_fn,
                                                                     Harness::raw_fn, Harness::event_fn});
            CHECK_TRUE(dse == Err::OK);
            drive(h, stream);
            CHECK_TRUE(h.pipeline.state() == JobState::Finalizing);
            CHECK_TRUE(h.pipeline.finish(false, Err::OK) == Err::OK);
            if (!h.pipeline.all_slots_free()) {
                CHECK_TRUE(false);  // 槽位泄漏
                break;
            }
        }
        CHECK_TRUE(h.pipeline.all_slots_free());
    }

    return ::ijpd::test::summary("test_print_pipeline");
}
