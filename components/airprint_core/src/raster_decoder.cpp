// raster_decoder.cpp - Decoder contract diagnostics.
#include "airprint_core/raster_decoder.h"

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

}  // namespace airprint
}  // namespace ijpd
