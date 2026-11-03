// raster_decoder.cpp - Decoder contract diagnostics.
#include "airprint_core/raster_decoder.h"

#include <cstring>

namespace ijpd {
namespace airprint {

const char* raster_decoder_error_name(RasterDecoderError error) noexcept {
    switch (error) {
        case RasterDecoderError::Ok: return "ok";
        case RasterDecoderError::InvalidArgument: return "invalid_argument";
        case RasterDecoderError::InvalidState: return "invalid_state";
        case RasterDecoderError::NeedMoreData: return "need_more_data";
        case RasterDecoderError::UnsupportedFormat: return "unsupported_format";
        case RasterDecoderError::InvalidHeader: return "invalid_header";
        case RasterDecoderError::InvalidDimensions: return "invalid_dimensions";
        case RasterDecoderError::InvalidStride: return "invalid_stride";
        case RasterDecoderError::Truncated: return "truncated";
        case RasterDecoderError::DataTooLarge: return "data_too_large";
        case RasterDecoderError::BackendError: return "backend_error";
        case RasterDecoderError::Canceled: return "canceled";
    }
    return "unknown";
}

RasterDecoderError convert_apple_raster_row_to_mock(
    const RasterPageDescriptor& page, const uint8_t* source, size_t source_length,
    uint32_t preview_width, uint8_t* cmyk_row, size_t cmyk_capacity,
    uint32_t black_width,
    uint8_t* black_row, size_t black_capacity) noexcept {
    if (source == nullptr || preview_width == 0 || black_width == 0 || cmyk_row == nullptr ||
        black_row == nullptr) {
        return RasterDecoderError::InvalidArgument;
    }
    const uint32_t bytes_per_pixel = page.pixel_format == RasterPixelFormat::CMYK8888
        ? 4u : page.pixel_format == RasterPixelFormat::RGB888 ? 3u
        : page.pixel_format == RasterPixelFormat::Mono8 ? 1u : 0u;
    if (bytes_per_pixel == 0 || page.width_px == 0 ||
        page.width_px > UINT32_MAX / bytes_per_pixel ||
        page.bytes_per_row != page.width_px * bytes_per_pixel ||
        source_length < page.bytes_per_row) {
        return RasterDecoderError::InvalidStride;
    }
    if (preview_width > cmyk_capacity / 4u || black_width > black_capacity) {
        return RasterDecoderError::InvalidStride;
    }

    const bool integral_scale = black_width >= preview_width &&
                                black_width % preview_width == 0;
    if (integral_scale) {
        const uint32_t preview_step = black_width / preview_width;
        for (uint32_t pixel = 0; pixel < black_width; ++pixel) {
            const uint32_t source_pixel = static_cast<uint32_t>(
                static_cast<uint64_t>(pixel) * page.width_px / black_width);
            const uint8_t* const input = source +
                static_cast<size_t>(source_pixel) * bytes_per_pixel;
            uint8_t cyan = 0;
            uint8_t magenta = 0;
            uint8_t yellow = 0;
            uint8_t black = 0;

            if (page.pixel_format == RasterPixelFormat::CMYK8888) {
                cyan = input[0];
                magenta = input[1];
                yellow = input[2];
                black = input[3];
                const uint32_t red_ink = cyan + black > 255u ? 255u : cyan + black;
                const uint32_t green_ink = magenta + black > 255u ? 255u : magenta + black;
                const uint32_t blue_ink = yellow + black > 255u ? 255u : yellow + black;
                const uint8_t gray = static_cast<uint8_t>(
                    (77u * red_ink + 150u * green_ink + 29u * blue_ink) >> 8);
                black_row[pixel] = static_cast<uint8_t>(255u - gray);
            } else if (page.pixel_format == RasterPixelFormat::RGB888) {
                const uint32_t c = 255u - input[0];
                const uint32_t m = 255u - input[1];
                const uint32_t y = 255u - input[2];
                const uint32_t k = c < m ? (c < y ? c : y) : (m < y ? m : y);
                cyan = static_cast<uint8_t>(c - k);
                magenta = static_cast<uint8_t>(m - k);
                yellow = static_cast<uint8_t>(y - k);
                black = static_cast<uint8_t>(k);
                const uint8_t gray = static_cast<uint8_t>(
                    (77u * input[0] + 150u * input[1] + 29u * input[2]) >> 8);
                black_row[pixel] = static_cast<uint8_t>(255u - gray);
            } else {
                black = static_cast<uint8_t>(255u - input[0]);
                black_row[pixel] = black;
            }

            if ((pixel % preview_step) == 0) {
                const size_t output_offset =
                    static_cast<size_t>(pixel / preview_step) * 4u;
                cmyk_row[output_offset] = cyan;
                cmyk_row[output_offset + 1u] = magenta;
                cmyk_row[output_offset + 2u] = yellow;
                cmyk_row[output_offset + 3u] = black;
            }
        }
        return RasterDecoderError::Ok;
    }

    for (uint32_t pixel = 0; pixel < preview_width; ++pixel) {
        const uint32_t source_pixel = static_cast<uint32_t>(
            static_cast<uint64_t>(pixel) * page.width_px / preview_width);
        const uint8_t* const input = source +
            static_cast<size_t>(source_pixel) * bytes_per_pixel;
        uint8_t cyan = 0;
        uint8_t magenta = 0;
        uint8_t yellow = 0;
        uint8_t black = 0;

        if (page.pixel_format == RasterPixelFormat::CMYK8888) {
            cyan = input[0];
            magenta = input[1];
            yellow = input[2];
            black = input[3];
        } else if (page.pixel_format == RasterPixelFormat::RGB888) {
            const uint32_t c = 255u - input[0];
            const uint32_t m = 255u - input[1];
            const uint32_t y = 255u - input[2];
            const uint32_t k = c < m ? (c < y ? c : y) : (m < y ? m : y);
            cyan = static_cast<uint8_t>(c - k);
            magenta = static_cast<uint8_t>(m - k);
            yellow = static_cast<uint8_t>(y - k);
            black = static_cast<uint8_t>(k);
        } else {
            black = static_cast<uint8_t>(255u - input[0]);
        }

        const size_t output_offset = static_cast<size_t>(pixel) * 4u;
        cmyk_row[output_offset] = cyan;
        cmyk_row[output_offset + 1u] = magenta;
        cmyk_row[output_offset + 2u] = yellow;
        cmyk_row[output_offset + 3u] = black;
    }

    for (uint32_t pixel = 0; pixel < black_width; ++pixel) {
        const uint32_t source_pixel = static_cast<uint32_t>(
            static_cast<uint64_t>(pixel) * page.width_px / black_width);
        const uint8_t* const input = source +
            static_cast<size_t>(source_pixel) * bytes_per_pixel;
        if (page.pixel_format == RasterPixelFormat::CMYK8888) {
            const uint32_t cyan = input[0];
            const uint32_t magenta = input[1];
            const uint32_t yellow = input[2];
            const uint32_t black = input[3];
            const uint32_t red_ink = cyan + black > 255u ? 255u : cyan + black;
            const uint32_t green_ink = magenta + black > 255u ? 255u : magenta + black;
            const uint32_t blue_ink = yellow + black > 255u ? 255u : yellow + black;
            const uint8_t gray = static_cast<uint8_t>(
                (77u * red_ink + 150u * green_ink + 29u * blue_ink) >> 8);
            black_row[pixel] = static_cast<uint8_t>(255u - gray);
        } else if (page.pixel_format == RasterPixelFormat::RGB888) {
            const uint8_t gray = static_cast<uint8_t>(
                (77u * input[0] + 150u * input[1] + 29u * input[2]) >> 8);
            black_row[pixel] = static_cast<uint8_t>(255u - gray);
        } else {
            black_row[pixel] = static_cast<uint8_t>(255u - input[0]);
        }
    }
    return RasterDecoderError::Ok;
}

}  // namespace airprint
}  // namespace ijpd
