// test_ricoh_mh5440_pipeline.cpp - PrintPipeline -> Ricoh logical Mock slice.
#include "assert.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "core/byte_order.h"
#include "core/crc32.h"
#include "core/print_pipeline.h"
#include "printer_hal/ricoh_mh5440_mock_hal.h"

using namespace ijpd;

namespace {

constexpr size_t kWidthPixels = 320;
constexpr size_t kChannels = 4;
constexpr size_t kBytesPerRow = kWidthPixels * kChannels;
constexpr size_t kRows = 5;
constexpr size_t kTotalBytes = kBytesPerRow * kRows;
constexpr size_t kPacketCount = 4;

struct JobSpec {
    uint32_t token = 0;
    uint32_t packets = 0;
    size_t bytes = 0;
    size_t bytes_per_row = 0;
};

std::vector<uint8_t> make_cmyk_document() {
    std::vector<uint8_t> data(kTotalBytes);
    for (size_t row = 0; row < kRows; ++row) {
        for (size_t pixel = 0; pixel < kWidthPixels; ++pixel) {
            const size_t offset = row * kBytesPerRow + pixel * kChannels;
            data[offset + 0] = static_cast<uint8_t>((pixel + row * 3) & 0xFF);
            data[offset + 1] = static_cast<uint8_t>((pixel * 2 + row * 5) & 0xFF);
            data[offset + 2] = static_cast<uint8_t>((pixel * 3 + row * 7) & 0xFF);
            data[offset + 3] = static_cast<uint8_t>((pixel * 5 + row * 11) & 0xFF);
        }
    }
    return data;
}

std::vector<uint8_t> build_stream(const JobSpec& spec, const std::vector<uint8_t>& data) {
    const size_t nominal = (spec.bytes + spec.packets - 1) / spec.packets;
    const size_t last = spec.bytes - nominal * (spec.packets - 1);
    std::vector<uint8_t> stream{'I', 'J', 'P', 'D'};
    uint8_t header[4] = {};
    write_u16_le(header, cfg::PROTOCOL_HELLO_VERSION);
    stream.insert(stream.end(), header, header + 2);
    write_u32_le(header, spec.token);
    stream.insert(stream.end(), header, header + 4);
    stream.push_back(0);
    stream.push_back(0);

    for (uint32_t packet = 0; packet < spec.packets; ++packet) {
        const size_t length = packet + 1 == spec.packets ? last : nominal;
        const uint8_t* payload = data.data() + packet * nominal;
        std::vector<uint8_t> frame(6);
        write_u16_le(frame.data(), static_cast<uint16_t>(packet));
        write_u16_le(frame.data() + 2, static_cast<uint16_t>(length));
        write_u16_le(frame.data() + 4, static_cast<uint16_t>(length));
        frame.insert(frame.end(), payload, payload + length);
        uint8_t crc[4] = {};
        write_u32_le(crc, crc32(frame.data(), frame.size()));
        frame.insert(frame.end(), crc, crc + sizeof(crc));
        stream.insert(stream.end(), frame.begin(), frame.end());
    }
    return stream;
}

struct Harness {
    uint8_t slots[cfg::SLOT_COUNT * 4096] = {};
    uint8_t comp[2][cfg::MAX_PACKET_COMPRESSED_SIZE + 1] = {};
    uint8_t carry[kBytesPerRow] = {};
    PrintPipeline pipeline;
    MockRicohMh5440Hal hal;
    std::vector<PrintPipeline::Event> events;
    std::mutex events_mutex;
    std::atomic<uint64_t> now{1000};

    explicit Harness(const RicohMh5440MockConfig& config) : hal(config) {}

    static uint64_t now_fn(void* user) {
        return static_cast<Harness*>(user)->now.load(std::memory_order_relaxed);
    }

    static Err print_row_fn(void* user, const uint8_t* data, size_t length,
                            uint32_t row_index) {
        auto* harness = static_cast<Harness*>(user);
        return harness->hal.submitRaster(data, length, RasterMeta{row_index});
    }

    static void event_fn(void* user, const PrintPipeline::Event& event) {
        auto* harness = static_cast<Harness*>(user);
        std::lock_guard<std::mutex> lock(harness->events_mutex);
        harness->events.push_back(event);
    }

