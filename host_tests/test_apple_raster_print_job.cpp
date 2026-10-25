// test_apple_raster_print_job.cpp - Print-Job stream to Apple Raster sink slice.
#include "assert.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "airprint_core/apple_raster_print_backend.h"
#include "airprint_core/ipp_codec.h"
#include "airprint_core/ipp_job.h"
#include "airprint_core/ipp_print_job.h"

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
    uint32_t crc = 0;
    RasterDecoderError end_status = RasterDecoderError::InvalidState;
    bool canceled = false;
};

RasterDecoderError sink_begin(void* user, const RasterPageDescriptor& page) {
    auto* state = static_cast<SinkState*>(user);
    state->events.push_back('B');
    state->page = page;
    state->pages_seen.push_back(page);
    state->page_rows = 0;
    ++state->pages;
    return RasterDecoderError::Ok;
}

RasterDecoderError sink_row(void* user, const uint8_t* data, size_t length,
                            uint32_t row_index) {
    auto* state = static_cast<SinkState*>(user);
    state->events.push_back('W');
    if (data == nullptr || length != state->page.bytes_per_row ||
        row_index != state->page_rows) {
        return RasterDecoderError::BackendError;
    }
    state->row_data.emplace_back(data, data + length);
    state->row_indices.push_back(row_index);
    state->crc = crc32_update(state->crc, data, length);
    ++state->page_rows;
    ++state->rows;
    return RasterDecoderError::Ok;
}

RasterDecoderError sink_end(void* user, RasterDecoderError status) {
    auto* state = static_cast<SinkState*>(user);
    state->events.push_back('E');
    state->end_status = status;
    state->end_statuses.push_back(status);
    return RasterDecoderError::Ok;
}

RasterDecoderError sink_cancel(void* user) {
    auto* state = static_cast<SinkState*>(user);
    state->events.push_back('C');
    state->canceled = true;
    return RasterDecoderError::Ok;
}

RasterSink make_sink(SinkState* state) {
    return RasterSink{state, sink_begin, sink_row, sink_end, sink_cancel};
}

