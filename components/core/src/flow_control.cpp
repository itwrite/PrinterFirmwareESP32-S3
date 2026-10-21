// flow_control.cpp —— 水位滞回
#include "core/flow_control.h"

#include <atomic>

namespace ijpd {

Err FlowControl::make(size_t high, size_t low, FlowControl* out) noexcept {
    if (out == nullptr || low < 1 || low >= high) return Err::INVALID_ARGUMENT;
    out->high_ = high;
    out->low_ = low;
    out->paused_.store(false, std::memory_order_release);
    return Err::OK;
}

bool FlowControl::update(size_t busy) noexcept {
    if (busy >= high_) {
        paused_.store(true, std::memory_order_release);
    } else if (busy <= low_) {
        paused_.store(false, std::memory_order_release);
    }  // 滞回区间：维持原状态
    return !paused_.load(std::memory_order_acquire);
}

}  // namespace ijpd
