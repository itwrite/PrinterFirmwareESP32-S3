// test_apple_raster_decoder.cpp - Verified Apple Raster subset tests.
#include "assert.h"

#include <cstdio>
#include <cstdint>
#include <algorithm>
#include <string>
#include <vector>

#include "airprint_core/raster_decoder.h"

using namespace ijpd::airprint;

namespace {

uint32_t crc32_update(uint32_t crc, const uint8_t* data, size_t length) {
    crc = ~crc;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

bool read_file(const char* path, std::vector<uint8_t>* output) {
    if (path == nullptr || output == nullptr) return false;
    std::FILE* file = std::fopen(path, "rb");
    if (file == nullptr) return false;
    if (std::fseek(file, 0, SEEK_END) != 0) {
        std::fclose(file);
        return false;
    }
    const long size = std::ftell(file);
    if (size < 0 || std::fseek(file, 0, SEEK_SET) != 0) {
        std::fclose(file);
        return false;
    }
    output->assign(static_cast<size_t>(size), 0);
    const size_t read = std::fread(output->data(), 1, output->size(), file);
    std::fclose(file);
    return read == output->size();
}

struct SinkState {
    RasterPageDescriptor page{};
    std::vector<RasterPageDescriptor> pages_seen;
    std::vector<std::vector<uint8_t>> row_data;
    std::vector<uint32_t> row_indices;
    std::vector<RasterDecoderError> end_statuses;
    std::vector<char> events;
    uint32_t pages = 0;
    uint32_t rows = 0;
    uint32_t page_rows = 0;
    uint32_t last_row = 0;
    uint32_t crc = 0;
    bool ended = false;
    RasterDecoderError end_status = RasterDecoderError::InvalidState;
    bool canceled = false;
    RasterDecoderError begin_result = RasterDecoderError::Ok;
    RasterDecoderError write_result = RasterDecoderError::Ok;
    RasterDecoderError end_result = RasterDecoderError::Ok;
    RasterDecoderError cancel_result = RasterDecoderError::Ok;
};

RasterDecoderError begin_page(void* user, const RasterPageDescriptor& page) {
    auto* state = static_cast<SinkState*>(user);
    state->events.push_back('B');
    if (state->begin_result != RasterDecoderError::Ok) return state->begin_result;
    state->page = page;
    state->pages_seen.push_back(page);
    state->page_rows = 0;
    ++state->pages;
    return RasterDecoderError::Ok;
}

RasterDecoderError write_row(void* user, const uint8_t* data, size_t length,
                             uint32_t row_index) {
    auto* state = static_cast<SinkState*>(user);
    state->events.push_back('W');
    if (state->write_result != RasterDecoderError::Ok) return state->write_result;
    if (data == nullptr || length != state->page.bytes_per_row ||
        row_index != state->page_rows) {
        return RasterDecoderError::BackendError;
    }
    state->row_data.emplace_back(data, data + length);
    state->row_indices.push_back(row_index);
    state->crc = crc32_update(state->crc, data, length);
    state->last_row = row_index;
    ++state->page_rows;
    ++state->rows;
    return RasterDecoderError::Ok;
}

RasterDecoderError end_page(void* user, RasterDecoderError status) {
    auto* state = static_cast<SinkState*>(user);
    state->events.push_back('E');
    state->ended = true;
    state->end_status = status;
    state->end_statuses.push_back(status);
    return state->end_result;
}

RasterDecoderError cancel_sink(void* user) {
    auto* state = static_cast<SinkState*>(user);
    state->events.push_back('C');
    state->canceled = true;
    return state->cancel_result;
}

RasterSink make_sink(SinkState* state) {
    return RasterSink{state, begin_page, write_row, end_page, cancel_sink};
}

bool decode_golden(const char* path, uint32_t expected_bytes, const char* expected_crc) {
    std::vector<uint8_t> document;
    CHECK_TRUE(read_file(path, &document));
    CHECK_EQ(document.size(), expected_bytes);

    SinkState sink_state;
    AppleRasterDecoder decoder;
    RasterDecoderLimits limits;
    CHECK_TRUE(decoder.begin(limits, make_sink(&sink_state)) == RasterDecoderError::Ok);

    size_t offset = 0;
    while (offset < document.size()) {
        const size_t chunk = (document.size() - offset < 137)
                                 ? document.size() - offset
                                 : 137;
        size_t consumed = 0;
        const RasterDecoderError error = decoder.feed(document.data() + offset, chunk,
                                                       &consumed);
        CHECK_TRUE(error == RasterDecoderError::NeedMoreData ||
                   error == RasterDecoderError::Ok);
        CHECK_TRUE(consumed != 0 || error == RasterDecoderError::Ok);
        offset += consumed;
        if (error == RasterDecoderError::Ok && decoder.page_started() == false &&
            offset == document.size()) {
            break;
        }
    }
    CHECK_EQ(offset, document.size());
    CHECK_TRUE(decoder.finish() == RasterDecoderError::Ok);
    CHECK_TRUE(sink_state.ended);
    CHECK_TRUE(sink_state.end_status == RasterDecoderError::Ok);
    CHECK_EQ(sink_state.pages, 1u);
    CHECK_EQ(sink_state.rows, 1100u);
    CHECK_EQ(sink_state.page.width_px, 850u);
    CHECK_EQ(sink_state.page.height_px, 1100u);
    CHECK_EQ(sink_state.page.bytes_per_row, 850u);
    CHECK_EQ(sink_state.page.dpi_x, 100u);
    CHECK_EQ(sink_state.page.dpi_y, 100u);
    CHECK_TRUE(sink_state.page.pixel_format == RasterPixelFormat::Mono8);
    CHECK_EQ_STR(raster_decoder_error_name(RasterDecoderError::Ok), "ok");
    char crc_text[9] = {};
    std::snprintf(crc_text, sizeof(crc_text), "%08x", sink_state.crc);
    CHECK_EQ_STR(crc_text, expected_crc);
    return true;
}

std::vector<uint8_t> small_document() {
    std::vector<uint8_t> value;
    const uint8_t prefix[] = {
        'U', 'N', 'I', 'R', 'A', 'S', 'T', 0,
        0, 0, 0, 1,
        8, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 4, 0, 0, 0, 2, 0, 0, 0, 100,
        0, 0, 0, 0, 0, 0, 0, 0,
    };
    value.insert(value.end(), prefix, prefix + sizeof(prefix));
    const uint8_t rows[] = {
        0, 0xFD, 1, 2, 3, 4,
        0, 3, 0xAA,
    };
    value.insert(value.end(), rows, rows + sizeof(rows));
    return value;
}

std::vector<uint8_t> small_rgb_document() {
    std::vector<uint8_t> value{
        'U', 'N', 'I', 'R', 'A', 'S', 'T', 0,
        0, 0, 0, 1,
        24, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 2, 0, 0, 0, 1, 0, 0, 0, 100,
        0, 0, 0, 0, 0, 0, 0, 0,
        0, 0xFF, 255, 0, 0, 0, 0, 255,
    };
    return value;
}

std::vector<uint8_t> small_cmyk_document() {
    std::vector<uint8_t> value{
        'U', 'N', 'I', 'R', 'A', 'S', 'T', 0,
        0, 0, 0, 1,
        32, 6, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 2, 0, 0, 0, 1, 0, 0, 0, 100,
        0, 0, 0, 0, 0, 0, 0, 0,
        0, 0xFF, 0, 255, 255, 0, 255, 0, 0, 0,
    };
    return value;
}

void append_be32(std::vector<uint8_t>* output, uint32_t value) {
    output->push_back(static_cast<uint8_t>(value >> 24));
    output->push_back(static_cast<uint8_t>(value >> 16));
    output->push_back(static_cast<uint8_t>(value >> 8));
    output->push_back(static_cast<uint8_t>(value));
}

struct PageSpec {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t dpi = 0;
    std::vector<uint8_t> encoded_rows;
};

std::vector<uint8_t> make_document(const std::vector<PageSpec>& pages) {
    std::vector<uint8_t> output;
    const uint8_t magic[] = {'U', 'N', 'I', 'R', 'A', 'S', 'T', 0};
    output.insert(output.end(), magic, magic + sizeof(magic));
    append_be32(&output, static_cast<uint32_t>(pages.size()));
    for (const PageSpec& page : pages) {
        std::vector<uint8_t> header(32, 0);
        header[0] = 8;
        header[2] = 1;
        auto set_be32 = [&header](size_t offset, uint32_t value) {
            header[offset] = static_cast<uint8_t>(value >> 24);
            header[offset + 1] = static_cast<uint8_t>(value >> 16);
            header[offset + 2] = static_cast<uint8_t>(value >> 8);
            header[offset + 3] = static_cast<uint8_t>(value);
        };
        set_be32(12, page.width);
        set_be32(16, page.height);
        set_be32(20, page.dpi);
        output.insert(output.end(), header.begin(), header.end());
        output.insert(output.end(), page.encoded_rows.begin(), page.encoded_rows.end());
    }
    return output;
}

bool decode_ok(const std::vector<uint8_t>& document, SinkState* sink_state,
               size_t chunk_size = 0, const RasterDecoderLimits& limits = {}) {
    AppleRasterDecoder decoder;
    if (decoder.begin(limits, make_sink(sink_state)) != RasterDecoderError::Ok) return false;
    size_t offset = 0;
    if (chunk_size == 0) chunk_size = document.size();
    while (offset < document.size()) {
        const size_t length = std::min(chunk_size, document.size() - offset);
        size_t consumed = 0;
        const RasterDecoderError error = decoder.feed(document.data() + offset, length,
                                                      &consumed);
        if (error != RasterDecoderError::NeedMoreData && error != RasterDecoderError::Ok) {
            return false;
        }
        if (consumed == 0) return false;
        offset += consumed;
        if (error == RasterDecoderError::Ok && consumed == 0) return false;
    }
    return decoder.finish() == RasterDecoderError::Ok;
}

RasterDecoderError decode_error(const std::vector<uint8_t>& document, SinkState* sink_state,
                                size_t* consumed = nullptr) {
    AppleRasterDecoder decoder;
    const RasterDecoderError begin_error = decoder.begin({}, make_sink(sink_state));
    if (begin_error != RasterDecoderError::Ok) return begin_error;
    size_t local_consumed = 0;
    const RasterDecoderError error = decoder.feed(document.data(), document.size(),
                                                   &local_consumed);
    if (consumed != nullptr) *consumed = local_consumed;
    return error;
}

}  // namespace

int main(int argc, char** argv) {
    CHECK_TRUE(argc == 1 || argc >= 3);
    if (argc >= 3) {
        decode_golden(argv[1], 707738, "9b3ac4d5");
        decode_golden(argv[2], 722531, "ea4be99e");
    }

    const std::vector<uint8_t> small = small_document();
    SinkState rgb_sink;
    CHECK_TRUE(decode_ok(small_rgb_document(), &rgb_sink));
    CHECK_EQ(rgb_sink.pages, 1u);
    CHECK_EQ(rgb_sink.rows, 1u);
    CHECK_TRUE(rgb_sink.page.pixel_format == RasterPixelFormat::RGB888);
    CHECK_EQ(rgb_sink.page.bytes_per_row, 6u);
    SinkState cmyk_sink;
    CHECK_TRUE(decode_ok(small_cmyk_document(), &cmyk_sink));
    CHECK_EQ(cmyk_sink.pages, 1u);
    CHECK_EQ(cmyk_sink.rows, 1u);
    CHECK_TRUE(cmyk_sink.page.pixel_format == RasterPixelFormat::CMYK8888);
    CHECK_EQ(cmyk_sink.page.bytes_per_row, 8u);
    SinkState small_sink;
    AppleRasterDecoder small_decoder;
    CHECK_TRUE(small_decoder.begin({}, make_sink(&small_sink)) == RasterDecoderError::Ok);
    size_t consumed = 0;
    CHECK_TRUE(small_decoder.feed(small.data(), small.size(), &consumed) ==
               RasterDecoderError::Ok);
    CHECK_EQ(consumed, small.size());
    CHECK_TRUE(small_decoder.finish() == RasterDecoderError::Ok);
    CHECK_EQ(small_sink.rows, 2u);
    CHECK_TRUE(small_sink.page.pixel_format == RasterPixelFormat::Mono8);
    CHECK_EQ(small_decoder.decoded_bytes(), 8u);
    const std::vector<uint8_t> small_row_0{1, 2, 3, 4};
    const std::vector<uint8_t> small_row_1{0xAA, 0xAA, 0xAA, 0xAA};
    CHECK_EQ_MEM(small_sink.row_data[0].data(), small_row_0.data(), 4);
    CHECK_EQ_MEM(small_sink.row_data[1].data(), small_row_1.data(), 4);

    std::vector<uint8_t> trailing_document = small;
    trailing_document.push_back(0xEE);
    SinkState trailing_sink;
    AppleRasterDecoder trailing_decoder;
    CHECK_TRUE(trailing_decoder.begin({}, make_sink(&trailing_sink)) ==
               RasterDecoderError::Ok);
    consumed = 0;
    CHECK_TRUE(trailing_decoder.feed(trailing_document.data(), trailing_document.size(),
                                     &consumed) == RasterDecoderError::Ok);
    CHECK_EQ(consumed, small.size());
    CHECK_TRUE(trailing_decoder.finish() == RasterDecoderError::Ok);

    const PageSpec edge_page{
        4,
        3,
        100,
        {0, 0, 0x09, 0x80, 0, 3, 0xA5, 0, 0xFD, 1, 2, 3, 4},
    };
    const std::vector<uint8_t> edge_document = make_document({edge_page});
    SinkState edge_sink;
    CHECK_TRUE(decode_ok(edge_document, &edge_sink, 1));
    CHECK_EQ(edge_sink.rows, 3u);
    const std::vector<uint8_t> edge_row_0{9, 0xFF, 0xFF, 0xFF};
    const std::vector<uint8_t> edge_row_1{0xA5, 0xA5, 0xA5, 0xA5};
    const std::vector<uint8_t> edge_row_2{1, 2, 3, 4};
    CHECK_EQ_MEM(edge_sink.row_data[0].data(), edge_row_0.data(), 4);
    CHECK_EQ_MEM(edge_sink.row_data[1].data(), edge_row_1.data(), 4);
    CHECK_EQ_MEM(edge_sink.row_data[2].data(), edge_row_2.data(), 4);
    CHECK_EQ_STR(std::string(edge_sink.events.begin(), edge_sink.events.end()).c_str(),
                 "BWWWE");

    std::vector<uint8_t> literal_values(128);
    for (size_t i = 0; i < literal_values.size(); ++i) {
        literal_values[i] = static_cast<uint8_t>(i);
    }
    SinkState literal_sink;
    std::vector<uint8_t> literal_rows{0, 0x81};
    literal_rows.insert(literal_rows.end(), literal_values.begin(), literal_values.end());
    CHECK_TRUE(decode_ok(make_document({PageSpec{128, 1, 100, literal_rows}}),
                         &literal_sink));
    CHECK_EQ(literal_sink.row_data[0].size(), 128u);
    CHECK_EQ(literal_sink.row_data[0][0], 0u);
    CHECK_EQ(literal_sink.row_data[0][127], 127u);

    SinkState repeat_sink;
    CHECK_TRUE(decode_ok(make_document({PageSpec{1, 256, 100, {0xFF, 0, 0x5A}}}),
                         &repeat_sink));
    CHECK_EQ(repeat_sink.rows, 256u);
    CHECK_EQ(repeat_sink.row_data[0][0], 0x5Au);
    CHECK_EQ(repeat_sink.row_data[255][0], 0x5Au);

    RasterDecoderLimits decoded_limit;
    decoded_limit.max_decoded_bytes = 7;
    SinkState decoded_limit_sink;
    AppleRasterDecoder decoded_limit_decoder;
    const std::vector<uint8_t> limited_document = small_document();
    CHECK_TRUE(decoded_limit_decoder.begin(decoded_limit, make_sink(&decoded_limit_sink)) ==
               RasterDecoderError::Ok);
    consumed = 0;
    CHECK_TRUE(decoded_limit_decoder.feed(limited_document.data(), limited_document.size(),
                                          &consumed) == RasterDecoderError::DataTooLarge);
    CHECK_EQ(decoded_limit_decoder.decoded_bytes(), 0u);
    CHECK_EQ(decoded_limit_sink.rows, 0u);

    RasterDecoderLimits exact_decoded_limit;
    exact_decoded_limit.max_decoded_bytes = 8;
    SinkState exact_decoded_sink;
    AppleRasterDecoder exact_decoded_decoder;
    CHECK_TRUE(exact_decoded_decoder.begin(exact_decoded_limit,
                                           make_sink(&exact_decoded_sink)) ==
               RasterDecoderError::Ok);
    consumed = 0;
    CHECK_TRUE(exact_decoded_decoder.feed(limited_document.data(), limited_document.size(),
                                          &consumed) == RasterDecoderError::Ok);
    CHECK_TRUE(exact_decoded_decoder.finish() == RasterDecoderError::Ok);
    CHECK_EQ(exact_decoded_decoder.decoded_bytes(), 8u);

    const std::vector<uint8_t> two_page_document = make_document({
        PageSpec{2, 1, 100, {0, 0xFF, 1, 2}},
        PageSpec{2, 1, 100, {0, 0xFF, 3, 4}},
    });
    SinkState two_page_sink;
    CHECK_TRUE(decode_ok(two_page_document, &two_page_sink, 2));
    CHECK_EQ(two_page_sink.pages, 2u);
    CHECK_TRUE(!two_page_sink.pages_seen[0].last_page);
    CHECK_TRUE(two_page_sink.pages_seen[1].last_page);
    CHECK_EQ(two_page_sink.row_indices[0], 0u);
    CHECK_EQ(two_page_sink.row_indices[1], 0u);
    const std::vector<uint8_t> first_page_row{1, 2};
    const std::vector<uint8_t> second_page_row{3, 4};
    CHECK_EQ_MEM(two_page_sink.row_data[0].data(), first_page_row.data(), 2);
    CHECK_EQ_MEM(two_page_sink.row_data[1].data(), second_page_row.data(), 2);

    std::vector<uint8_t> reserved_bytes = edge_document;
    reserved_bytes[12 + 3] = 7;
    reserved_bytes[12 + 6] = 0xA5;
    reserved_bytes[12 + 24] = 0x5A;
    SinkState reserved_sink;
    CHECK_TRUE(decode_ok(reserved_bytes, &reserved_sink));

    std::vector<uint8_t> bad_bits = edge_document;
    bad_bits[12] = 1;
    SinkState bad_bits_sink;
    CHECK_TRUE(decode_error(bad_bits, &bad_bits_sink) == RasterDecoderError::UnsupportedFormat);

    std::vector<uint8_t> zero_width = edge_document;
    zero_width[24] = 0;
    zero_width[25] = 0;
    zero_width[26] = 0;
    zero_width[27] = 0;
    SinkState zero_width_sink;
    CHECK_TRUE(decode_error(zero_width, &zero_width_sink) == RasterDecoderError::InvalidDimensions);

    std::vector<uint8_t> bad_dpi = edge_document;
    bad_dpi[32] = 0;
    bad_dpi[33] = 1;
    bad_dpi[34] = 0;
    bad_dpi[35] = 0;
    SinkState bad_dpi_sink;
    CHECK_TRUE(decode_error(bad_dpi, &bad_dpi_sink) == RasterDecoderError::InvalidDimensions);

    std::vector<uint8_t> bad_command = edge_document;
    bad_command[45] = 0xFC;  // Five literal bytes for a four-byte row.
    SinkState bad_command_sink;
    CHECK_TRUE(decode_error(bad_command, &bad_command_sink) == RasterDecoderError::InvalidStride);

    std::vector<uint8_t> truncated_literal = edge_document;
    truncated_literal.resize(47);
    truncated_literal[44] = 0;
    truncated_literal[45] = 0xFE;
    truncated_literal[46] = 1;
    SinkState truncated_literal_sink;
    AppleRasterDecoder truncated_literal_decoder;
    CHECK_TRUE(truncated_literal_decoder.begin({}, make_sink(&truncated_literal_sink)) ==
               RasterDecoderError::Ok);
    consumed = 0;
    CHECK_TRUE(truncated_literal_decoder.feed(truncated_literal.data(),
                                              truncated_literal.size(), &consumed) ==
               RasterDecoderError::NeedMoreData);
    CHECK_TRUE(truncated_literal_decoder.finish() == RasterDecoderError::Truncated);

    SinkState begin_failure_sink;
    begin_failure_sink.begin_result = RasterDecoderError::BackendError;
    AppleRasterDecoder begin_failure_decoder;
    CHECK_TRUE(begin_failure_decoder.begin({}, make_sink(&begin_failure_sink)) ==
               RasterDecoderError::Ok);
    consumed = 0;
    CHECK_TRUE(begin_failure_decoder.feed(edge_document.data(), edge_document.size(), &consumed) ==
               RasterDecoderError::BackendError);
    CHECK_EQ_STR(std::string(begin_failure_sink.events.begin(), begin_failure_sink.events.end()).c_str(),
                 "B");

    SinkState write_failure_sink;
    write_failure_sink.write_result = RasterDecoderError::BackendError;
    AppleRasterDecoder write_failure_decoder;
    CHECK_TRUE(write_failure_decoder.begin({}, make_sink(&write_failure_sink)) ==
               RasterDecoderError::Ok);
    consumed = 0;
    CHECK_TRUE(write_failure_decoder.feed(edge_document.data(), edge_document.size(), &consumed) ==
               RasterDecoderError::BackendError);
    CHECK_EQ_STR(std::string(write_failure_sink.events.begin(), write_failure_sink.events.end()).c_str(),
                 "BWE");
    CHECK_TRUE(write_failure_sink.end_status == RasterDecoderError::BackendError);

    SinkState cancel_page_sink;
    AppleRasterDecoder cancel_page_decoder;
    CHECK_TRUE(cancel_page_decoder.begin({}, make_sink(&cancel_page_sink)) ==
               RasterDecoderError::Ok);
    consumed = 0;
    CHECK_TRUE(cancel_page_decoder.feed(edge_document.data(), 44, &consumed) ==
               RasterDecoderError::NeedMoreData);
    CHECK_TRUE(cancel_page_decoder.cancel() == RasterDecoderError::Canceled);
    CHECK_EQ_STR(std::string(cancel_page_sink.events.begin(), cancel_page_sink.events.end()).c_str(),
                 "BCE");
    CHECK_TRUE(cancel_page_sink.end_status == RasterDecoderError::Canceled);

    SinkState failed_cancel_sink;
    AppleRasterDecoder failed_cancel_decoder;
    CHECK_TRUE(failed_cancel_decoder.begin({}, make_sink(&failed_cancel_sink)) ==
               RasterDecoderError::Ok);
    consumed = 0;
    CHECK_TRUE(failed_cancel_decoder.feed(bad_bits.data(), bad_bits.size(), &consumed) ==
               RasterDecoderError::UnsupportedFormat);
    CHECK_TRUE(failed_cancel_decoder.cancel() == RasterDecoderError::UnsupportedFormat);

    std::vector<uint8_t> bad_magic = small;
    bad_magic[0] = 'X';
    AppleRasterDecoder bad_magic_decoder;
    SinkState bad_magic_sink;
    CHECK_TRUE(bad_magic_decoder.begin({}, make_sink(&bad_magic_sink)) == RasterDecoderError::Ok);
    consumed = 0;
    CHECK_TRUE(bad_magic_decoder.feed(bad_magic.data(), bad_magic.size(), &consumed) ==
               RasterDecoderError::UnsupportedFormat);

    std::vector<uint8_t> bad_color = small;
    bad_color[13] = 1;
    AppleRasterDecoder bad_color_decoder;
    SinkState bad_color_sink;
    CHECK_TRUE(bad_color_decoder.begin({}, make_sink(&bad_color_sink)) == RasterDecoderError::Ok);
    consumed = 0;
    CHECK_TRUE(bad_color_decoder.feed(bad_color.data(), bad_color.size(), &consumed) ==
               RasterDecoderError::UnsupportedFormat);

    std::vector<uint8_t> bad_repeat = small;
    bad_repeat[44] = 2;  // Three rows for a two-row page.
    AppleRasterDecoder bad_repeat_decoder;
    SinkState bad_repeat_sink;
    CHECK_TRUE(bad_repeat_decoder.begin({}, make_sink(&bad_repeat_sink)) ==
               RasterDecoderError::Ok);
    consumed = 0;
    CHECK_TRUE(bad_repeat_decoder.feed(bad_repeat.data(), bad_repeat.size(), &consumed) ==
               RasterDecoderError::InvalidDimensions);

    AppleRasterDecoder truncated_decoder;
    SinkState truncated_sink;
    CHECK_TRUE(truncated_decoder.begin({}, make_sink(&truncated_sink)) == RasterDecoderError::Ok);
    consumed = 0;
    CHECK_TRUE(truncated_decoder.feed(small.data(), small.size() - 1, &consumed) ==
               RasterDecoderError::NeedMoreData);
    CHECK_TRUE(truncated_decoder.finish() == RasterDecoderError::Truncated);

    AppleRasterDecoder limited_decoder;
    SinkState limited_sink;
    RasterDecoderLimits limited;
    limited.max_document_bytes = 8;
    CHECK_TRUE(limited_decoder.begin(limited, make_sink(&limited_sink)) == RasterDecoderError::Ok);
    consumed = 0;
    CHECK_TRUE(limited_decoder.feed(small.data(), small.size(), &consumed) ==
               RasterDecoderError::DataTooLarge);

    AppleRasterDecoder canceled_decoder;
    SinkState canceled_sink;
    CHECK_TRUE(canceled_decoder.begin({}, make_sink(&canceled_sink)) == RasterDecoderError::Ok);
    CHECK_TRUE(canceled_decoder.cancel() == RasterDecoderError::Canceled);
    CHECK_TRUE(canceled_sink.canceled);

    return ::ijpd::test::summary("test_apple_raster_decoder");
}
