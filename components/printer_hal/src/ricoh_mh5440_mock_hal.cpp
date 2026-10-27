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
    PrintheadConfig effective = cfg;
    if (effective.pixel_bits == 0) {
        effective.pixel_bits = 8;
        effective.channel_count = config_.channel_count;
        effective.pixel_width = config_.logical_width_px;
        effective.raster_layout = RasterLayout::Interleaved;
        effective.plane_count = config_.channel_count;
        effective.sequential_channels = false;
        effective.channel_order[0] = RasterChannel::Cyan;
        effective.channel_order[1] = RasterChannel::Magenta;
        effective.channel_order[2] = RasterChannel::Yellow;
        effective.channel_order[3] = RasterChannel::Black;
    }
    if (effective.pixel_width == 0) effective.pixel_width = config_.logical_width_px;
    if (effective.channel_count == 0) effective.channel_count = config_.channel_count;
    const uint32_t bits = effective.pixel_bits;
    const uint32_t channels = effective.channel_count > 0
        ? effective.channel_count : 1;
    const uint32_t pixel_width = effective.pixel_width > 0
        ? effective.pixel_width : config_.logical_width_px;
    const uint32_t packed_bits = effective.raster_layout == RasterLayout::Interleaved
        ? bits * channels : bits;
    const uint32_t logical_row_bytes = static_cast<uint32_t>(
        (static_cast<uint64_t>(pixel_width) * packed_bits + 7) / 8);
    if (effective.row_bytes != 0 && effective.row_bytes != config_.row_bytes &&
        config_.row_bytes != 0) {
        return Err::INVALID_ARGUMENT;
    }
    const uint32_t derived_row_bytes = config_.row_bytes == 0
                                           ? logical_row_bytes : config_.row_bytes;
    if (effective.row_bytes != 0 && effective.row_bytes != derived_row_bytes) {
        return Err::INVALID_ARGUMENT;
    }

    begin_config_ = effective;
    row_bytes_ = effective.row_bytes == 0 ? derived_row_bytes : effective.row_bytes;
    // 行周期优先级：宿主注入的 rows_per_second > 静态配置 row_period_us。
    // The target delay callback has millisecond resolution, so values above
    // 1000 rows/s are limited by the scheduler's one-tick minimum delay.
    // The print task must not add another per-row tick on top of this.
    if (cfg.rows_per_second > 1) {
        effective_row_period_us_ = ceil_div_u32(1000000u, cfg.rows_per_second);
    } else {
        effective_row_period_us_ = config_.row_period_us;
    }
    reset_stats_();
    random_state_ = config_.random_seed == 0 ? 1u : config_.random_seed;
    next_row_index_ = 0;
    next_plane_index_ = 0;
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
    const uint8_t expected_plane = (begin_config_.sequential_channels ||
                                    begin_config_.dispatch_planar_rows)
        ? next_plane_index_ : 0;
    if (config_.require_monotonic_rows &&
        (meta.plane_index != expected_plane || meta.row_index != next_row_index_)) {
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

    const uint32_t bits = begin_config_.pixel_bits;
    const uint32_t channels = begin_config_.channel_count;
    const uint32_t pixel_bits = begin_config_.raster_layout == RasterLayout::Interleaved
        ? bits * channels : bits;
    bool fast_path = false;
    if (bits == 1 && begin_config_.raster_layout == RasterLayout::Planar &&
        (begin_config_.sequential_channels || begin_config_.dispatch_planar_rows) &&
        config_.missing_nozzle_ppm == 0 && meta.plane_index < channels &&
        meta.plane_index < config_.channel_count &&
        (begin_config_.pixel_width + 7u) / 8u <= len) {
        uint32_t channel = meta.plane_index;
        switch (begin_config_.channel_order[meta.plane_index]) {
            case RasterChannel::Cyan: channel = 0; break;
            case RasterChannel::Magenta: channel = 1; break;
            case RasterChannel::Yellow: channel = 2; break;
            case RasterChannel::Black: channel = 3; break;
            default: break;
        }

        // The common CMYK1 contract stores one bit per pixel. Decode packed
        // bytes in batches instead of calling the general pixel unpacker and
        // CRC updater once per pixel; the old per-pixel path dominated job
        // time for multi-thousand-row pages.
        uint8_t expanded[256];
        size_t expanded_len = 0;
        uint32_t pixels = 0;
        uint32_t nonzero = 0;
        uint64_t intensity = 0;
        for (size_t byte = 0; byte < len && pixels < begin_config_.pixel_width; ++byte) {
            const uint8_t packed = data[byte];
            for (int bit = 7; bit >= 0 && pixels < begin_config_.pixel_width; --bit) {
                const uint8_t value = (packed & (1u << bit)) != 0 ? 255u : 0u;
                expanded[expanded_len++] = value;
                if (value != 0) {
                    ++nonzero;
                    intensity += value;
                }
                ++pixels;
                if (expanded_len == sizeof(expanded)) {
                    channel_crc_state_[channel] = crc32_update(
                        channel_crc_state_[channel], expanded, expanded_len);
                    expanded_len = 0;
                }
            }
        }
        if (expanded_len != 0) {
            channel_crc_state_[channel] = crc32_update(
                channel_crc_state_[channel], expanded, expanded_len);
        }
        stats_.nonzero_pixels[channel] += nonzero;
        stats_.intensity_sum[channel] += intensity;
        fast_path = true;
    }

    if (!fast_path) for (uint32_t pixel = 0; pixel < begin_config_.pixel_width; ++pixel) {
        const uint32_t channel_begin = (begin_config_.sequential_channels ||
                                        begin_config_.dispatch_planar_rows)
            ? meta.plane_index : 0;
        const uint32_t channel_end = (begin_config_.sequential_channels ||
                                      begin_config_.dispatch_planar_rows)
            ? channel_begin + 1 : channels;
        for (uint32_t channel_index = channel_begin; channel_index < channel_end; ++channel_index) {
            if (channel_index >= channels || channel_index >= config_.channel_count) {
                return fail_input_(Err::INVALID_ARGUMENT, meta.row_index);
            }
            uint32_t channel = channel_index;
            if (channels == 4 && begin_config_.channel_order[0] == RasterChannel::Unknown) {
                channel = channel_index;
            } else {
                switch (begin_config_.channel_order[channel_index]) {
                    case RasterChannel::Cyan: channel = 0; break;
                    case RasterChannel::Magenta: channel = 1; break;
                    case RasterChannel::Yellow: channel = 2; break;
                    case RasterChannel::Black: channel = 3; break;
                    default: channel = channel_index; break;
                }
            }
            const uint32_t order_index = begin_config_.raster_layout == RasterLayout::Interleaved
                ? (pixel * pixel_bits + channel_index * bits)
                : ((begin_config_.sequential_channels || begin_config_.dispatch_planar_rows)
                    ? pixel * bits
                    : (channel_index * begin_config_.pixel_width + pixel) * bits);
            const uint32_t byte_index = order_index / 8;
            const uint32_t shift = 8 - bits - (order_index & 7);
            uint32_t value = 0;
            if (bits == 8) {
                value = data[byte_index];
            } else {
                const uint32_t mask = (1u << bits) - 1u;
                value = (data[byte_index] >> shift) & mask;
                value = (begin_config_.pixel_bits == 1)
                    ? (value != 0 ? 255 : 0)
                    : value * 255 / mask;
                if (begin_config_.channel_count == 4 && begin_config_.channel_order[0] == RasterChannel::Unknown) {
                    value = (value != 0 ? 255u : 0u);
                }
            }
            const uint8_t byte = static_cast<uint8_t>(value);
            channel_crc_state_[channel] = crc32_update(
                channel_crc_state_[channel], &byte, 1);
            if (config_.missing_nozzle_ppm != 0 && should_drop_(meta.row_index, pixel, channel)) {
                if (begin_config_.pixel_bits == 0 || value != 0) {
                    ++stats_.missing_nozzle_drops[channel];
                }
            } else {
                if (value != 0) ++stats_.nonzero_pixels[channel];
                stats_.intensity_sum[channel] += value;
            }
        }
    }

    ++stats_.rows_accepted;
    stats_.bytes_accepted += len;
    stats_.last_row_index = meta.row_index;
    if ((begin_config_.sequential_channels || begin_config_.dispatch_planar_rows) &&
        begin_config_.rows_per_plane != 0) {
        if (++next_row_index_ >= begin_config_.rows_per_plane) {
            next_row_index_ = 0;
            ++next_plane_index_;
        }
    } else {
        next_row_index_ = meta.row_index + 1;
    }

    // A sub-millisecond logical row period cannot be represented by the
    // millisecond DelayFn. Do not round every fast row up to 1 ms; that turns
    // a 10k rows/s request into an artificial 1k rows/s ceiling.
    if (effective_row_period_us_ >= 1000u) {
        const uint32_t row_period_ms = ceil_div_u32(effective_row_period_us_, 1000u);
        delay_ms_(row_period_ms);
    }

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
