// ============================================================================
// sensor_hal.h —— 传感器抽象接口（中间件 Phase M1）
//
// 热敏打印机常见：缺纸/开盖/过温/卡纸。中间件周期轮询并在触发时
// 产生 sensor_alert 事件（协议 v1.1）+ 安全停机。
// ============================================================================
#ifndef IJPD_PRINTER_HAL_SENSOR_HAL_H
#define IJPD_PRINTER_HAL_SENSOR_HAL_H

#include <cstdint>

#include "core/errors.h"
#include "core/print_profile.h"

namespace ijpd {

enum class SensorId : uint8_t {
    PaperOut = 0,
    CoverOpen = 1,
    Overtemp = 2,
    PaperJam = 3,
};

class ISensorHal {
public:
    virtual ~ISensorHal() = default;

    // 查询指定传感器：true = 触发（缺纸/开盖/过温/卡纸）
    virtual bool get(SensorId id) const = 0;

    // Profile 声明的传感器集合（中间件只轮询声明的项）
    virtual SensorFlags supported() const = 0;
};

// ---- Mock 实现（热敏开发/演示用；可注入触发状态） ----
class MockSensorHal final : public ISensorHal {
public:
    explicit MockSensorHal(SensorFlags supported_mask = SensorFlags::None)
        : mask_(supported_mask) {}

    void set(SensorId id, bool triggered) noexcept {
        const uint8_t bit = static_cast<uint8_t>(1) << static_cast<uint8_t>(id);
        if (triggered) {
            state_ |= bit;
        } else {
            state_ &= static_cast<uint8_t>(~bit);
        }
    }

    bool get(SensorId id) const override {
        const uint8_t bit = static_cast<uint8_t>(1) << static_cast<uint8_t>(id);
        return (state_ & bit) != 0;
    }

    SensorFlags supported() const override { return mask_; }

private:
    SensorFlags mask_;
    uint8_t state_ = 0;
};

}  // namespace ijpd

#endif  // IJPD_PRINTER_HAL_SENSOR_HAL_H
