// ricoh_mh5440_mock_hal.cpp - Deterministic logical MH5440 mock.
#include "printer_hal/ricoh_mh5440_mock_hal.h"

#include "core/crc32.h"

namespace ijpd {
namespace {

constexpr uint32_t kPpmScale = 1000000u;

uint32_t ceil_div_u32(uint32_t value, uint32_t divisor) noexcept {
    return value / divisor + ((value % divisor) != 0 ? 1u : 0u);
}

}  // namespace

Err MockRicohMh5440Hal::validate_config_() const noexcept {
    if (config_.channel_count != 4 || config_.virtual_nozzles_per_channel == 0 ||
        config_.logical_width_px == 0 || config_.missing_nozzle_ppm > kPpmScale) {
        return Err::INVALID_ARGUMENT;
    }
    const uint64_t derived_row_bytes =
        static_cast<uint64_t>(config_.logical_width_px) * config_.channel_count;
    if (derived_row_bytes == 0 || derived_row_bytes > UINT32_MAX) {
        return Err::INVALID_ARGUMENT;
    }
    const uint32_t expected_row_bytes = config_.row_bytes == 0
                                            ? static_cast<uint32_t>(derived_row_bytes)
                                            : config_.row_bytes;
    if (expected_row_bytes == 0 || expected_row_bytes % config_.channel_count != 0) {
        return Err::INVALID_ARGUMENT;
    }
    if (config_.strict_head_geometry &&
        (config_.logical_width_px != config_.virtual_nozzles_per_channel ||
         expected_row_bytes != config_.virtual_nozzles_per_channel * config_.channel_count)) {
        return Err::INVALID_ARGUMENT;
    }
    return Err::OK;
}

void MockRicohMh5440Hal::reset_stats_() noexcept {
    stats_ = RicohMh5440MockStats{};
    for (uint32_t& state : channel_crc_state_) state = CRC32_INIT_STATE;
}

Err MockRicohMh5440Hal::begin(const PrintheadConfig& cfg) {
    const Err valid = validate_config_();
    if (valid != Err::OK) return valid;
    if (cfg.row_bytes != 0 && cfg.row_bytes != config_.row_bytes &&
        config_.row_bytes != 0) {
        return Err::INVALID_ARGUMENT;
    }
    const uint32_t derived_row_bytes = config_.row_bytes == 0
                                           ? static_cast<uint32_t>(
                                                 static_cast<uint64_t>(config_.logical_width_px) *
                                                 config_.channel_count)
                                           : config_.row_bytes;
    if (cfg.row_bytes != 0 && cfg.row_bytes != derived_row_bytes) {
        return Err::INVALID_ARGUMENT;
    }

    begin_config_ = cfg;
    row_bytes_ = cfg.row_bytes == 0 ? derived_row_bytes : cfg.row_bytes;
    reset_stats_();
    random_state_ = config_.random_seed == 0 ? 1u : config_.random_seed;
    next_row_index_ = 0;
    configured_ = true;
    stopped_ = false;
    hardware_fault_ = config_.inject_thermal_fault;
    blocked_ = false;
    if (hardware_fault_) {
        stats_.fault_count = 1;
        stats_.terminal_error = Err::HAL_FAULT;
        return Err::HAL_FAULT;
    }
    delay_ms_(config_.startup_delay_ms);
    return Err::OK;
}

Err MockRicohMh5440Hal::fail_input_(Err error, uint32_t row_index) noexcept {
    if (!stats_.has_bad_row) {
        stats_.has_bad_row = true;
        stats_.first_bad_row = row_index;
    }
    stats_.terminal_error = error;
    return error;
}

Err MockRicohMh5440Hal::fail_hardware_(Err error) noexcept {
    hardware_fault_ = true;
    ++stats_.fault_count;
    stats_.terminal_error = error;
    return error;
}

uint32_t MockRicohMh5440Hal::next_random_() noexcept {
    random_state_ = random_state_ * 1664525u + 1013904223u;
    return random_state_;
}

bool MockRicohMh5440Hal::should_drop_(uint32_t, uint32_t, uint32_t) noexcept {
    if (config_.missing_nozzle_ppm == 0) return false;
    return (next_random_() % kPpmScale) < config_.missing_nozzle_ppm;
}

void MockRicohMh5440Hal::delay_ms_(uint32_t ms) noexcept {
    if (delay_ != nullptr && ms != 0) delay_(delay_user_, ms);
}

Err MockRicohMh5440Hal::submitRaster(const uint8_t* data, size_t len,
                                     const RasterMeta& meta) {
    if (!configured_) return Err::HAL_NOT_CONFIGURED;
    if (stopped_) return Err::JOB_CANCELLED;
    if (hardware_fault_) return Err::HAL_FAULT;
    if (stats_.terminal_error != Err::OK) return stats_.terminal_error;
    if (data == nullptr || len != row_bytes_) return fail_input_(Err::INVALID_ARGUMENT,
                                                                  meta.row_index);
    if (config_.require_monotonic_rows && meta.row_index != next_row_index_) {
        return fail_input_(Err::INVALID_ARGUMENT, meta.row_index);
    }
    if (config_.inject_paper_jam && stats_.rows_accepted == 0) {
        return fail_hardware_(Err::HAL_FAULT);
    }
    if (config_.backpressure_after_rows != 0 &&
        stats_.rows_accepted >= config_.backpressure_after_rows) {
        blocked_ = true;
        stats_.terminal_error = Err::QUEUE_TIMEOUT;
        return Err::QUEUE_TIMEOUT;
    }

    const uint32_t width = static_cast<uint32_t>(len / config_.channel_count);
    for (uint32_t pixel = 0; pixel < width; ++pixel) {
        for (uint32_t channel = 0; channel < config_.channel_count; ++channel) {
            const uint8_t value = data[pixel * config_.channel_count + channel];
            channel_crc_state_[channel] = crc32_update(channel_crc_state_[channel], &value, 1);
            if (should_drop_(meta.row_index, pixel, channel)) {
                ++stats_.missing_nozzle_drops[channel];
            } else {
                if (value != 0) ++stats_.nonzero_pixels[channel];
                stats_.intensity_sum[channel] += value;
            }
        }
    }

    ++stats_.rows_accepted;
    stats_.bytes_accepted += len;
    stats_.last_row_index = meta.row_index;
    next_row_index_ = meta.row_index + 1;

    const uint32_t row_period_ms = ceil_div_u32(config_.row_period_us, 1000u);
    delay_ms_(row_period_ms);

    if ((config_.fault_after_rows != 0 &&
         stats_.rows_accepted >= config_.fault_after_rows) ||
        (config_.fault_after_bytes != 0 &&
         stats_.bytes_accepted >= config_.fault_after_bytes)) {
        return fail_hardware_(Err::HAL_FAULT);
    }
    return Err::OK;
}

Err MockRicohMh5440Hal::submitRaw(const uint8_t* data, size_t len) {
    if (!configured_) return Err::HAL_NOT_CONFIGURED;
    if (!config_.allow_raw) return Err::UNSUPPORTED;
    if (stopped_) return Err::JOB_CANCELLED;
    if (hardware_fault_) return Err::HAL_FAULT;
    if (data == nullptr && len != 0) return Err::INVALID_ARGUMENT;
    stats_.raw_bytes_received += len;
    return Err::OK;
}

Err MockRicohMh5440Hal::waitUntilIdle(uint32_t timeout_ms) {
    (void)timeout_ms;
    if (!configured_) return Err::HAL_NOT_CONFIGURED;
    if (hardware_fault_) return Err::HAL_FAULT;
    if (blocked_) return Err::QUEUE_TIMEOUT;
    return Err::OK;
}

Err MockRicohMh5440Hal::stop() {
    if (!stopped_) delay_ms_(config_.stop_delay_ms);
    stopped_ = true;
    return Err::OK;
}

bool MockRicohMh5440Hal::healthy() const {
    return configured_ && !stopped_ && !hardware_fault_ && !blocked_;
}

RicohMh5440MockStats MockRicohMh5440Hal::stats() const noexcept {
    RicohMh5440MockStats result = stats_;
    for (uint32_t channel = 0; channel < 4; ++channel) {
        result.channel_crc32[channel] = crc32_final(channel_crc_state_[channel]);
    }
    return result;
}

}  // namespace ijpd
