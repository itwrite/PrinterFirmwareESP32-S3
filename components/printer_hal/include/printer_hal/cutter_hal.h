// ============================================================================
// hal/cutter_hal.h —— 切纸抽象接口（Phase 6，任务书 9.3）
//
// 默认 UNSUPPORTED：切纸机构未确认（hardware_questions Q30/Q31），
// 不得驱动任何未知引脚。cutting_after_finish=true 且不支持时任务以
// DONE_WITH_WARNING 完成（不影响打印结果）。
// ============================================================================
#ifndef IJPD_HAL_CUTTER_HAL_H
#define IJPD_HAL_CUTTER_HAL_H

#include "core/errors.h"

namespace ijpd {

class ICutterHal {
public:
    virtual ~ICutterHal() = default;
    virtual bool supported() const = 0;
    virtual Err cut() = 0;  // UNSUPPORTED 或执行结果
};

class MockCutterHal final : public ICutterHal {
public:
    // 默认不支持；硬件确认后由配置打开（fault_on_cut 用于注入故障）
    void set_supported(bool on, bool fault_on_cut = false) {
        supported_ = on;
        fault_on_cut_ = fault_on_cut;
    }
    bool supported() const override { return supported_; }
    Err cut() override {
        if (!supported_) return Err::UNSUPPORTED;
        if (fault_on_cut_) return Err::HAL_FAULT;
        return Err::OK;
    }

private:
    bool supported_ = false;
    bool fault_on_cut_ = false;
};

}  // namespace ijpd

#endif  // IJPD_HAL_CUTTER_HAL_H
