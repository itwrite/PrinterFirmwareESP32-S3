// fuzz_apple_raster.cpp - deterministic malformed Apple Raster smoke fuzz.
#include "assert.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "airprint_core/raster_decoder.h"

using namespace ijpd::airprint;

namespace {

struct Rng {
    uint32_t state = 0;

    explicit Rng(uint32_t seed) : state(seed) {}

    uint32_t next() noexcept {
        state = state * 1664525u + 1013904223u;
        return state;
    }

    uint32_t below(uint32_t limit) noexcept { return next() % limit; }
    uint8_t byte() noexcept { return static_cast<uint8_t>(next() >> 24); }
};

void append_be32(std::vector<uint8_t>* output, uint32_t value) {
    output->push_back(static_cast<uint8_t>(value >> 24));
    output->push_back(static_cast<uint8_t>(value >> 16));
    output->push_back(static_cast<uint8_t>(value >> 8));
    output->push_back(static_cast<uint8_t>(value));
}

struct ValidDocument {
    std::vector<uint8_t> bytes;
    uint32_t pages = 0;
    uint32_t rows = 0;
    uint64_t decoded_bytes = 0;
};

ValidDocument make_valid_document(Rng* rng) {
    ValidDocument document;
    document.pages = 1 + rng->below(3);
    const uint8_t magic[] = {'U', 'N', 'I', 'R', 'A', 'S', 'T', 0};
    document.bytes.insert(document.bytes.end(), magic, magic + sizeof(magic));
    append_be32(&document.bytes, document.pages);

    for (uint32_t page_index = 0; page_index < document.pages; ++page_index) {
        const uint32_t width = 1 + rng->below(64);
        const uint32_t height = 1 + rng->below(64);
        const uint32_t dpi = 72 + rng->below(129);
        std::vector<uint8_t> header(32, 0);
        header[0] = 8;
        header[1] = 0;
        header[2] = static_cast<uint8_t>(page_index & 1u);
        auto set_be32 = [&header](size_t offset, uint32_t value) {
            header[offset] = static_cast<uint8_t>(value >> 24);
            header[offset + 1] = static_cast<uint8_t>(value >> 16);
            header[offset + 2] = static_cast<uint8_t>(value >> 8);
            header[offset + 3] = static_cast<uint8_t>(value);
        };
        set_be32(12, width);
        set_be32(16, height);
        set_be32(20, dpi);
        document.bytes.insert(document.bytes.end(), header.begin(), header.end());

        uint32_t rows_left = height;
        while (rows_left != 0) {
            const uint32_t repeat = 1 + rng->below(rows_left);
            document.bytes.push_back(static_cast<uint8_t>(repeat - 1));
            const uint32_t mode = rng->below(3);
            if (mode == 0) {
                document.bytes.push_back(0x80);  // Fill the row with 0xFF.
            } else if (mode == 1 || width == 1) {
                document.bytes.push_back(static_cast<uint8_t>(width - 1));
                document.bytes.push_back(rng->byte());
            } else {
                document.bytes.push_back(static_cast<uint8_t>(257u - width));
                for (uint32_t i = 0; i < width; ++i) document.bytes.push_back(rng->byte());
            }
            rows_left -= repeat;
            document.rows += repeat;
            document.decoded_bytes += static_cast<uint64_t>(width) * repeat;
        }
    }
    return document;
}

struct SinkState {
    RasterPageDescriptor page{};
    uint32_t pages = 0;
    uint32_t rows = 0;
    uint32_t page_rows = 0;
    bool contract_violation = false;
};

RasterDecoderError begin_page(void* user, const RasterPageDescriptor& page) {
    auto* state = static_cast<SinkState*>(user);
    if (page.width_px == 0 || page.height_px == 0 ||
        page.bytes_per_row != page.width_px || page.pixel_format != RasterPixelFormat::Mono8) {
        state->contract_violation = true;
        return RasterDecoderError::BackendError;
    }
    state->page = page;
    state->page_rows = 0;
    ++state->pages;
    return RasterDecoderError::Ok;
}

RasterDecoderError write_row(void* user, const uint8_t* data, size_t length,
                             uint32_t row_index) {
    auto* state = static_cast<SinkState*>(user);
    if (data == nullptr || length != state->page.bytes_per_row ||
        row_index != state->page_rows) {
        state->contract_violation = true;
        return RasterDecoderError::BackendError;
    }
    ++state->page_rows;
    ++state->rows;
    return RasterDecoderError::Ok;
}

RasterDecoderError end_page(void*, RasterDecoderError) {
    return RasterDecoderError::Ok;
}

RasterDecoderError cancel_sink(void*) {
    return RasterDecoderError::Ok;
}

RasterSink make_sink(SinkState* state) {
    return RasterSink{state, begin_page, write_row, end_page, cancel_sink};
}

void mutate(std::vector<uint8_t>* document, Rng* rng) {
    const uint32_t mutations = 1 + rng->below(5);
    for (uint32_t i = 0; i < mutations; ++i) {
        switch (rng->below(5)) {
            case 0:
                if (!document->empty()) {
                    (*document)[rng->below(static_cast<uint32_t>(document->size()))] = rng->byte();
                }
                break;
            case 1:
                document->resize(rng->below(static_cast<uint32_t>(document->size()) + 1));
                break;
            case 2: {
                const size_t count = 1 + rng->below(8);
                for (size_t j = 0; j < count; ++j) document->push_back(rng->byte());
                break;
            }
            case 3:
                if (document->size() >= 12) {
                    (*document)[8 + rng->below(4)] = rng->byte();
                }
                break;
            default:
                if (!document->empty()) {
                    const size_t at = rng->below(static_cast<uint32_t>(document->size()));
                    document->insert(document->begin() + static_cast<ptrdiff_t>(at), rng->byte());
                }
                break;
        }
    }
}

void run_one(Rng* rng) {
    const ValidDocument corpus = make_valid_document(rng);
    std::vector<uint8_t> input = corpus.bytes;
    const bool valid_input = rng->below(4) == 0;
    if (!valid_input) mutate(&input, rng);

    SinkState sink;
    AppleRasterDecoder decoder;
    RasterDecoderLimits limits;
    if (valid_input && rng->below(3) == 0) {
        limits.max_decoded_bytes = corpus.decoded_bytes;
    } else if (!valid_input && rng->below(4) == 0) {
        limits.max_decoded_bytes = 1 + rng->below(4096);
    }
    CHECK_TRUE(decoder.begin(limits, make_sink(&sink)) == RasterDecoderError::Ok);

    size_t offset = 0;
    bool terminal = false;
    for (uint32_t guard = 0; offset < input.size() && guard < 10000; ++guard) {
        const size_t requested = std::min<size_t>(1 + rng->below(32), input.size() - offset);
        size_t consumed = 0;
        const RasterDecoderError error = decoder.feed(input.data() + offset, requested, &consumed);
        CHECK_TRUE(consumed <= requested);
        if (consumed == 0) {
            CHECK_TRUE(error != RasterDecoderError::NeedMoreData);
            terminal = true;
            break;
        }
        offset += consumed;
        if (error != RasterDecoderError::Ok && error != RasterDecoderError::NeedMoreData) {
            terminal = true;
            break;
        }
        if (error == RasterDecoderError::Ok && consumed < requested) {
            terminal = true;
            break;
        }
    }
    CHECK_TRUE(offset == input.size() || terminal);
    CHECK_TRUE(sink.contract_violation == false);

    const RasterDecoderError finish = decoder.finish();
    if (valid_input) {
        CHECK_TRUE(finish == RasterDecoderError::Ok);
        CHECK_EQ(sink.pages, corpus.pages);
        CHECK_EQ(sink.rows, corpus.rows);
    }
}

}  // namespace

int main(int argc, char** argv) {
    const uint32_t iterations = argc > 1 ? static_cast<uint32_t>(std::atoi(argv[1])) : 50000u;
    Rng rng(0xA17E5EEDu);
    for (uint32_t i = 0; i < iterations; ++i) run_one(&rng);
    std::printf("apple raster fuzz: %u iterations\n", iterations);
    return ::ijpd::test::summary("fuzz_apple_raster");
}
