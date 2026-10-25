// ============================================================================
// ipp_codec.h - Bounded IPP message reader and writer.
// ============================================================================
#ifndef IJPD_AIRPRINT_CORE_IPP_CODEC_H
#define IJPD_AIRPRINT_CORE_IPP_CODEC_H

#include <cstddef>
#include <cstdint>

#include "airprint_core/ipp_types.h"

namespace ijpd {
namespace airprint {

class IppReader {
public:
    IppReader() noexcept = default;

    IppError begin(const uint8_t* data, size_t length) noexcept;
    IppReadResult next() noexcept;

    const IppMessageHeader& header() const noexcept { return header_; }
    bool started() const noexcept { return started_; }
    bool ended() const noexcept { return ended_; }
    IppGroupTag current_group() const noexcept { return current_group_; }

    const uint8_t* document_data() const noexcept;
    size_t document_length() const noexcept;

private:
    IppMessageHeader header_{};
    const uint8_t* data_ = nullptr;
    size_t length_ = 0;
    size_t offset_ = 0;
    size_t document_offset_ = 0;
    const uint8_t* last_name_ = nullptr;
    uint16_t last_name_length_ = 0;
    IppGroupTag current_group_ = IppGroupTag::None;
    uint8_t collection_depth_ = 0;
    bool started_ = false;
    bool ended_ = false;
};

class IppWriter {
public:
    IppWriter(uint8_t* data, size_t capacity) noexcept
        : data_(data), capacity_(capacity) {}

    IppError begin(IppVersion version, uint16_t code, uint32_t request_id) noexcept;
    IppError begin_request(IppVersion version, IppOperation operation,
                           uint32_t request_id) noexcept;
    IppError begin_response(IppVersion version, IppStatus status,
                            uint32_t request_id) noexcept;

    IppError begin_group(IppGroupTag group) noexcept;
    IppError add_raw(IppValueTag tag, const char* name, size_t name_length,
                     const uint8_t* value, size_t value_length) noexcept;
    IppError add_continuation(IppValueTag tag, const uint8_t* value,
                              size_t value_length) noexcept;
    IppError add_integer(const char* name, int32_t value) noexcept;
    IppError add_enum(const char* name, int32_t value) noexcept;
    IppError add_boolean(const char* name, bool value) noexcept;
    IppError add_string(IppValueTag tag, const char* name, const char* value,
                        size_t value_length) noexcept;
    IppError add_cstr(IppValueTag tag, const char* name, const char* value) noexcept;
    IppError add_resolution(const char* name, int32_t x, int32_t y,
                            uint8_t unit) noexcept;
    IppError begin_collection(const char* name) noexcept;
    IppError add_collection_member_name(const char* name) noexcept;
    IppError end_collection() noexcept;
    IppError end_attributes() noexcept;
    IppError append_document(const uint8_t* data, size_t length) noexcept;

    const uint8_t* data() const noexcept { return data_; }
    size_t length() const noexcept { return length_; }
    size_t capacity() const noexcept { return capacity_; }
    bool started() const noexcept { return started_; }
    bool ended() const noexcept { return ended_; }

private:
    IppError append_u8_(uint8_t value) noexcept;
    IppError append_be16_(uint16_t value) noexcept;
    IppError append_be32_(uint32_t value) noexcept;
    IppError append_(const uint8_t* data, size_t length) noexcept;
    IppError validate_attribute_state_() const noexcept;

    uint8_t* data_ = nullptr;
    size_t capacity_ = 0;
    size_t length_ = 0;
    IppGroupTag current_group_ = IppGroupTag::None;
    const char* last_name_ = nullptr;
    uint16_t last_name_length_ = 0;
    bool started_ = false;
    bool ended_ = false;
};

}  // namespace airprint
}  // namespace ijpd

#endif  // IJPD_AIRPRINT_CORE_IPP_CODEC_H
