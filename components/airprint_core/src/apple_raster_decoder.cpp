// apple_raster_decoder.cpp - Verified public CUPS Apple Raster subset.
#include "airprint_core/raster_decoder.h"

#include <cstring>

namespace ijpd {
namespace airprint {
namespace {

constexpr uint8_t kAppleColorSpaceSw = 0;
constexpr uint8_t kAppleColorSpaceSrgb = 1;
constexpr uint8_t kAppleColorSpaceAdobeRgb = 3;
constexpr uint8_t kAppleColorSpaceRgb = 5;
constexpr uint8_t kAppleColorSpaceCmyk = 6;
constexpr uint8_t kAppleBitsPerPixelMono8 = 8;
constexpr uint8_t kAppleBitsPerPixelRgb8 = 24;
constexpr uint8_t kAppleBitsPerPixelCmyk8 = 32;
constexpr size_t kAppleFileHeaderBytes = 12;
constexpr size_t kApplePageHeaderBytes = 32;
constexpr uint64_t kDefaultMaxDocumentBytes = 64ull * 1024ull * 1024ull;
constexpr uint64_t kDefaultMaxDecodedBytes = 256ull * 1024ull * 1024ull;

uint32_t read_be32(const uint8_t* data) noexcept {
    return (static_cast<uint32_t>(data[0]) << 24) |
           (static_cast<uint32_t>(data[1]) << 16) |
           (static_cast<uint32_t>(data[2]) << 8) |
           static_cast<uint32_t>(data[3]);
}

}  // namespace

RasterDecoderError AppleRasterDecoder::begin(const RasterDecoderLimits& limits,
                                              const RasterSink& sink) noexcept {
    if (sink.begin_page == nullptr || sink.write_row == nullptr ||
        sink.end_page == nullptr || sink.cancel == nullptr) {
        return RasterDecoderError::InvalidArgument;
    }
    if (limits.max_bytes_per_row > kAppleRasterMaxRowBytes) {
        return RasterDecoderError::InvalidStride;
    }
    limits_ = limits;
    if (limits_.max_width_px == 0) limits_.max_width_px = kAppleRasterMaxRowBytes;
    if (limits_.max_height_px == 0) limits_.max_height_px = 0x00FFFFFFu;
    if (limits_.max_bytes_per_row == 0) limits_.max_bytes_per_row = kAppleRasterMaxRowBytes;
    if (limits_.max_document_bytes == 0) limits_.max_document_bytes = kDefaultMaxDocumentBytes;
    if (limits_.max_decoded_bytes == 0) limits_.max_decoded_bytes = kDefaultMaxDecodedBytes;
    sink_ = sink;
    state_ = State::FileHeader;
    terminal_error_ = RasterDecoderError::Ok;
    file_header_length_ = 0;
    page_header_length_ = 0;
    page_count_ = 0;
    page_index_ = 0;
    row_index_ = 0;
    row_repeat_remaining_ = 0;
    literal_remaining_ = 0;
    repeat_pixel_count_ = 0;
    row_length_ = 0;
    document_bytes_ = 0;
    decoded_bytes_ = 0;
    configured_ = true;
    page_started_ = false;
    return RasterDecoderError::Ok;
}

RasterDecoderError AppleRasterDecoder::parse_file_header_() noexcept {
    static constexpr uint8_t kMagic[] = {'U', 'N', 'I', 'R', 'A', 'S', 'T', '\0'};
    if (std::memcmp(file_header_, kMagic, sizeof(kMagic)) != 0) {
        return RasterDecoderError::UnsupportedFormat;
    }
    page_count_ = read_be32(file_header_ + 8);
    if (page_count_ == 0 || page_count_ > kAppleRasterMaxPages) {
        return RasterDecoderError::InvalidDimensions;
    }
    state_ = State::PageHeader;
    page_header_length_ = 0;
    return RasterDecoderError::Ok;
}

RasterDecoderError AppleRasterDecoder::parse_page_header_() noexcept {
    const uint8_t bits_per_pixel = page_header_[0];
    const uint8_t color_space = page_header_[1];
    const bool mono8 = bits_per_pixel == kAppleBitsPerPixelMono8 &&
                       color_space == kAppleColorSpaceSw;
    const bool rgb8 = bits_per_pixel == kAppleBitsPerPixelRgb8 &&
                      (color_space == kAppleColorSpaceSrgb ||
                       color_space == kAppleColorSpaceAdobeRgb ||
                       color_space == kAppleColorSpaceRgb);
    const bool cmyk8 = bits_per_pixel == kAppleBitsPerPixelCmyk8 &&
                       color_space == kAppleColorSpaceCmyk;
    if (!mono8 && !rgb8 && !cmyk8) {
        return RasterDecoderError::UnsupportedFormat;
    }
    const uint32_t width = read_be32(page_header_ + 12);
    const uint32_t height = read_be32(page_header_ + 16);
    const uint32_t dpi = read_be32(page_header_ + 20);
    if (width == 0 || height == 0 || dpi == 0 || dpi > 0xFFFFu) {
        return RasterDecoderError::InvalidDimensions;
    }
    if (width > limits_.max_width_px || height > limits_.max_height_px) {
        return RasterDecoderError::InvalidDimensions;
    }
    const uint32_t bytes_per_pixel = cmyk8 ? 4u : rgb8 ? 3u : 1u;
    if (width > UINT32_MAX / bytes_per_pixel) return RasterDecoderError::InvalidStride;
    const uint32_t bytes_per_row = width * bytes_per_pixel;
    if (bytes_per_row > limits_.max_bytes_per_row ||
        bytes_per_row > kAppleRasterMaxRowBytes) {
        return RasterDecoderError::InvalidStride;
    }
    const uint64_t page_decoded_bytes = static_cast<uint64_t>(bytes_per_row) * height;
    if (decoded_bytes_ > limits_.max_decoded_bytes ||
        page_decoded_bytes > limits_.max_decoded_bytes - decoded_bytes_) {
        return RasterDecoderError::DataTooLarge;
    }

    page_ = RasterPageDescriptor{};
    page_.width_px = width;
    page_.height_px = height;
    page_.bytes_per_row = bytes_per_row;
    page_.dpi_x = static_cast<uint16_t>(dpi);
    page_.dpi_y = static_cast<uint16_t>(dpi);
    page_.pixel_format = cmyk8 ? RasterPixelFormat::CMYK8888
                               : rgb8 ? RasterPixelFormat::RGB888
                                      : RasterPixelFormat::Mono8;
    bytes_per_pixel_ = static_cast<uint8_t>(bytes_per_pixel);
    page_.last_page = page_index_ + 1 == page_count_;
    row_index_ = 0;
    row_length_ = 0;
    row_repeat_remaining_ = 0;
    literal_remaining_ = 0;
    repeat_pixel_count_ = 0;
    repeat_pixel_length_ = 0;
    const RasterDecoderError begin_error = sink_.begin_page(sink_.user, page_);
    if (begin_error != RasterDecoderError::Ok) return RasterDecoderError::BackendError;
    page_started_ = true;
    state_ = State::RowRepeat;
    return RasterDecoderError::Ok;
}

RasterDecoderError AppleRasterDecoder::end_page_(RasterDecoderError status) noexcept {
    if (!page_started_) return status;
    page_started_ = false;
    const RasterDecoderError sink_error = sink_.end_page(sink_.user, status);
    if (sink_error != RasterDecoderError::Ok && status == RasterDecoderError::Ok) {
        return RasterDecoderError::BackendError;
    }
    return status;
}

RasterDecoderError AppleRasterDecoder::finish_page_() noexcept {
    const RasterDecoderError end_error = end_page_(RasterDecoderError::Ok);
    if (end_error != RasterDecoderError::Ok) return end_error;
    ++page_index_;
    if (page_index_ == page_count_) {
        state_ = State::Complete;
    } else {
        page_header_length_ = 0;
        state_ = State::PageHeader;
    }
    return RasterDecoderError::Ok;
}

RasterDecoderError AppleRasterDecoder::emit_row_group_() noexcept {
    if (row_length_ != page_.bytes_per_row || row_repeat_remaining_ == 0) {
        return RasterDecoderError::InvalidStride;
    }
    while (row_repeat_remaining_ != 0) {
        if (decoded_bytes_ > limits_.max_decoded_bytes ||
            row_length_ > limits_.max_decoded_bytes - decoded_bytes_) {
            return RasterDecoderError::DataTooLarge;
        }
        const RasterDecoderError error = sink_.write_row(
            sink_.user, row_, row_length_, row_index_);
        if (error != RasterDecoderError::Ok) {
            return error == RasterDecoderError::Canceled
                       ? RasterDecoderError::Canceled
                       : RasterDecoderError::BackendError;
        }
        ++row_index_;
        --row_repeat_remaining_;
        decoded_bytes_ += row_length_;
    }
    row_length_ = 0;
    if (row_index_ == page_.height_px) return finish_page_();
    state_ = State::RowRepeat;
    return RasterDecoderError::Ok;
}

RasterDecoderError AppleRasterDecoder::consume_byte_(uint8_t value) noexcept {
    switch (state_) {
        case State::FileHeader:
            file_header_[file_header_length_++] = value;
            if (file_header_length_ == kAppleFileHeaderBytes) return parse_file_header_();
            return RasterDecoderError::Ok;
        case State::PageHeader:
            page_header_[page_header_length_++] = value;
            if (page_header_length_ == kApplePageHeaderBytes) return parse_page_header_();
            return RasterDecoderError::Ok;
        case State::RowRepeat:
            row_repeat_remaining_ = static_cast<uint32_t>(value) + 1;
            if (row_repeat_remaining_ > page_.height_px - row_index_) {
                return RasterDecoderError::InvalidDimensions;
            }
            row_length_ = 0;
            state_ = State::RowCommand;
            return RasterDecoderError::Ok;
        case State::RowCommand: {
            if (value == 0x80) {
                const uint8_t clear_value = page_.pixel_format == RasterPixelFormat::CMYK8888
                                                ? 0x00
                                                : 0xFF;
                std::memset(row_ + row_length_, clear_value,
                            page_.bytes_per_row - row_length_);
                row_length_ = page_.bytes_per_row;
                return emit_row_group_();
            }
            if (value & 0x80) {
                const uint32_t literal_pixels = 257u - static_cast<uint32_t>(value);
                if (literal_pixels > UINT32_MAX / bytes_per_pixel_) {
                    return RasterDecoderError::InvalidStride;
                }
                literal_remaining_ = literal_pixels * bytes_per_pixel_;
                if (literal_remaining_ > page_.bytes_per_row - row_length_) {
                    return RasterDecoderError::InvalidStride;
                }
                state_ = State::LiteralData;
                return RasterDecoderError::Ok;
            }
            const uint32_t repeat_pixels = static_cast<uint32_t>(value) + 1u;
            if (repeat_pixels > UINT32_MAX / bytes_per_pixel_) {
                return RasterDecoderError::InvalidStride;
            }
            repeat_pixel_count_ = repeat_pixels * bytes_per_pixel_;
            if (repeat_pixel_count_ > page_.bytes_per_row - row_length_) {
                return RasterDecoderError::InvalidStride;
            }
            repeat_pixel_length_ = 0;
            state_ = State::RepeatPixel;
            return RasterDecoderError::Ok;
        }
        case State::LiteralData:
            row_[row_length_++] = value;
            if (--literal_remaining_ == 0) {
                if (row_length_ == page_.bytes_per_row) return emit_row_group_();
                state_ = State::RowCommand;
            }
            return RasterDecoderError::Ok;
        case State::RepeatPixel:
            repeat_pixel_[repeat_pixel_length_++] = value;
            if (repeat_pixel_length_ < bytes_per_pixel_) return RasterDecoderError::Ok;
            for (uint32_t offset = 0; offset < repeat_pixel_count_; ++offset) {
                row_[row_length_ + offset] = repeat_pixel_[offset % bytes_per_pixel_];
            }
            row_length_ += repeat_pixel_count_;
            repeat_pixel_count_ = 0;
            repeat_pixel_length_ = 0;
            if (row_length_ == page_.bytes_per_row) return emit_row_group_();
            state_ = State::RowCommand;
            return RasterDecoderError::Ok;
        case State::Complete:
            return RasterDecoderError::InvalidState;
        case State::Failed:
            return terminal_error_;
        case State::Canceled:
            return RasterDecoderError::Canceled;
        case State::Idle:
            return RasterDecoderError::InvalidState;
    }
    return RasterDecoderError::InvalidState;
}

RasterDecoderError AppleRasterDecoder::fail_(RasterDecoderError error) noexcept {
    if (state_ == State::Failed || state_ == State::Canceled) return terminal_error_;
    terminal_error_ = error;
    if (page_started_) (void)end_page_(error);
    state_ = error == RasterDecoderError::Canceled ? State::Canceled : State::Failed;
    return error;
}

RasterDecoderError AppleRasterDecoder::feed(const uint8_t* data, size_t length,
                                            size_t* consumed) noexcept {
    if (consumed == nullptr) return RasterDecoderError::InvalidArgument;
    *consumed = 0;
    if (!configured_) return RasterDecoderError::InvalidState;
    if (data == nullptr && length != 0) return RasterDecoderError::InvalidArgument;
    if (state_ == State::Complete) return length == 0 ? RasterDecoderError::Ok
                                                      : RasterDecoderError::InvalidState;
    if (state_ == State::Failed || state_ == State::Canceled) return terminal_error_;

    while (*consumed < length) {
        if (document_bytes_ >= limits_.max_document_bytes) {
            return fail_(RasterDecoderError::DataTooLarge);
        }
        const RasterDecoderError error = consume_byte_(data[*consumed]);
        ++*consumed;
        ++document_bytes_;
        if (error != RasterDecoderError::Ok) return fail_(error);
        if (state_ == State::Complete) return RasterDecoderError::Ok;
    }
    return RasterDecoderError::NeedMoreData;
}

RasterDecoderError AppleRasterDecoder::finish() noexcept {
    if (!configured_) return RasterDecoderError::InvalidState;
    if (state_ == State::Complete) return RasterDecoderError::Ok;
    if (state_ == State::Failed || state_ == State::Canceled) return terminal_error_;
    return fail_(RasterDecoderError::Truncated);
}

RasterDecoderError AppleRasterDecoder::cancel() noexcept {
    if (!configured_) return RasterDecoderError::InvalidState;
    if (state_ == State::Failed) return terminal_error_;
    if (state_ == State::Canceled) return RasterDecoderError::Canceled;
    if (state_ == State::Complete) return RasterDecoderError::InvalidState;
    const RasterDecoderError cancel_error = sink_.cancel(sink_.user);
    if (cancel_error != RasterDecoderError::Ok) return fail_(RasterDecoderError::BackendError);
    terminal_error_ = RasterDecoderError::Canceled;
    if (page_started_) (void)end_page_(RasterDecoderError::Canceled);
    state_ = State::Canceled;
    return RasterDecoderError::Canceled;
}

}  // namespace airprint
}  // namespace ijpd
