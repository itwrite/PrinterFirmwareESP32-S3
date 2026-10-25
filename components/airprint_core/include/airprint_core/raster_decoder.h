// ============================================================================
// raster_decoder.h - Document decoder contract, intentionally format-neutral.
//
// The format-specific AppleRasterDecoder is intentionally limited to the
// public CUPS Apple Raster subset verified against real golden samples:
// UNIRAST framing, 8-bit SW/RGB/CMYK pages, 32-byte page headers, and modified
// PackBits rows. Other color spaces and bit depths remain unsupported.
// ============================================================================
#ifndef IJPD_AIRPRINT_CORE_RASTER_DECODER_H
#define IJPD_AIRPRINT_CORE_RASTER_DECODER_H

#include <cstddef>
#include <cstdint>

namespace ijpd {
namespace airprint {

enum class RasterDecoderError : uint8_t {
    Ok = 0,
    InvalidArgument,
    InvalidState,
    NeedMoreData,
    UnsupportedFormat,
    InvalidHeader,
    InvalidDimensions,
    InvalidStride,
    Truncated,
    DataTooLarge,
    BackendError,
    Canceled,
};

const char* raster_decoder_error_name(RasterDecoderError error) noexcept;

enum class RasterPixelFormat : uint8_t {
    Mono1 = 0,
    Mono8,
    RGB888,
    CMYK8888,
    Unknown,
};

struct RasterPageDescriptor {
    uint32_t width_px = 0;
    uint32_t height_px = 0;
    uint32_t bytes_per_row = 0;
    uint16_t dpi_x = 0;
    uint16_t dpi_y = 0;
    // Mono8 is one raw byte per pixel. The decoder does not apply thresholding,
    // polarity conversion, orientation, or hardware-specific color handling.
    RasterPixelFormat pixel_format = RasterPixelFormat::Unknown;
    bool last_page = true;
};

struct RasterDecoderLimits {
    uint32_t max_width_px = 0;
    uint32_t max_height_px = 0;
    uint32_t max_bytes_per_row = 0;
    uint64_t max_document_bytes = 0;
    // Separate from compressed input bytes: bounds the total bytes delivered
    // through write_row, including repeated rows.
    uint64_t max_decoded_bytes = 0;
};

struct RasterSink {
    void* user = nullptr;
    RasterDecoderError (*begin_page)(void* user,
                                     const RasterPageDescriptor& page) = nullptr;
    // write_row is synchronous: data points to decoder-owned scratch storage
    // and is valid only for the duration of this callback. The sink must copy
    // it before returning if it needs the row later.
    RasterDecoderError (*write_row)(void* user, const uint8_t* data,
                                    size_t length, uint32_t row_index) = nullptr;
    // A started page receives exactly one end_page call. A sink failure or
    // decoder failure is reported there on a best-effort basis. If begin_page
    // fails, end_page is not called for that page.
    RasterDecoderError (*end_page)(void* user, RasterDecoderError status) = nullptr;
    RasterDecoderError (*cancel)(void* user) = nullptr;
};

class RasterDecoder {
public:
    virtual ~RasterDecoder() = default;

    virtual RasterDecoderError begin(const RasterDecoderLimits& limits,
                                     const RasterSink& sink) noexcept = 0;
    // Consumed bytes are valid even when the decoder returns NeedMoreData.
    virtual RasterDecoderError feed(const uint8_t* data, size_t length,
                                    size_t* consumed) noexcept = 0;
    virtual RasterDecoderError finish() noexcept = 0;
    virtual RasterDecoderError cancel() noexcept = 0;
    virtual bool page_started() const noexcept = 0;
    // Compressed bytes accepted from the document stream.
    virtual uint64_t document_bytes() const noexcept = 0;
};

inline constexpr size_t kAppleRasterMaxRowBytes = 64u * 1024u;
inline constexpr uint32_t kAppleRasterMaxPages = 1024;

class AppleRasterDecoder final : public RasterDecoder {
public:
    AppleRasterDecoder() noexcept = default;

    RasterDecoderError begin(const RasterDecoderLimits& limits,
                             const RasterSink& sink) noexcept override;
    RasterDecoderError feed(const uint8_t* data, size_t length,
                            size_t* consumed) noexcept override;
    RasterDecoderError finish() noexcept override;
    RasterDecoderError cancel() noexcept override;
    bool page_started() const noexcept override { return page_started_; }
    uint64_t document_bytes() const noexcept override { return document_bytes_; }
    // Expanded bytes delivered through RasterSink::write_row.
    uint64_t decoded_bytes() const noexcept { return decoded_bytes_; }
    uint32_t pages_completed() const noexcept { return page_index_; }

private:
    enum class State : uint8_t {
        Idle = 0,
        FileHeader,
        PageHeader,
        RowRepeat,
        RowCommand,
        LiteralData,
        RepeatPixel,
        Complete,
        Failed,
        Canceled,
    };

    RasterDecoderError consume_byte_(uint8_t value) noexcept;
    RasterDecoderError parse_file_header_() noexcept;
    RasterDecoderError parse_page_header_() noexcept;
    RasterDecoderError emit_row_group_() noexcept;
    RasterDecoderError finish_page_() noexcept;
    RasterDecoderError fail_(RasterDecoderError error) noexcept;
    RasterDecoderError end_page_(RasterDecoderError status) noexcept;

    RasterDecoderLimits limits_{};
    RasterSink sink_{};
    State state_ = State::Idle;
    RasterDecoderError terminal_error_ = RasterDecoderError::Ok;
    uint8_t file_header_[12] = {};
    size_t file_header_length_ = 0;
    uint8_t page_header_[32] = {};
    size_t page_header_length_ = 0;
    uint8_t row_[kAppleRasterMaxRowBytes] = {};
    RasterPageDescriptor page_{};
    uint32_t page_count_ = 0;
    uint32_t page_index_ = 0;
    uint32_t row_index_ = 0;
    uint32_t row_repeat_remaining_ = 0;
    uint32_t literal_remaining_ = 0;
    uint32_t repeat_pixel_count_ = 0;
    uint8_t bytes_per_pixel_ = 1;
    uint8_t repeat_pixel_[4] = {};
    uint8_t repeat_pixel_length_ = 0;
    size_t row_length_ = 0;
    uint64_t document_bytes_ = 0;
    uint64_t decoded_bytes_ = 0;
    bool configured_ = false;
    bool page_started_ = false;
};

}  // namespace airprint
}  // namespace ijpd

#endif  // IJPD_AIRPRINT_CORE_RASTER_DECODER_H
