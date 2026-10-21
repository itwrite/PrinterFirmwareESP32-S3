// ============================================================================
// slot_pool.h —— PSRAM 槽位池 + 就绪队列（Phase 4，规范见任务书 7.2）
//
// 槽位状态机：FREE → FILLING → READY → PROCESSING → FREE
//   - 全部状态转换由本类集中提供原子接口（每槽 CAS + 无锁队列），
//     生产者不可能覆盖消费者正在处理的槽位；
//   - 就绪队列元素只含（槽位索引、包序号、有效长度、任务代次），
//     不拷贝图像数据；
//   - 任务取消：bump_generation() 后，旧代次元素在消费时被丢弃并回收
//     槽位；FILLING 中的槽由生产者 abort_fill() 放弃。
//
// 并发模型：多生产者多消费者（MPMC）安全。
// 阻塞语义：全部接口非阻塞（try 语义）；等待/超时/取消唤醒由平台任务
//   组合实现（FreeRTOS 信号量/通知），满足 7.4 “等待必须带超时且可取消”。
// 所有权：    槽位内存由调用方提供（启动期一次性分配，运行期零动态分配）。
// 容量上限：  kMaxSlotCount = 16（编译期，static_assert SLOT_COUNT <= 16）。
// 最大耗时：  所有操作 O(1)，无锁，微秒级。
// ============================================================================
#ifndef IJPD_CORE_SLOT_POOL_H
#define IJPD_CORE_SLOT_POOL_H

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "core/errors.h"

namespace ijpd {

class SlotPool {
public:
    static constexpr size_t kMaxSlotCount = 16;
    static constexpr uint32_t kInvalidIndex = 0xFFFFFFFFu;

    enum class SlotState : uint8_t { Free = 0, Filling, Ready, Processing };

    struct ReadyItem {
        uint32_t slot_index;
        uint32_t packet_seq;   // 包序号
        uint32_t valid_len;    // 解压后有效字节数
        uint32_t generation;   // 任务代次（不匹配者被消费端丢弃）
    };

    // memory 必须是 slot_count * slot_size 的可写内存（PSRAM 或主机堆）。
    // slot_count ∈ [1, kMaxSlotCount]；构造后所有槽为 FREE。
    SlotPool(uint8_t* memory, size_t slot_count, size_t slot_size) noexcept;

    // 默认构造：空池（不可用）；经 reinit() 启用
    SlotPool() noexcept = default;

    // 重新绑定内存并复位（单线程时机调用：任务启动/结束）
    Err reinit(uint8_t* memory, size_t slot_count, size_t slot_size) noexcept;

    SlotPool(const SlotPool&) = delete;
    SlotPool& operator=(const SlotPool&) = delete;

    // ---- 生产者（decode_task） ----
    // FREE→FILLING；无空闲返回 kInvalidIndex（背压：调用方停止读取并让出）
    uint32_t try_acquire_free() noexcept;
    // FILLING→READY 并入队；gen 为该数据的任务代次
    Err commit_filled(uint32_t index, uint32_t packet_seq, uint32_t valid_len,
                      uint32_t gen) noexcept;
    // FILLING→FREE：生产者放弃（任务取消/解压失败）
    Err abort_fill(uint32_t index) noexcept;

    // ---- 消费者（print_task） ----
    // READY→PROCESSING；代次不匹配的元素被静默丢弃（槽回收），继续取下一个。
    // 队列空返回 false。
    bool try_acquire_ready(ReadyItem* out) noexcept;
    // PROCESSING→FREE
    Err release(uint32_t index) noexcept;

    // ---- 任务代次 ----
    // 取消/新任务时调用：当前代次 +1。返回新代次。
    uint32_t bump_generation() noexcept;
    uint32_t generation() const noexcept { return generation_.load(std::memory_order_acquire); }

    // ---- 观测 ----
    // READY+PROCESSING 槽位数（FlowControl 的输入；单调计数差，无撕裂）
    size_t busy_count() const noexcept;
    uint8_t* slot_data(uint32_t index) noexcept;
    size_t slot_count() const noexcept { return slot_count_; }
    size_t slot_size() const noexcept { return slot_size_; }
    SlotState state(uint32_t index) const noexcept;

    // 单线程诊断（任务结束/测试断言用）：全部槽 FREE 且队列空
    bool all_free() const noexcept;
    // 单线程复位（任务间复用；调用前必须保证无并发访问）
    void reset() noexcept;

private:
    // Vyukov 有界 MPMC 队列（容量为 2 的幂）
    template <typename T, size_t kPow2>
    struct MpmcQueue {
        struct Cell {
            std::atomic<uint32_t> seq;
            T item;
        };
        static constexpr uint32_t kMask = static_cast<uint32_t>(kPow2 - 1);

        MpmcQueue() noexcept {
            for (size_t i = 0; i < kPow2; ++i) {
                cells_[i].seq.store(static_cast<uint32_t>(i), std::memory_order_relaxed);
            }
        }
        void reset() noexcept {
            for (size_t i = 0; i < kPow2; ++i) {
                cells_[i].seq.store(static_cast<uint32_t>(i), std::memory_order_relaxed);
            }
            enq_.store(0, std::memory_order_relaxed);
            deq_.store(0, std::memory_order_relaxed);
        }
        bool push(const T& v) noexcept {
            Cell* c;
            uint32_t pos = enq_.load(std::memory_order_relaxed);
            for (;;) {
                c = &cells_[pos & kMask];
                const uint32_t seq = c->seq.load(std::memory_order_acquire);
                const int32_t dif = static_cast<int32_t>(seq) - static_cast<int32_t>(pos);
                if (dif == 0) {
                    if (enq_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) break;
                } else if (dif < 0) {
                    return false;  // 满
                } else {
                    pos = enq_.load(std::memory_order_relaxed);
                }
            }
            c->item = v;
            c->seq.store(pos + 1, std::memory_order_release);
            return true;
        }
        bool pop(T* out) noexcept {
            Cell* c;
            uint32_t pos = deq_.load(std::memory_order_relaxed);
            for (;;) {
                c = &cells_[pos & kMask];
                const uint32_t seq = c->seq.load(std::memory_order_acquire);
                const int32_t dif =
                    static_cast<int32_t>(seq) - static_cast<int32_t>(pos + 1);
                if (dif == 0) {
                    if (deq_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) break;
                } else if (dif < 0) {
                    return false;  // 空
                } else {
                    pos = deq_.load(std::memory_order_relaxed);
                }
            }
            *out = c->item;
            c->seq.store(pos + static_cast<uint32_t>(kPow2), std::memory_order_release);
            return true;
        }

        Cell cells_[kPow2];
        alignas(64) std::atomic<uint32_t> enq_{0};
        alignas(64) std::atomic<uint32_t> deq_{0};
    };

    bool valid_index_(uint32_t i) const noexcept { return i < slot_count_; }
    Err retire_(uint32_t index, SlotState expect) noexcept;  // CAS→Free + 回收入队 + retired++

    uint8_t* memory_ = nullptr;
    size_t slot_count_ = 0;
    size_t slot_size_ = 0;

    std::atomic<uint8_t> slot_state_[kMaxSlotCount];
    MpmcQueue<ReadyItem, 16> ready_q_;
    MpmcQueue<uint32_t, 16> free_q_;
    std::atomic<uint64_t> committed_{0};
    std::atomic<uint64_t> retired_{0};
    std::atomic<uint32_t> generation_{0};
};

}  // namespace ijpd

#endif  // IJPD_CORE_SLOT_POOL_H
