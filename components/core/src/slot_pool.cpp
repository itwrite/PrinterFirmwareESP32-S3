// slot_pool.cpp —— 槽位池实现
#include "core/slot_pool.h"

#include <cstring>

namespace ijpd {

SlotPool::SlotPool(uint8_t* memory, size_t slot_count, size_t slot_size) noexcept {
    (void)reinit(memory, slot_count, slot_size);
}

Err SlotPool::reinit(uint8_t* memory, size_t slot_count, size_t slot_size) noexcept {
    memory_ = nullptr;
    slot_count_ = 0;
    slot_size_ = 0;
    if (memory == nullptr || slot_count == 0 || slot_count > kMaxSlotCount ||
        slot_size == 0) {
        return Err::INVALID_ARGUMENT;
    }
    memory_ = memory;
    slot_count_ = slot_count;
    slot_size_ = slot_size;
    reset();
    return Err::OK;
}

void SlotPool::reset() noexcept {
    ready_q_.reset();
    free_q_.reset();
    for (size_t i = 0; i < kMaxSlotCount; ++i) {
        slot_state_[i].store(static_cast<uint8_t>(SlotState::Free), std::memory_order_relaxed);
    }
    committed_.store(0, std::memory_order_relaxed);
    retired_.store(0, std::memory_order_relaxed);
    for (uint32_t i = 0; i < static_cast<uint32_t>(slot_count_); ++i) {
        (void)free_q_.push(i);
    }
}

uint32_t SlotPool::try_acquire_free() noexcept {
    if (slot_count_ == 0) return kInvalidIndex;
    uint32_t idx = kInvalidIndex;
    if (!free_q_.pop(&idx)) return kInvalidIndex;
    // 出队即独占；CAS 仅作不变量防御（失败说明内部损坏，回收入队并报无效）
    uint8_t expect = static_cast<uint8_t>(SlotState::Free);
    if (!slot_state_[idx].compare_exchange_strong(
            expect, static_cast<uint8_t>(SlotState::Filling), std::memory_order_acq_rel)) {
        (void)free_q_.push(idx);
        return kInvalidIndex;
    }
    return idx;
}

Err SlotPool::commit_filled(uint32_t index, uint32_t packet_seq, uint32_t valid_len,
                            uint32_t gen) noexcept {
    if (!valid_index_(index) || valid_len == 0 || valid_len > slot_size_) {
        return Err::INVALID_ARGUMENT;
    }
    // 代次已过期（任务取消/致命终结后 bump）：数据作废，槽位直接回收。
    // 否则该提交将以 READY 滞留，而终态后消费端可能不再被调度，
    // 造成槽位泄漏（依赖"消费者丢弃旧代次"的兜底不再成立）。
    if (gen != generation_.load(std::memory_order_acquire)) {
        (void)abort_fill(index);
        return Err::STALE_GENERATION;
    }
    uint8_t expect = static_cast<uint8_t>(SlotState::Filling);
    if (!slot_state_[index].compare_exchange_strong(
            expect, static_cast<uint8_t>(SlotState::Ready), std::memory_order_acq_rel)) {
        return Err::INVALID_ARGUMENT;  // 状态非法（未 acquire 或重复 commit）
    }
    // 容量不变量：就绪队列容量(16) >= 槽数，且每个非 FREE 槽至多占一个条目
    if (!ready_q_.push(ReadyItem{index, packet_seq, valid_len, gen})) {
        slot_state_[index].store(static_cast<uint8_t>(SlotState::Free),
                                 std::memory_order_release);
        (void)free_q_.push(index);
        return Err::INTERNAL_ERROR;
    }
    committed_.fetch_add(1, std::memory_order_relaxed);
    // 二次复检：上面的 gen 检查与 push 之间可能横跨 bump_generation()。
    // 过期则就地回收（CAS 保证与消费端"旧代次丢弃"路径互斥，避免双重回收）；
    // 队列中的残留条目由消费端按代次丢弃，槽位已 FREE 故无副作用。
    if (gen != generation_.load(std::memory_order_acquire)) {
        uint8_t ready = static_cast<uint8_t>(SlotState::Ready);
        if (slot_state_[index].compare_exchange_strong(
                ready, static_cast<uint8_t>(SlotState::Free), std::memory_order_acq_rel)) {
            (void)free_q_.push(index);
            retired_.fetch_add(1, std::memory_order_relaxed);  // 平衡上面的 committed_
        }
        return Err::STALE_GENERATION;
    }
    return Err::OK;
}

Err SlotPool::abort_fill(uint32_t index) noexcept {
    if (!valid_index_(index)) return Err::INVALID_ARGUMENT;
    // FILLING→FREE：从未计入 committed_，故不计 retired_
    uint8_t e = static_cast<uint8_t>(SlotState::Filling);
    if (!slot_state_[index].compare_exchange_strong(
            e, static_cast<uint8_t>(SlotState::Free), std::memory_order_acq_rel)) {
        return Err::INVALID_ARGUMENT;
    }
    (void)free_q_.push(index);
    return Err::OK;
}

bool SlotPool::try_acquire_ready(ReadyItem* out) noexcept {
    if (slot_count_ == 0) return false;
    for (;;) {
        ReadyItem item;
        if (!ready_q_.pop(&item)) return false;
        if (item.generation != generation()) {
            // 旧代次残留：丢弃并回收
            (void)retire_(item.slot_index, SlotState::Ready);
            continue;
        }
        uint8_t expect = static_cast<uint8_t>(SlotState::Ready);
        if (!slot_state_[item.slot_index].compare_exchange_strong(
                expect, static_cast<uint8_t>(SlotState::Processing),
                std::memory_order_acq_rel)) {
            // 不变量破坏的防御路径：回收槽位并保持计数一致
            (void)retire_(item.slot_index, SlotState::Ready);
            return false;
        }
        *out = item;
        return true;
    }
}

Err SlotPool::release(uint32_t index) noexcept {
    if (!valid_index_(index)) return Err::INVALID_ARGUMENT;
    return retire_(index, SlotState::Processing);
}

Err SlotPool::retire_(uint32_t index, SlotState expect) noexcept {
    uint8_t e = static_cast<uint8_t>(expect);
    if (!slot_state_[index].compare_exchange_strong(
            e, static_cast<uint8_t>(SlotState::Free), std::memory_order_acq_rel)) {
        return Err::INVALID_ARGUMENT;
    }
    (void)free_q_.push(index);
    retired_.fetch_add(1, std::memory_order_relaxed);
    return Err::OK;
}

uint32_t SlotPool::bump_generation() noexcept {
    return generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
}

size_t SlotPool::busy_count() const noexcept {
    const uint64_t c = committed_.load(std::memory_order_relaxed);
    const uint64_t r = retired_.load(std::memory_order_relaxed);
    return (c >= r) ? static_cast<size_t>(c - r) : 0;
}

uint8_t* SlotPool::slot_data(uint32_t index) noexcept {
    return valid_index_(index) ? memory_ + static_cast<size_t>(index) * slot_size_
                               : nullptr;
}

SlotPool::SlotState SlotPool::state(uint32_t index) const noexcept {
    if (!valid_index_(index)) return SlotState::Free;
    return static_cast<SlotState>(slot_state_[index].load(std::memory_order_acquire));
}

bool SlotPool::all_free() const noexcept {
    for (uint32_t i = 0; i < static_cast<uint32_t>(slot_count_); ++i) {
        if (state(i) != SlotState::Free) return false;
    }
    return busy_count() == 0;
}

}  // namespace ijpd