    PrintPipeline::Config config(const JobSpec& spec) {
        PrintPipeline::Config value;
        value.job_id = "ricoh-mh5440-pipeline";
        value.data_token = spec.token;
        value.total_packets = spec.packets;
        value.total_bytes_uncompressed = spec.bytes;
        value.nominal_packet_bytes = static_cast<uint16_t>(
            (spec.bytes + spec.packets - 1) / spec.packets);
        value.last_packet_bytes = static_cast<uint16_t>(
            spec.bytes - value.nominal_packet_bytes * (spec.packets - 1));
        value.bytes_per_row = spec.bytes_per_row;
        value.total_rows = static_cast<uint32_t>(spec.bytes / spec.bytes_per_row);
        value.compression = Compression::None;
        value.integrity = Integrity::Crc32;
        value.comp_buf[0] = comp[0];
        value.comp_buf[1] = comp[1];
        value.slot_memory = slots;
        value.slot_count = cfg::SLOT_COUNT;
        value.slot_size = 4096;
        value.row_carry = carry;
        return value;
    }
};

void drive(Harness* harness, const std::vector<uint8_t>& stream) {
    std::atomic<bool> stop{false};
    std::atomic<size_t> offset{0};
    std::thread rx([&]() {
        while (!stop.load(std::memory_order_relaxed)) {
            const size_t current = offset.load(std::memory_order_relaxed);
            if (current >= stream.size()) break;
            const size_t requested = std::min<size_t>(73, stream.size() - current);
            size_t consumed = 0;
            const PrintPipeline::Pump result = harness->pipeline.rx_pump(
                stream.data() + current, requested, &consumed);
            if (result == PrintPipeline::Pump::Failed) break;
            if (consumed != 0) offset.fetch_add(consumed, std::memory_order_relaxed);
            else std::this_thread::yield();
        }
    });
    std::thread decode([&]() {
        while (!stop.load(std::memory_order_relaxed)) {
            if (harness->pipeline.decode_pump() != PrintPipeline::Pump::Progress) {
                std::this_thread::yield();
            }
        }
    });
    std::thread print([&]() {
        while (!stop.load(std::memory_order_relaxed)) {
            if (harness->pipeline.print_pump() != PrintPipeline::Pump::Progress) {
                std::this_thread::yield();
            }
        }
    });

    for (uint32_t tick = 0; tick < 30000; ++tick) {
        harness->now.fetch_add(1, std::memory_order_relaxed);
        harness->pipeline.tick(harness->now.load(std::memory_order_relaxed));
        if (harness->pipeline.terminal() ||
            harness->pipeline.state() == JobState::Finalizing) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    stop.store(true, std::memory_order_relaxed);
    rx.join();
    decode.join();
    print.join();
}

void check_stats(const RicohMh5440MockStats& actual,
                 const std::vector<uint8_t>& data,
                 uint64_t expected_rows) {
    uint64_t nonzero[4] = {};
    uint64_t intensity[4] = {};
    uint32_t crc_state[4] = {
        CRC32_INIT_STATE, CRC32_INIT_STATE, CRC32_INIT_STATE, CRC32_INIT_STATE};
    for (size_t pixel = 0; pixel < data.size() / 4; ++pixel) {
        for (size_t channel = 0; channel < 4; ++channel) {
            const uint8_t value = data[pixel * 4 + channel];
            if (value != 0) ++nonzero[channel];
            intensity[channel] += value;
            crc_state[channel] = crc32_update(crc_state[channel], &value, 1);
        }
    }
    CHECK_EQ(actual.rows_accepted, expected_rows);
    CHECK_EQ(actual.bytes_accepted, data.size());
    for (size_t channel = 0; channel < 4; ++channel) {
        CHECK_EQ(actual.nonzero_pixels[channel], nonzero[channel]);
        CHECK_EQ(actual.intensity_sum[channel], intensity[channel]);
        CHECK_EQ(actual.channel_crc32[channel], crc32_final(crc_state[channel]));
    }
}

}  // namespace

int main() {
    const std::vector<uint8_t> data = make_cmyk_document();
    const JobSpec spec{0x54484D34u, kPacketCount, data.size(), kBytesPerRow};
    const std::vector<uint8_t> stream = build_stream(spec, data);

    RicohMh5440MockConfig config;
    config.row_period_us = 0;
    Harness normal(config);
    CHECK_TRUE(normal.hal.begin(PrintheadConfig{static_cast<uint32_t>(kBytesPerRow), 1, 0}) ==
               Err::OK);
    CHECK_TRUE(normal.pipeline.start(normal.config(spec),
                                     {&normal, Harness::now_fn, Harness::print_row_fn,
                                      nullptr, Harness::event_fn}) == Err::OK);
    drive(&normal, stream);
    CHECK_TRUE(normal.pipeline.state() == JobState::Finalizing);
    CHECK_TRUE(normal.pipeline.finish(false, Err::OK) == Err::OK);
    CHECK_TRUE(normal.pipeline.state() == JobState::Completed);
    CHECK_TRUE(normal.pipeline.all_slots_free());
    CHECK_TRUE(normal.hal.waitUntilIdle(1) == Err::OK);
    check_stats(normal.hal.stats(), data, kRows);
    CHECK_EQ(normal.pipeline.snapshot().bytes_printed, data.size());
    bool completed = false;
    for (const PrintPipeline::Event& event : normal.events) {
        if (event.kind == PrintPipeline::Event::Kind::Completed) completed = true;
    }
    CHECK_TRUE(completed);

    RicohMh5440MockConfig fault_config = config;
    fault_config.fault_after_rows = 3;
    Harness faulty(fault_config);
    CHECK_TRUE(faulty.hal.begin(PrintheadConfig{static_cast<uint32_t>(kBytesPerRow), 1, 0}) ==
               Err::OK);
    CHECK_TRUE(faulty.pipeline.start(faulty.config(spec),
                                     {&faulty, Harness::now_fn, Harness::print_row_fn,
                                      nullptr, Harness::event_fn}) == Err::OK);
    drive(&faulty, stream);
    CHECK_TRUE(faulty.pipeline.state() == JobState::Error);
    CHECK_TRUE(faulty.pipeline.last_error() == Err::HAL_FAULT);
    CHECK_TRUE(faulty.pipeline.all_slots_free());
    CHECK_EQ(faulty.hal.stats().rows_accepted, 3u);
    CHECK_EQ(faulty.hal.stats().bytes_accepted, 3u * kBytesPerRow);
    bool failed = false;
    for (const PrintPipeline::Event& event : faulty.events) {
        if (event.kind == PrintPipeline::Event::Kind::Failed &&
            event.code == Err::HAL_FAULT) {
            failed = true;
        }
    }
    CHECK_TRUE(failed);

    return ::ijpd::test::summary("test_ricoh_mh5440_pipeline");
}
