// ============================================================================
// flow_control.h —— 高低水位滞回流控（Phase 4，规范见任务书 7.4）
//
// busy = READY+PROCESSING 槽位数（由 SlotPool::busy_count() 提供）。
//   busy >= HIGH_WATERMARK -> 暂停读取（TCP 窗口收缩形成背压）
//   busy <= LOW_WATERMARK  -> 恢复读取（滞回防抖，两者之间维持原状态）
//
// 线程安全性：update/paused 原子（状态为原子量）；无锁。
// 说明：      TCP 背压不等价于应用层 ACK；APP 侧依赖 print_progress 事件。
// ============================================================================
#ifndef IJPD_CORE_FLOW_CONTROL_H
#define IJPD_CORE_FLOW_CONTROL_H

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "core/errors.h"

namespace ijpd {

class FlowControl {
public:
    // 默认构造：对象不可用（high=low=0，update 恒不暂停）；经 make() 初始化
    FlowControl() = default;

    // 校验：1 <= low < high；非法返回 INVALID_ARGUMENT（对象保持不可用）
    static Err make(size_t high, size_t low, FlowControl* out) noexcept;

    // 输入最新 busy 计数；返回是否允许继续读取（!paused）
    bool update(size_t busy) noexcept;
    bool paused() const noexcept { return paused_.load(std::memory_order_acquire); }

    size_t high_watermark() const noexcept { return high_; }
    size_t low_watermark() const noexcept { return low_; }

private:
    size_t high_ = 0;
    size_t low_ = 0;
    std::atomic<bool> paused_{false};
};

}  // namespace ijpd

#endif  // IJPD_CORE_FLOW_CONTROL_H