bool read_file(const char* path, std::vector<uint8_t>* output) {
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

std::vector<uint8_t> small_document() {
    return {
        'U', 'N', 'I', 'R', 'A', 'S', 'T', 0,
        0, 0, 0, 1,
        8, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 4, 0, 0, 0, 2, 0, 0, 0, 100,
        0, 0, 0, 0, 0, 0, 0, 0,
        0, 0xFD, 1, 2, 3, 4,
        0, 3, 0xAA,
    };
}

void append_be32(std::vector<uint8_t>* output, uint32_t value) {
    output->push_back(static_cast<uint8_t>(value >> 24));
    output->push_back(static_cast<uint8_t>(value >> 16));
    output->push_back(static_cast<uint8_t>(value >> 8));
    output->push_back(static_cast<uint8_t>(value));
}

std::vector<uint8_t> multi_page_document() {
    std::vector<uint8_t> output{
        'U', 'N', 'I', 'R', 'A', 'S', 'T', 0,
    };
    append_be32(&output, 2);
    const uint8_t pages[][2] = {{1, 2}, {3, 4}};
    for (const auto& row : pages) {
        const uint8_t header[] = {
            8, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0,
            0, 0, 0, 2, 0, 0, 0, 1, 0, 0, 0, 100,
            0, 0, 0, 0, 0, 0, 0, 0,
        };
        output.insert(output.end(), header, header + sizeof(header));
        output.push_back(0);
        output.push_back(0xFF);
        output.push_back(row[0]);
        output.push_back(row[1]);
    }
    return output;
}

size_t make_ipp_body(const std::vector<uint8_t>& document,
                     std::vector<uint8_t>* output) {
    uint8_t buffer[512] = {};
    IppWriter writer(buffer, sizeof(buffer));
    if (writer.begin_request({2, 0}, IppOperation::PrintJob, 55) != IppError::Ok ||
        writer.begin_group(IppGroupTag::OperationAttributes) != IppError::Ok ||
        writer.add_cstr(IppValueTag::Charset, "attributes-charset", "utf-8") !=
            IppError::Ok ||
        writer.add_cstr(IppValueTag::NaturalLanguage,
                        "attributes-natural-language", "en") != IppError::Ok ||
        writer.add_cstr(IppValueTag::Uri, "printer-uri",
                        "ipp://printer.local/ipp/print") != IppError::Ok ||
        writer.add_cstr(IppValueTag::MimeMediaType, "document-format", "image/urf") !=
            IppError::Ok ||
        writer.add_cstr(IppValueTag::NameWithoutLanguage, "job-name", "small") !=
            IppError::Ok ||
        writer.add_cstr(IppValueTag::NameWithoutLanguage,
                        "requesting-user-name", "test") != IppError::Ok ||
        writer.end_attributes() != IppError::Ok) {
        return 0;
    }
    if (writer.append_document(document.data(), document.size()) != IppError::Ok) return 0;
    output->assign(buffer, buffer + writer.length());
    return output->size();
}

bool run_stream(const std::vector<uint8_t>& body, SinkState* sink_state,
                uint32_t expected_pages, uint32_t expected_rows,
                uint64_t expected_decoded_bytes, const char* expected_crc,
                const char* expected_job_name, const char* expected_user_name) {
    AppleRasterPrintBackend raster_backend({}, make_sink(sink_state));
    IppPrintBackend backend = raster_backend.backend();
    IppJobStore jobs;
    IppPrintJobLimits limits;
    limits.printer_uri = "ipp://printer.local/ipp/print";
    limits.document_format = "image/urf";
    limits.max_document_bytes = 2ull * 1024ull * 1024ull;
    IppPrintJobStream stream(&jobs, backend, limits);

    size_t offset = 0;
    while (offset < body.size()) {
        const size_t length = body.size() - offset < 251 ? body.size() - offset : 251;
        IppPrintFeedResult result = stream.feed(body.data() + offset, length, 100 + offset);
        CHECK_TRUE(result.error == IppPrintStreamError::Ok);
        CHECK_TRUE(result.consumed != 0);
        if (result.error != IppPrintStreamError::Ok || result.consumed == 0) return false;
        offset += result.consumed;
    }
    CHECK_TRUE(stream.finish(999) == IppPrintStreamError::Ok);
    CHECK_TRUE(stream.state() == IppPrintStreamState::Completed);
    CHECK_EQ(sink_state->pages, expected_pages);
    CHECK_EQ(sink_state->rows, expected_rows);
    CHECK_EQ_STR(raster_decoder_error_name(sink_state->end_status), "ok");
    CHECK_EQ(stream.job().pages_completed, expected_pages);
    CHECK_TRUE(stream.job().state == IppJobState::Completed);
    CHECK_EQ_STR(stream.job().job_name, expected_job_name);
    CHECK_EQ_STR(stream.job().user_name, expected_user_name);
    CHECK_EQ(raster_backend.document_bytes(),
             static_cast<uint64_t>(stream.document_bytes()));
    CHECK_EQ(raster_backend.decoded_document_bytes(), expected_decoded_bytes);
    CHECK_TRUE(!sink_state->events.empty());
    CHECK_TRUE(sink_state->events.front() == 'B');
    CHECK_TRUE(sink_state->events.back() == 'E');
    if (expected_rows <= 2) {
        CHECK_EQ_STR(std::string(sink_state->events.begin(), sink_state->events.end()).c_str(),
                     expected_pages == 1 ? "BWWE" : "BWEBWE");
    }
    char crc[9] = {};
    std::snprintf(crc, sizeof(crc), "%08x", sink_state->crc);
    CHECK_EQ_STR(crc, expected_crc);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    CHECK_TRUE(argc == 1 || argc >= 3);
    if (argc >= 3) {
        for (int i = 1; i < 3; ++i) {
            std::vector<uint8_t> body;
            CHECK_TRUE(read_file(argv[i], &body));
            SinkState sink;
            CHECK_TRUE(run_stream(body, &sink, 1, 1100, 935000,
                                  i == 1 ? "9b3ac4d5" : "ea4be99e",
                                  i == 1 ? "10 - bitmap-1791683691.png"
                                          : "11 - bitmap-1791683779.png",
                                  "zzpzero"));
        }
    } else {
        std::vector<uint8_t> body;
        CHECK_TRUE(make_ipp_body(small_document(), &body) != 0);
        SinkState sink;
        CHECK_TRUE(run_stream(body, &sink, 1, 2, 8, "d9d125f2", "small", "test"));

        body.clear();
        CHECK_TRUE(make_ipp_body(multi_page_document(), &body) != 0);
        SinkState multi_sink;
        CHECK_TRUE(run_stream(body, &multi_sink, 2, 2, 4, "b63cfbcd", "small", "test"));
        CHECK_TRUE(!multi_sink.pages_seen[0].last_page);
        CHECK_TRUE(multi_sink.pages_seen[1].last_page);
        CHECK_EQ(multi_sink.row_indices[0], 0u);
        CHECK_EQ(multi_sink.row_indices[1], 0u);

        std::vector<uint8_t> malformed = body;
        const std::vector<uint8_t> document = small_document();
        malformed.resize(malformed.size() - multi_page_document().size() + document.size());
        const size_t document_offset = malformed.size() - document.size();
        std::copy(document.begin(), document.end(), malformed.begin() + document_offset);
        malformed[document_offset] = 'X';
        AppleRasterPrintBackend bad_backend({}, make_sink(&multi_sink));
        IppJobStore bad_jobs;
        IppPrintJobLimits bad_limits;
        bad_limits.printer_uri = "ipp://printer.local/ipp/print";
        IppPrintJobStream bad_stream(&bad_jobs, bad_backend.backend(), bad_limits);
        size_t offset = 0;
        while (offset < malformed.size()) {
            const size_t length = std::min<size_t>(251, malformed.size() - offset);
            const IppPrintFeedResult result = bad_stream.feed(
                malformed.data() + offset, length, 200 + offset);
            if (result.error != IppPrintStreamError::Ok) break;
            CHECK_TRUE(result.consumed != 0);
            offset += result.consumed;
        }
        CHECK_TRUE(bad_stream.state() == IppPrintStreamState::Failed);
        CHECK_TRUE(bad_stream.job().state == IppJobState::Aborted);
        CHECK_TRUE(bad_backend.decoder_error() == RasterDecoderError::UnsupportedFormat);

        std::vector<uint8_t> trailing_document = small_document();
        trailing_document.push_back(0xEE);
        std::vector<uint8_t> trailing_body;
        CHECK_TRUE(make_ipp_body(trailing_document, &trailing_body) != 0);
        SinkState trailing_sink;
        AppleRasterPrintBackend trailing_backend({}, make_sink(&trailing_sink));
        IppJobStore trailing_jobs;
        IppPrintJobStream trailing_stream(&trailing_jobs, trailing_backend.backend(), bad_limits);
        size_t trailing_offset = 0;
        IppPrintFeedResult trailing_result;
        while (trailing_offset < trailing_body.size()) {
            trailing_result = trailing_stream.feed(
                trailing_body.data() + trailing_offset,
                trailing_body.size() - trailing_offset, 300 + trailing_offset);
            trailing_offset += trailing_result.consumed;
            if (trailing_result.error != IppPrintStreamError::Ok) break;
            CHECK_TRUE(trailing_result.consumed != 0);
        }
        CHECK_TRUE(trailing_result.error == IppPrintStreamError::BackendError);
        CHECK_TRUE(trailing_stream.state() == IppPrintStreamState::Failed);
        CHECK_TRUE(trailing_stream.job().state == IppJobState::Aborted);
        CHECK_TRUE(trailing_backend.decoder_error() == RasterDecoderError::InvalidState);
    }
    return ::ijpd::test::summary("test_apple_raster_print_job");
}
