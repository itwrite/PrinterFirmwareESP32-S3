// test_pwg_raster.cpp - PWG Raster v2 header bounds and layout validation.
#include "core/pwg_raster.h"
#include "assert.h"

#include <cstring>
#include <vector>

using namespace ijpd;

namespace {
void be32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v);
}

std::vector<uint8_t> header(uint32_t width, uint32_t height, uint32_t bpl,
                            uint32_t bpp = 8, uint32_t compression = 0) {
    std::vector<uint8_t> data(PWG_RASTER_HEADER_BYTES, 0);
    std::memcpy(data.data(), "RaS2", 4);
    uint8_t* h = data.data() + 4;
    be32(h + 276, 300);
    be32(h + 280, 300);
    be32(h + 372, width);
    be32(h + 376, height);
    be32(h + 384, bpp == 1 ? 1 : 8);
    be32(h + 388, bpp);
    be32(h + 392, bpl);
    be32(h + 396, 0);
    be32(h + 400, bpp == 1 ? 3 : 1);  // K for mono1, RGB otherwise
    be32(h + 404, compression);
    return data;
}
}  // namespace

int main() {
    PwgRasterPage page{};
    auto valid = header(384, 10, 48, 1);
    CHECK_TRUE(parse_pwg_raster_header(valid.data(), valid.size(), &page) == Err::OK);
    CHECK_EQ(page.width, 384u);
    CHECK_EQ(page.height, 10u);
    CHECK_EQ(page.bytes_per_line, 48u);
    CHECK_EQ(page.total_bytes, 480u);

    auto too_short = valid;
    too_short.resize(PWG_RASTER_HEADER_BYTES - 1);
    CHECK_TRUE(parse_pwg_raster_header(too_short.data(), too_short.size(), &page) ==
               Err::INVALID_ARGUMENT);

    auto bad_width = header(384, 10, 47, 1);
    CHECK_TRUE(parse_pwg_raster_header(bad_width.data(), bad_width.size(), &page) ==
               Err::INVALID_ARGUMENT);

    auto compressed = header(384, 10, 48, 1, 1);
    CHECK_TRUE(parse_pwg_raster_header(compressed.data(), compressed.size(), &page) ==
               Err::UNSUPPORTED);

    auto bad_sync = valid;
    bad_sync[0] = 'X';
    CHECK_TRUE(parse_pwg_raster_header(bad_sync.data(), bad_sync.size(), &page) ==
               Err::INVALID_ARGUMENT);
    return ::ijpd::test::summary("test_pwg_raster");
}
