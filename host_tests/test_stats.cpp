// test_stats.cpp —— 计数 / 水位 / 吞吐窗口 / 复位
#include "core/stats.h"
#include "assert.h"

#include <atomic>
#include <thread>

using namespace ijpd;

int main() {
    // ---- 基本计数与快照 ----
    {
        Stats st;
        st.begin(1000);
        st.on_packet_received();
        st.on_packet_received();
        st.on_packet_decoded(1500);
        st.on_packet_decoded(2500);
        st.on_bytes_printed(3000);
        st.on_crc_error();
        st.on_decode_error();
        st.on_decode_error();
        st.on_queue_watermark(3);
        st.on_queue_watermark(5);
        st.on_queue_watermark(4);  // 回落不改 max

        auto s = st.snapshot();
        CHECK_EQ(s.packets_received, 2u);
        CHECK_EQ(s.packets_decoded, 2u);
        CHECK_EQ(s.bytes_decoded, 4000u);
        CHECK_EQ(s.bytes_printed, 3000u);
        CHECK_EQ(s.crc_errors, 1u);
        CHECK_EQ(s.decode_errors, 2u);
        CHECK_EQ(s.queue_watermark, 4u);
        CHECK_EQ(s.queue_watermark_max, 5u);
        CHECK_EQ(st.start_ms(), 1000u);
    }

    // ---- 吞吐窗口 ----
    {
        Stats st;
        st.begin(0);
        st.on_packet_decoded(5000);              // 窗口内 5000B
        st.refresh(100);                          // < 200ms：不结算
        CHECK_EQ(st.snapshot().throughput_bps, 0u);
        st.refresh(200);                          // 恰 200ms：结算 5000*1000/200 = 25000
        CHECK_EQ(st.snapshot().throughput_bps, 25000u);
        st.on_packet_decoded(1000);
        st.refresh(400);                          // 窗口 200ms，1000B -> 5000
        CHECK_EQ(st.snapshot().throughput_bps, 5000u);
        st.refresh(1000);                         // 无新字节 -> 0
        CHECK_EQ(st.snapshot().throughput_bps, 0u);
    }

    // ---- 复位 ----
    {
        Stats st;
        st.begin(50);
        st.on_packet_received();
        st.on_queue_watermark(6);
        st.begin(2000);  // 新任务：全部复位
        auto s = st.snapshot();
        CHECK_EQ(s.packets_received, 0u);
        CHECK_EQ(s.queue_watermark_max, 0u);
        CHECK_EQ(st.start_ms(), 2000u);
    }

    // ---- 并发计数（多线程原子性） ----
    {
        Stats st;
        st.begin(0);
        std::atomic<uint64_t> expect_bytes{0};
        std::thread producers[4];
        for (int t = 0; t < 4; ++t) {
            producers[t] = std::thread([&, t]() {
                uint64_t local = 0;
                for (int i = 0; i < 10000; ++i) {
                    const uint32_t bytes = static_cast<uint32_t>(t * 100 + (i % 50));
                    st.on_packet_received();
                    st.on_packet_decoded(bytes);
                    st.on_queue_watermark(static_cast<size_t>(i % 7));
                    local += bytes;
                }
                expect_bytes.fetch_add(local);
            });
        }
        for (auto& t : producers) t.join();
        auto s = st.snapshot();
        CHECK_EQ(s.packets_received, 40000u);
        CHECK_EQ(s.packets_decoded, 40000u);
        CHECK_EQ(s.bytes_decoded, expect_bytes.load());
        CHECK_EQ(s.queue_watermark_max, 6u);
    }

    return ::ijpd::test::summary("test_stats");
}
