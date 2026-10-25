// test_airprint_sample.cpp - Decoder contract and golden sample manifest tests.
#include "assert.h"

#include <cstring>

#include "airprint_core/airprint_sample.h"
#include "airprint_core/raster_decoder.h"

using namespace ijpd::airprint;

namespace {

AirPrintSampleManifest valid_manifest() {
    AirPrintSampleManifest manifest;
    std::strcpy(manifest.sample_name, "mono-a4-001");
    std::strcpy(manifest.document_format, "image/urf");
    std::strcpy(manifest.sha256,
                "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
    manifest.ipp_body_bytes = 4096;
    manifest.document_offset = 128;
    manifest.document_bytes = 3968;
    return manifest;
}

}  // namespace

int main() {
    const AirPrintSampleManifest valid = valid_manifest();
    CHECK_TRUE(validate_airprint_sample(valid) == AirPrintSampleError::Ok);
    CHECK_EQ_STR(raster_decoder_error_name(RasterDecoderError::NeedMoreData),
                 "need_more_data");

    AirPrintSampleManifest out_of_range = valid;
    out_of_range.document_offset = 4090;
    CHECK_TRUE(validate_airprint_sample(out_of_range) == AirPrintSampleError::BodyRangeInvalid);

    AirPrintSampleManifest bad_format = valid;
    std::strcpy(bad_format.document_format, "application/pdf");
    CHECK_TRUE(validate_airprint_sample(bad_format) == AirPrintSampleError::UnsupportedFormat);

    AirPrintSampleManifest bad_hash = valid;
    bad_hash.sha256[0] = 'z';
    CHECK_TRUE(validate_airprint_sample(bad_hash) == AirPrintSampleError::Sha256Invalid);

    AirPrintSampleManifest known = valid;
    known.expected_values_known = true;
    CHECK_TRUE(validate_airprint_sample(known) == AirPrintSampleError::InvalidManifest);
    known.expected_width_px = 384;
    known.expected_height_px = 100;
    known.expected_dpi_x = 203;
    known.expected_dpi_y = 203;
    CHECK_TRUE(validate_airprint_sample(known) == AirPrintSampleError::Ok);

    AirPrintSampleManifest empty = valid;
    empty.sample_name[0] = '\0';
    CHECK_TRUE(validate_airprint_sample(empty) == AirPrintSampleError::InvalidManifest);

    AirPrintSampleManifest empty_document = valid;
    empty_document.document_bytes = 0;
    CHECK_TRUE(validate_airprint_sample(empty_document) == AirPrintSampleError::InvalidManifest);

    AirPrintSampleManifest body_zero = valid;
    body_zero.ipp_body_bytes = 0;
    CHECK_TRUE(validate_airprint_sample(body_zero) == AirPrintSampleError::InvalidManifest);

    AirPrintSampleManifest hash_short = valid;
    hash_short.sha256[63] = '\0';
    CHECK_TRUE(validate_airprint_sample(hash_short) == AirPrintSampleError::Sha256Invalid);

    AirPrintSampleManifest hash_upper = valid;
    hash_upper.sha256[0] = 'A';
    CHECK_TRUE(validate_airprint_sample(hash_upper) == AirPrintSampleError::Ok);

    return ::ijpd::test::summary("test_airprint_sample");
}
