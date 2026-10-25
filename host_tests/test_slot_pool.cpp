// test_slot_pool.cpp —— 槽位池：单线程语义 / 代次 / 背压 / 多线程压力
// 压力不变量（任务书 12.3）：
//   - 并发生产者/消费者下状态转换合法，不出现数据覆盖（内容校验逐字节）
//   - 槽位耗尽时生产者非阻塞返回（背压），不死锁
//   - 任意时刻 bump_generation（取消）后可完全回收（all_free）
#include "core/slot_pool.h"
#include "assert.h"

#include <atomic>
#include <cstring>
#include <thread>
#include <vector>

#include "core/config_limits.h"

using namespace ijpd;

namespace {

constexpr size_t kTestSlots = 6;
constexpr size_t kTestSlotSize = 512;

// 由 (slot, seq, len) 决定的确定性填充图案
void fill_pattern(uint8_t* p, uint32_t slot, uint32_t seq, uint32_t len) {
    uint32_t st = slot * 2654435761u + seq * 40503u + len;
    for (uint32_t i = 0; i < len; ++i) {
        st = st * 1664525u + 1013904223u;
        p[i] = static_cast<uint8_t>(st >> 24);
    }
}

bool check_pattern(const uint8_t* p, uint32_t slot, uint32_t seq, uint32_t len) {
    std::vector<uint8_t> want(len);
    fill_pattern(want.data(), slot, seq, len);
    return std::memcmp(p, want.data(), len) == 0;
}

}  // namespace

