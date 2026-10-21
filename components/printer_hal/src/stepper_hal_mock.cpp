// hal/stepper_hal_mock.cpp —— Mock 走纸实现
#include "printer_hal/stepper_hal.h"

namespace ijpd {

Err MockStepperHal::begin() {
    total_um_ = 0;
    healthy_ = true;
    return Err::OK;
}

Err MockStepperHal::feed(const FeedRequest& req, uint32_t* moved_um) {
    if (req.distance_um == 0 || req.um_per_second == 0) {
        return Err::INVALID_ARGUMENT;
    }
    const uint32_t ms = req.distance_um / req.um_per_second +
                        ((req.distance_um % req.um_per_second) != 0 ? 1 : 0);
    if (delay_ != nullptr) delay_(delay_user_, ms);
    total_um_ += req.distance_um;
    if (moved_um != nullptr) *moved_um = req.distance_um;
    return Err::OK;
}

Err MockStepperHal::stop() {
    return Err::OK;
}

}  // namespace ijpd
