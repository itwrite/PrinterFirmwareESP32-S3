// ============================================================================
// ipp_request.h - Bounded IPP request model and operation validation.
//
// This layer consumes IppReader views without allocating or copying attribute
// bytes. It validates the first AirPrint operation slice before HTTP/job code.
// ============================================================================
#ifndef IJPD_AIRPRINT_CORE_IPP_REQUEST_H
#define IJPD_AIRPRINT_CORE_IPP_REQUEST_H

#include <cstddef>
#include <cstdint>

#include "airprint_core/ipp_codec.h"

namespace ijpd {
namespace airprint {

inline constexpr size_t kMaxRequestAttributes = 128;

enum class IppRequestError : uint8_t {
    Ok = 0,
    InvalidArgument,
    Truncated,
    UnsupportedVersion,
    UnsupportedOperation,
    InvalidGroupOrder,
    TooManyAttributes,
    MissingAttribute,
    DuplicateAttribute,
    InvalidAttributeTag,
    InvalidAttributeValue,
    MissingDocument,
    UnexpectedDocument,
    UnsupportedDocumentFormat,
};

const char* ipp_request_error_name(IppRequestError error) noexcept;
IppStatus ipp_request_error_status(IppRequestError error) noexcept;

struct IppRequestPolicy {
    // nullptr accepts any syntactically valid MIME value. A non-null value
    // restricts Print-Job/Validate-Job document-format to an exact match.
    const char* document_format = nullptr;
    bool require_ipp_uri = true;
    bool require_document = true;
    bool allow_duplicate_document_format = false;
};

struct IppRequest {
    IppMessageHeader header{};
    IppOperation operation = IppOperation::GetPrinterAttributes;
    IppAttributeView attributes[kMaxRequestAttributes] = {};
    size_t attribute_count = 0;
    const uint8_t* document_data = nullptr;
    size_t document_length = 0;
    IppRequestError error = IppRequestError::Ok;
    IppStatus status = IppStatus::SuccessfulOk;
    const uint8_t* error_attribute = nullptr;
    uint16_t error_attribute_length = 0;

    const IppAttributeView* find_first(const char* name) const noexcept;
    size_t count(const char* name) const noexcept;
    bool has(const char* name) const noexcept { return find_first(name) != nullptr; }
};

IppRequestError parse_ipp_request(const uint8_t* data, size_t length,
                                  const IppRequestPolicy& policy,
                                  IppRequest* out) noexcept;

}  // namespace airprint
}  // namespace ijpd

#endif  // IJPD_AIRPRINT_CORE_IPP_REQUEST_H
