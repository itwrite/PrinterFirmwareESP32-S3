// test_ricoh_mh5440_mock_hal.cpp - logical RICOH MH5440 mock contract.
#include "assert.h"

#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/crc32.h"
#include "printer_hal/ricoh_mh5440_mock_hal.h"

using namespace ijpd;

namespace {

struct DelayState {
    std::vector<uint32_t> delays;
};

void record_delay(void* user, uint32_t ms) {
    static_cast<DelayState*>(user)->delays.push_back(ms);
}

RicohMh5440MockConfig small_config() {
    RicohMh5440MockConfig config;
    config.virtual_nozzles_per_channel = 4;
    config.logical_width_px = 4;
    config.row_bytes = 16;
    return config;
}

PrintheadConfig begin_config() {
    PrintheadConfig config;
    config.row_bytes = 16;
    return config;
}

std::vector<uint8_t> row_a() {
    return {
        0, 10, 20, 30,
        40, 0, 60, 70,
        80, 90, 0, 110,
        120, 130, 140, 0,
    };
}

std::vector<uint8_t> row_b() {
    return {
        1, 2, 3, 4,
        5, 6, 7, 8,
        9, 10, 11, 12,
        13, 14, 15, 16,
    };
}

uint32_t channel_crc(const std::vector<uint8_t>& row_a_value,
                     const std::vector<uint8_t>& row_b_value,
                     uint32_t channel) {
    uint32_t state = CRC32_INIT_STATE;
    for (const std::vector<uint8_t>* row : {&row_a_value, &row_b_value}) {
        for (size_t pixel = 0; pixel < row->size() / 4; ++pixel) {
            const uint8_t value = (*row)[pixel * 4 + channel];
            state = crc32_update(state, &value, 1);
        }
    }
    return crc32_final(state);
}

}  // namespace