int main() {
    static uint8_t mem[kTestSlots * kTestSlotSize];

    // ================= 1. 单线程基本语义 =================
    {
        SlotPool pool(mem, kTestSlots, kTestSlotSize);
        CHECK_EQ(pool.slot_count(), kTestSlots);
        CHECK_TRUE(pool.all_free());
        CHECK_EQ(pool.busy_count(), 0u);

        // 全部占用
        uint32_t idx[kTestSlots];
        for (size_t i = 0; i < kTestSlots; ++i) {
            idx[i] = pool.try_acquire_free();
            CHECK_TRUE(idx[i] != SlotPool::kInvalidIndex);
            CHECK_TRUE(pool.state(idx[i]) == SlotPool::SlotState::Filling);
        }
        CHECK_TRUE(pool.try_acquire_free() == SlotPool::kInvalidIndex);  // 背压
        CHECK_EQ(pool.busy_count(), 0u);  // 尚未 commit

        // FIFO 提交
        for (size_t i = 0; i < kTestSlots; ++i) {
            CHECK_TRUE(pool.commit_filled(idx[i], static_cast<uint32_t>(i), 100, 0) == Err::OK);
        }
        CHECK_EQ(pool.busy_count(), kTestSlots);
        for (size_t i = 0; i < kTestSlots; ++i) {
            SlotPool::ReadyItem it;
            CHECK_TRUE(pool.try_acquire_ready(&it));
            CHECK_EQ(it.slot_index, idx[i]);  // FIFO 顺序
            CHECK_EQ(it.packet_seq, i);
            CHECK_TRUE(pool.state(it.slot_index) == SlotPool::SlotState::Processing);
        }
        CHECK_TRUE(!pool.try_acquire_ready(nullptr));  // 队列空（nullptr 出参不接触）
        CHECK_EQ(pool.busy_count(), kTestSlots);
        for (size_t i = 0; i < kTestSlots; ++i) {
            CHECK_TRUE(pool.release(idx[i]) == Err::OK);
        }
        CHECK_TRUE(pool.all_free());
        CHECK_EQ(pool.busy_count(), 0u);

        // 非法操作拒绝
        CHECK_TRUE(pool.release(0) == Err::INVALID_ARGUMENT);          // 非 PROCESSING
        CHECK_TRUE(pool.commit_filled(0, 1, 10, 0) == Err::INVALID_ARGUMENT);  // 非 FILLING
        CHECK_TRUE(pool.commit_filled(99, 1, 10, 0) == Err::INVALID_ARGUMENT); // 越界
        CHECK_TRUE(pool.release(99) == Err::INVALID_ARGUMENT);
        uint32_t a = pool.try_acquire_free();
        CHECK_TRUE(pool.commit_filled(a, 1, kTestSlotSize + 1, 0) ==
                   Err::INVALID_ARGUMENT);  // 超长
        CHECK_TRUE(pool.commit_filled(a, 1, 0, 0) == Err::INVALID_ARGUMENT);
        CHECK_TRUE(pool.abort_fill(a) == Err::OK);
        const uint32_t b = pool.try_acquire_free();  // 空闲队列 FIFO，回收槽在队尾
        CHECK_TRUE(b != SlotPool::kInvalidIndex);
        CHECK_TRUE(pool.abort_fill(b) == Err::OK);
        CHECK_TRUE(pool.all_free());
    }

    // ================= 2. 代次（取消）语义 =================
    {
        SlotPool pool(mem, kTestSlots, kTestSlotSize);
        // 3 个槽提交为 gen 0，随后取消
        for (uint32_t i = 0; i < 3; ++i) {
            uint32_t s = pool.try_acquire_free();
            CHECK_TRUE(s != SlotPool::kInvalidIndex);
            CHECK_TRUE(pool.commit_filled(s, i, 64, pool.generation()) == Err::OK);
        }
        CHECK_EQ(pool.busy_count(), 3u);
        const uint32_t g1 = pool.bump_generation();
        CHECK_EQ(g1, 1u);

        SlotPool::ReadyItem it;
        CHECK_TRUE(!pool.try_acquire_ready(&it));  // 旧代次全部被丢弃
        CHECK_EQ(pool.busy_count(), 0u);
        CHECK_TRUE(pool.all_free());

        // 新代次正常流转
        uint32_t s0 = pool.try_acquire_free();
        CHECK_TRUE(pool.commit_filled(s0, 7, 32, pool.generation()) == Err::OK);
        CHECK_TRUE(pool.try_acquire_ready(&it));
        CHECK_EQ(it.packet_seq, 7u);
        CHECK_TRUE(pool.release(it.slot_index) == Err::OK);
        CHECK_TRUE(pool.all_free());
    }

    // ================= 3. 背压 + 取消响应（无死锁） =================
    {
        SlotPool pool(mem, kTestSlots, kTestSlotSize);
        std::atomic<bool> consumer_paused{true};
        std::atomic<uint32_t> saturated_count{0};

        std::thread producer([&]() {
            uint32_t made = 0;
            while (made < 100) {
                const uint32_t s = pool.try_acquire_free();
                if (s == SlotPool::kInvalidIndex) {
                    saturated_count.fetch_add(1);  // 观察到背压，非阻塞
                    std::this_thread::yield();
                    continue;
                }
                fill_pattern(pool.slot_data(s), s, made, 16);
                if (pool.commit_filled(s, made, 16, pool.generation()) == Err::OK) {
                    ++made;
                }
            }
        });
        std::thread consumer([&]() {
            while (consumer_paused.load()) std::this_thread::yield();
            uint32_t got = 0;
            while (got < 100) {
                SlotPool::ReadyItem it;
                if (!pool.try_acquire_ready(&it)) {
                    std::this_thread::yield();
                    continue;
                }
                CHECK_TRUE(check_pattern(pool.slot_data(it.slot_index), it.slot_index,
                                         it.packet_seq, it.valid_len));
                CHECK_TRUE(pool.release(it.slot_index) == Err::OK);
                ++got;
            }
        });
        // 等待生产者观察到饱和（证明背压路径被执行）
        while (saturated_count.load() == 0) std::this_thread::yield();
        consumer_paused.store(false);
        producer.join();
        consumer.join();
        CHECK_TRUE(saturated_count.load() > 0);
        CHECK_TRUE(pool.all_free());
        CHECK_EQ(pool.busy_count(), 0u);
    }

    // ================= 4. 多线程压力：1P1C + 随机取消代次 =================
    {
        SlotPool pool(mem, kTestSlots, kTestSlotSize);
        std::atomic<uint32_t> produced{0};
        std::atomic<uint32_t> consumed{0};
        std::atomic<bool> producer_done{false};
        std::atomic<bool> canceler_done{false};
        const uint32_t kTotal = 30000;
        std::atomic<uint32_t> mismatches{0};

        std::thread producer([&]() {
            uint32_t seq = 0;
            while (seq < kTotal) {
                const uint32_t s = pool.try_acquire_free();
                if (s == SlotPool::kInvalidIndex) {
                    std::this_thread::yield();
                    continue;
                }
                const uint32_t len = 16 + (seq % 64);
                fill_pattern(pool.slot_data(s), s, seq, len);
                // 提交使用当前代次（读取可能恰逢取消 -> 元素将被消费端丢弃）
                const Err commit_error = pool.commit_filled(s, seq, len, pool.generation());
                if (commit_error == Err::OK) {
                    ++seq;
                    produced.fetch_add(1);
                } else if (commit_error != Err::STALE_GENERATION) {
                    // STALE_GENERATION already recycles the filling slot.
                    CHECK_TRUE(pool.abort_fill(s) == Err::OK);
                }
            }
            producer_done.store(true);
        });
        std::thread canceler([&]() {
            // 每 ~5000 个随机取消一次（最后一次取消后不再取消，保证可收敛）
            for (int i = 0; i < 5; ++i) {
                while (produced.load() < static_cast<uint32_t>((i + 1) * 5000) &&
                       !producer_done.load()) {
                    std::this_thread::yield();
                }
                if (i < 4) pool.bump_generation();
            }
            canceler_done.store(true);
        });
        std::thread consumer([&]() {
            for (;;) {
                SlotPool::ReadyItem it;
                if (!pool.try_acquire_ready(&it)) {
                    // 生产者已完成且队列排空 -> 退出（被丢弃的旧代次元素不算缺口）
                    if (producer_done.load() && pool.busy_count() == 0) break;
                    std::this_thread::yield();
                    continue;
                }
                if (!check_pattern(pool.slot_data(it.slot_index), it.slot_index,
                                   it.packet_seq, it.valid_len)) {
                    mismatches.fetch_add(1);  // 数据被覆盖 -> 严重错误
                }
                CHECK_TRUE(pool.release(it.slot_index) == Err::OK);
                consumed.fetch_add(1);
            }
        });
        producer.join();
        canceler.join();
        consumer.join();
        CHECK_EQ(mismatches.load(), 0u);   // 不覆盖数据
        CHECK_TRUE(pool.all_free());        // 不泄漏槽位
        CHECK_EQ(pool.busy_count(), 0u);
        CHECK_TRUE(consumed.load() <= produced.load());
    }

    // ================= 5. 多线程压力：2P2C（MPMC） =================
    {
        SlotPool pool(mem, kTestSlots, kTestSlotSize);
        const uint32_t kPerProducer = 20000;
        std::atomic<uint32_t> produced{0};
        std::atomic<bool> done{false};
        std::atomic<uint32_t> mismatches{0};
        std::atomic<uint32_t> consumed{0};

        auto prod_fn = [&](uint32_t tag) {
            uint32_t seq = 0;
            while (seq < kPerProducer) {
                const uint32_t s = pool.try_acquire_free();
                if (s == SlotPool::kInvalidIndex) {
                    std::this_thread::yield();
                    continue;
                }
                const uint32_t len = 16 + ((seq + tag) % 200);
                fill_pattern(pool.slot_data(s), s, seq * 2 + tag, len);
                if (pool.commit_filled(s, seq * 2 + tag, len, pool.generation()) == Err::OK) {
                    ++seq;
                    produced.fetch_add(1);
                } else {
                    (void)pool.abort_fill(s);
                }
            }
        };
        auto cons_fn = [&]() {
            while (!done.load() || pool.busy_count() > 0) {
                SlotPool::ReadyItem it;
                if (!pool.try_acquire_ready(&it)) {
                    std::this_thread::yield();
                    continue;
                }
                if (!check_pattern(pool.slot_data(it.slot_index), it.slot_index,
                                   it.packet_seq, it.valid_len)) {
                    mismatches.fetch_add(1);
                }
                (void)pool.release(it.slot_index);
                consumed.fetch_add(1);
            }
        };
        std::thread p0(prod_fn, 0), p1(prod_fn, 1), c0(cons_fn), c1(cons_fn);
        p0.join();
        p1.join();
        done.store(true);
        c0.join();
        c1.join();
        CHECK_EQ(mismatches.load(), 0u);
        CHECK_TRUE(pool.all_free());
        CHECK_EQ(consumed.load(), 2u * kPerProducer);  // 无代次取消 -> 全量消费
    }

    // ================= 6. 构造参数防御 =================
    {
        SlotPool bad1(nullptr, 4, 100);
        CHECK_TRUE(bad1.try_acquire_free() == SlotPool::kInvalidIndex);
        SlotPool bad2(mem, 0, 100);
        CHECK_TRUE(bad2.try_acquire_free() == SlotPool::kInvalidIndex);
        SlotPool bad3(mem, SlotPool::kMaxSlotCount + 1, 100);
        CHECK_TRUE(bad3.try_acquire_free() == SlotPool::kInvalidIndex);
        // 合法上限
        static uint8_t mem16[SlotPool::kMaxSlotCount * 64];
        SlotPool ok(mem16, SlotPool::kMaxSlotCount, 64);
        CHECK_EQ(ok.slot_count(), SlotPool::kMaxSlotCount);
        CHECK_TRUE(ok.all_free());
    }

    return ::ijpd::test::summary("test_slot_pool");
}
