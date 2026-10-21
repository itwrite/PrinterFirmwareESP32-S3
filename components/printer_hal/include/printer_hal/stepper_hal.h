// ============================================================================
// hal/stepper_hal.h —— 走纸步进电机抽象接口（Phase 6，任务书 9.2）
//
// 真实实现：GPTimer/MCPWM 脉冲 + 自研梯形加减速（Phase 7，待硬件资料）。
// Mock：按速度模拟耗时。A4988 仅为候选步进驱动器，绝不用于喷头。
// ============================================================================
#ifndef IJPD_HAL_STEPPER_HAL_H
#define IJPD_HAL_STEPPER_HAL_H

#include <cstdint>

#include "core/errors.h"

namespace ijpd {

struct FeedRequest {
    uint32_t distance_um = 0;      // 走纸距离（微米）
    uint32_t um_per_second = 1000; // 速度（真实实现受加减速与机械上限约束）
};

class IStepperHal {
public:
    virtual ~IStepperHal() = default;
    virtual Err begin() = 0;
    virtual Err feed(const FeedRequest& req, uint32_t* moved_um) = 0;
    virtual Err stop() = 0;  // 幂等
    virtual bool healthy() const = 0;
};

class MockStepperHal final : public IStepperHal {
public:
    using DelayFn = void (*)(void* user, uint32_t ms);
    explicit MockStepperHal(DelayFn delay = nullptr, void* delay_user = nullptr)
        : delay_(delay), delay_user_(delay_user) {}

    Err begin() override;
    Err feed(const FeedRequest& req, uint32_t* moved_um) override;
    Err stop() override;
    bool healthy() const override { return healthy_; }

    uint64_t total_um() const { return total_um_; }

private:
    DelayFn delay_;
    void* delay_user_;
    uint64_t total_um_ = 0;
    bool healthy_ = true;
};

}  // namespace ijpd

#endif  // IJPD_HAL_STEPPER_HAL_H