int main() {
    const std::vector<uint8_t> first = row_a();
    const std::vector<uint8_t> second = row_b();

    // Normal CMYK8 path: channel metrics, CRC, row order and delay are deterministic.
    DelayState delay_state;
    RicohMh5440MockConfig config = small_config();
    config.row_period_us = 5000;
    config.startup_delay_ms = 3;
    MockRicohMh5440Hal hal(config, record_delay, &delay_state);
    CHECK_TRUE(hal.begin(begin_config()) == Err::OK);
    CHECK_TRUE(hal.configured());
    CHECK_EQ(hal.row_bytes(), 16u);
    CHECK_TRUE(hal.healthy());
    CHECK_TRUE(hal.submitRaster(first.data(), first.size(), RasterMeta{0}) == Err::OK);
    CHECK_TRUE(hal.submitRaster(second.data(), second.size(), RasterMeta{1}) == Err::OK);
    CHECK_TRUE(hal.waitUntilIdle(100) == Err::OK);
    const RicohMh5440MockStats stats = hal.stats();
    CHECK_EQ(stats.rows_accepted, 2u);
    CHECK_EQ(stats.bytes_accepted, 32u);
    CHECK_EQ(stats.last_row_index, 1u);
    CHECK_EQ(stats.nonzero_pixels[0], 7u);
    CHECK_EQ(stats.nonzero_pixels[1], 7u);
    CHECK_EQ(stats.nonzero_pixels[2], 7u);
    CHECK_EQ(stats.nonzero_pixels[3], 7u);
    CHECK_EQ(stats.intensity_sum[0], 268u);
    CHECK_EQ(stats.intensity_sum[1], 262u);
    CHECK_EQ(stats.intensity_sum[2], 256u);
    CHECK_EQ(stats.intensity_sum[3], 250u);
    for (uint32_t channel = 0; channel < 4; ++channel) {
        CHECK_EQ(stats.channel_crc32[channel], channel_crc(first, second, channel));
    }
    CHECK_EQ(delay_state.delays.size(), 3u);
    CHECK_EQ(delay_state.delays[0], 3u);
    CHECK_EQ(delay_state.delays[1], 5u);
    CHECK_EQ(delay_state.delays[2], 5u);

    // Input contract: short rows and non-monotonic row indices are rejected.
    MockRicohMh5440Hal input_hal(small_config());
    CHECK_TRUE(input_hal.begin(begin_config()) == Err::OK);
    CHECK_TRUE(input_hal.submitRaster(first.data(), first.size() - 1, RasterMeta{0}) ==
               Err::INVALID_ARGUMENT);
    CHECK_TRUE(input_hal.submitRaster(first.data(), first.size(), RasterMeta{2}) ==
               Err::INVALID_ARGUMENT);
    CHECK_TRUE(input_hal.stats().has_bad_row);
    CHECK_EQ(input_hal.stats().first_bad_row, 0u);
    CHECK_TRUE(input_hal.submitRaster(first.data(), first.size(), RasterMeta{0}) ==
               Err::INVALID_ARGUMENT);

    // Strict geometry catches a profile/configuration mismatch before accepting rows.
    RicohMh5440MockConfig invalid_geometry = small_config();
    invalid_geometry.logical_width_px = 5;
    MockRicohMh5440Hal geometry_hal(invalid_geometry);
    CHECK_TRUE(geometry_hal.begin(begin_config()) == Err::INVALID_ARGUMENT);
    CHECK_TRUE(!geometry_hal.configured());

    RicohMh5440MockConfig invalid_channels = small_config();
    invalid_channels.channel_count = 3;
    MockRicohMh5440Hal channel_hal(invalid_channels);
    CHECK_TRUE(channel_hal.begin(begin_config()) == Err::INVALID_ARGUMENT);

    // Deterministic missing-nozzle model: 100% drop keeps input CRC but removes metrics.
    RicohMh5440MockConfig missing = small_config();
    missing.missing_nozzle_ppm = 1000000;
    MockRicohMh5440Hal missing_hal(missing);
    CHECK_TRUE(missing_hal.begin(begin_config()) == Err::OK);
    CHECK_TRUE(missing_hal.submitRaster(first.data(), first.size(), RasterMeta{0}) == Err::OK);
    const RicohMh5440MockStats missing_stats = missing_hal.stats();
    CHECK_EQ(missing_stats.nonzero_pixels[0], 0u);
    CHECK_EQ(missing_stats.intensity_sum[3], 0u);
    CHECK_EQ(missing_stats.missing_nozzle_drops[0], 4u);
    CHECK_EQ(missing_stats.missing_nozzle_drops[3], 4u);
    CHECK_EQ(missing_stats.channel_crc32[0], channel_crc(first, {}, 0));

    // Row and byte fault injection happen deterministically at the configured boundary.
    RicohMh5440MockConfig row_fault = small_config();
    row_fault.fault_after_rows = 2;
    MockRicohMh5440Hal row_fault_hal(row_fault);
    CHECK_TRUE(row_fault_hal.begin(begin_config()) == Err::OK);
    CHECK_TRUE(row_fault_hal.submitRaster(first.data(), first.size(), RasterMeta{0}) == Err::OK);
    CHECK_TRUE(row_fault_hal.submitRaster(second.data(), second.size(), RasterMeta{1}) ==
               Err::HAL_FAULT);
    CHECK_TRUE(row_fault_hal.submitRaster(first.data(), first.size(), RasterMeta{2}) ==
               Err::HAL_FAULT);
    CHECK_TRUE(!row_fault_hal.healthy());
    CHECK_EQ(row_fault_hal.stats().fault_count, 1u);
    CHECK_TRUE(row_fault_hal.stop() == Err::OK);
    CHECK_TRUE(row_fault_hal.stop() == Err::OK);

    RicohMh5440MockConfig byte_fault = small_config();
    byte_fault.fault_after_bytes = 16;
    MockRicohMh5440Hal byte_fault_hal(byte_fault);
    CHECK_TRUE(byte_fault_hal.begin(begin_config()) == Err::OK);
    CHECK_TRUE(byte_fault_hal.submitRaster(first.data(), first.size(), RasterMeta{0}) ==
               Err::HAL_FAULT);
    CHECK_EQ(byte_fault_hal.stats().bytes_accepted, 16u);

    // Backpressure and paper/thermal faults are observable without real hardware.
    RicohMh5440MockConfig blocked = small_config();
    blocked.backpressure_after_rows = 1;
    MockRicohMh5440Hal blocked_hal(blocked);
    CHECK_TRUE(blocked_hal.begin(begin_config()) == Err::OK);
    CHECK_TRUE(blocked_hal.submitRaster(first.data(), first.size(), RasterMeta{0}) == Err::OK);
    CHECK_TRUE(blocked_hal.submitRaster(second.data(), second.size(), RasterMeta{1}) ==
               Err::QUEUE_TIMEOUT);
    CHECK_TRUE(blocked_hal.waitUntilIdle(10) == Err::QUEUE_TIMEOUT);
    CHECK_TRUE(!blocked_hal.healthy());

    RicohMh5440MockConfig jam = small_config();
    jam.inject_paper_jam = true;
    MockRicohMh5440Hal jam_hal(jam);
    CHECK_TRUE(jam_hal.begin(begin_config()) == Err::OK);
    CHECK_TRUE(jam_hal.submitRaster(first.data(), first.size(), RasterMeta{0}) == Err::HAL_FAULT);

    RicohMh5440MockConfig thermal = small_config();
    thermal.inject_thermal_fault = true;
    MockRicohMh5440Hal thermal_hal(thermal);
    CHECK_TRUE(thermal_hal.begin(begin_config()) == Err::HAL_FAULT);
    CHECK_TRUE(!thermal_hal.healthy());

    // This is a raster-only mock; future controller-specific raw support is explicit.
    MockRicohMh5440Hal raw_hal(small_config());
    CHECK_TRUE(raw_hal.begin(begin_config()) == Err::OK);
    CHECK_TRUE(raw_hal.submitRaw(first.data(), first.size()) == Err::UNSUPPORTED);
    RicohMh5440MockConfig raw_config = small_config();
    raw_config.allow_raw = true;
    MockRicohMh5440Hal raw_enabled(raw_config);
    CHECK_TRUE(raw_enabled.begin(begin_config()) == Err::OK);
    CHECK_TRUE(raw_enabled.submitRaw(first.data(), first.size()) == Err::OK);
    CHECK_EQ(raw_enabled.stats().raw_bytes_received, first.size());

    return ::ijpd::test::summary("test_ricoh_mh5440_mock_hal");
}
