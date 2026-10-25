// ipp_codec.cpp - Bounded IPP binary reader/writer.
#include "airprint_core/ipp_codec.h"

#include <cstring>

namespace ijpd {
namespace airprint {
namespace {

uint16_t read_be16(const uint8_t* p) noexcept {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

uint32_t read_be32(const uint8_t* p) noexcept {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

void write_be16(uint8_t* p, uint16_t value) noexcept {
    p[0] = static_cast<uint8_t>(value >> 8);
    p[1] = static_cast<uint8_t>(value);
}

void write_be32(uint8_t* p, uint32_t value) noexcept {
    p[0] = static_cast<uint8_t>(value >> 24);
    p[1] = static_cast<uint8_t>(value >> 16);
    p[2] = static_cast<uint8_t>(value >> 8);
    p[3] = static_cast<uint8_t>(value);
}

bool is_valid_group(IppGroupTag group) noexcept {
    return group == IppGroupTag::OperationAttributes ||
           group == IppGroupTag::JobAttributes ||
           group == IppGroupTag::PrinterAttributes ||
           group == IppGroupTag::UnsupportedAttributes ||
           group == IppGroupTag::SubscriptionAttributes ||
           group == IppGroupTag::EventNotificationAttributes;
}

}  // namespace

const char* ipp_error_name(IppError error) noexcept {
    switch (error) {
        case IppError::Ok:              return "ok";
        case IppError::EndOfAttributes: return "end_of_attributes";
        case IppError::InvalidArgument: return "invalid_argument";
        case IppError::InvalidState:    return "invalid_state";
        case IppError::Truncated:       return "truncated";
        case IppError::InvalidTag:      return "invalid_tag";
        case IppError::InvalidLength:   return "invalid_length";
        case IppError::OutputTooSmall:  return "output_too_small";
        case IppError::Unsupported:     return "unsupported";
    }
    return "unknown";
}

const char* ipp_operation_name(IppOperation operation) noexcept {
    switch (operation) {
        case IppOperation::PrintJob:            return "Print-Job";
        case IppOperation::ValidateJob:         return "Validate-Job";
        case IppOperation::CancelJob:           return "Cancel-Job";
        case IppOperation::GetJobAttributes:    return "Get-Job-Attributes";
        case IppOperation::GetJobs:             return "Get-Jobs";
        case IppOperation::GetPrinterAttributes:return "Get-Printer-Attributes";
    }
    return "Unknown-Operation";
}

const char* ipp_status_name(IppStatus status) noexcept {
    switch (status) {
        case IppStatus::SuccessfulOk: return "successful-ok";
        case IppStatus::SuccessfulOkIgnoredOrSubstitutedAttributes:
            return "successful-ok-ignored-or-substituted-attributes";
        case IppStatus::SuccessfulOkConflictingAttributes:
            return "successful-ok-conflicting-attributes";
        case IppStatus::ClientErrorBadRequest: return "client-error-bad-request";
        case IppStatus::ClientErrorForbidden: return "client-error-forbidden";
        case IppStatus::ClientErrorNotAuthenticated: return "client-error-not-authenticated";
        case IppStatus::ClientErrorNotAuthorized: return "client-error-not-authorized";
        case IppStatus::ClientErrorNotPossible: return "client-error-not-possible";
        case IppStatus::ClientErrorTimeout: return "client-error-timeout";
        case IppStatus::ClientErrorNotFound: return "client-error-not-found";
        case IppStatus::ClientErrorGone: return "client-error-gone";
        case IppStatus::ClientErrorRequestEntityTooLarge:
            return "client-error-request-entity-too-large";
        case IppStatus::ClientErrorRequestValueTooLong:
            return "client-error-request-value-too-long";
        case IppStatus::ClientErrorDocumentFormatNotSupported:
            return "client-error-document-format-not-supported";
        case IppStatus::ClientErrorAttributesOrValuesNotSupported:
            return "client-error-attributes-or-values-not-supported";
        case IppStatus::ClientErrorUriSchemeNotSupported:
            return "client-error-uri-scheme-not-supported";
        case IppStatus::ClientErrorCharsetNotSupported:
            return "client-error-charset-not-supported";
        case IppStatus::ClientErrorConflictingAttributes:
            return "client-error-conflicting-attributes";
        case IppStatus::ClientErrorCompressionNotSupported:
            return "client-error-compression-not-supported";
        case IppStatus::ClientErrorCompressionError: return "client-error-compression-error";
        case IppStatus::ClientErrorDocumentFormatError:
            return "client-error-document-format-error";
        case IppStatus::ClientErrorDocumentAccessError:
            return "client-error-document-access-error";
        case IppStatus::ClientErrorUriTooLong: return "client-error-uri-too-long";
        case IppStatus::ClientErrorRequestIdConflicts:
            return "client-error-request-id-conflicts";
        case IppStatus::ClientErrorDocumentUnprintable:
            return "client-error-document-unprintable";
        case IppStatus::ClientErrorAccountInfoNeeded:
            return "client-error-account-info-needed";
        case IppStatus::ClientErrorAccountClosed: return "client-error-account-closed";
        case IppStatus::ClientErrorAccountLimitReached:
            return "client-error-account-limit-reached";
        case IppStatus::ClientErrorAccountAuthorizationFailed:
            return "client-error-account-authorization-failed";
        case IppStatus::ServerErrorInternalError: return "server-error-internal-error";
        case IppStatus::ServerErrorOperationNotSupported:
            return "server-error-operation-not-supported";
        case IppStatus::ServerErrorServiceUnavailable:
            return "server-error-service-unavailable";
        case IppStatus::ServerErrorVersionNotSupported:
            return "server-error-version-not-supported";
        case IppStatus::ServerErrorDeviceError: return "server-error-device-error";
        case IppStatus::ServerErrorTemporaryError: return "server-error-temporary-error";
        case IppStatus::ServerErrorNotAcceptingJobs:
            return "server-error-not-accepting-jobs";
        case IppStatus::ServerErrorBusy: return "server-error-busy";
        case IppStatus::ServerErrorJobCanceled: return "server-error-job-canceled";
        case IppStatus::ServerErrorMultipleDocumentJobsNotSupported:
            return "server-error-multiple-document-jobs-not-supported";
    }
    return "unknown-status";
}

IppError IppReader::begin(const uint8_t* data, size_t length) noexcept {
    *this = IppReader{};
    if (data == nullptr) return IppError::InvalidArgument;
    if (length < 8) return IppError::Truncated;

    header_.version.major = data[0];
    header_.version.minor = data[1];
    header_.code = read_be16(data + 2);
    header_.request_id = read_be32(data + 4);
    data_ = data;
    length_ = length;
    offset_ = 8;
    started_ = true;
    return IppError::Ok;
}

IppReadResult IppReader::next() noexcept {
    if (!started_) return IppReadResult::error_result(IppError::InvalidState);
    if (ended_) return IppReadResult::end_result();
    if (offset_ >= length_) return IppReadResult::error_result(IppError::Truncated);

    const uint8_t tag = data_[offset_++];
    if (tag == static_cast<uint8_t>(IppGroupTag::EndOfAttributes)) {
        if (collection_depth_ != 0) return IppReadResult::error_result(IppError::InvalidTag);
        ended_ = true;
        document_offset_ = offset_;
        return IppReadResult::end_result();
    }
    if (is_group_tag(tag)) {
        if (collection_depth_ != 0) return IppReadResult::error_result(IppError::InvalidTag);
        const IppGroupTag group = static_cast<IppGroupTag>(tag);
        if (!is_valid_group(group)) return IppReadResult::error_result(IppError::InvalidTag);
        current_group_ = group;
        last_name_ = nullptr;
        last_name_length_ = 0;
        return IppReadResult::group_result(group);
    }
    if (!is_value_tag(tag) || current_group_ == IppGroupTag::None) {
        return IppReadResult::error_result(IppError::InvalidTag);
    }
    if (length_ - offset_ < 2) return IppReadResult::error_result(IppError::Truncated);
    const uint16_t name_length = read_be16(data_ + offset_);
    offset_ += 2;
    if (name_length > length_ - offset_) {
        return IppReadResult::error_result(IppError::Truncated);
    }
    const uint8_t* name = data_ + offset_;
    offset_ += name_length;
    if (length_ - offset_ < 2) return IppReadResult::error_result(IppError::Truncated);
    const uint16_t value_length = read_be16(data_ + offset_);
    offset_ += 2;
    if (value_length > length_ - offset_) {
        return IppReadResult::error_result(IppError::Truncated);
    }
    const uint8_t* value = data_ + offset_;
    offset_ += value_length;

    IppAttributeView attribute;
    attribute.group = current_group_;
    attribute.value_tag = static_cast<IppValueTag>(tag);
    attribute.value = value;
    attribute.value_length = value_length;
    if (name_length == 0) {
        if (last_name_ == nullptr) return IppReadResult::error_result(IppError::InvalidLength);
        if (static_cast<IppValueTag>(tag) == IppValueTag::MemberAttrName &&
            collection_depth_ == 0) {
            return IppReadResult::error_result(IppError::InvalidTag);
        }
        attribute.name = last_name_;
        attribute.name_length = last_name_length_;
        attribute.continuation = true;
    } else {
        attribute.name = name;
        attribute.name_length = name_length;
        attribute.continuation = false;
        last_name_ = name;
        last_name_length_ = name_length;
    }
    if (static_cast<IppValueTag>(tag) == IppValueTag::MemberAttrName &&
        name_length != 0) {
        return IppReadResult::error_result(IppError::InvalidLength);
    }
    if (static_cast<IppValueTag>(tag) == IppValueTag::BegCollection) {
        if (collection_depth_ == UINT8_MAX) return IppReadResult::error_result(IppError::InvalidState);
        ++collection_depth_;
    } else if (static_cast<IppValueTag>(tag) == IppValueTag::EndCollection) {
        if (collection_depth_ == 0) return IppReadResult::error_result(IppError::InvalidTag);
        --collection_depth_;
    }
    return IppReadResult::attribute_result(attribute);
}

const uint8_t* IppReader::document_data() const noexcept {
    return ended_ ? data_ + document_offset_ : nullptr;
}

size_t IppReader::document_length() const noexcept {
    return ended_ ? length_ - document_offset_ : 0;
}

IppError IppWriter::begin(IppVersion version, uint16_t code, uint32_t request_id) noexcept {
    length_ = 0;
    current_group_ = IppGroupTag::None;
    last_name_ = nullptr;
    last_name_length_ = 0;
    started_ = false;
    ended_ = false;
    if (data_ == nullptr || capacity_ < 8) return IppError::OutputTooSmall;
    uint8_t header[8] = {};
    header[0] = version.major;
    header[1] = version.minor;
    write_be16(header + 2, code);
    write_be32(header + 4, request_id);
    const IppError result = append_(header, sizeof(header));
    if (result == IppError::Ok) started_ = true;
    return result;
}

IppError IppWriter::begin_request(IppVersion version, IppOperation operation,
                                  uint32_t request_id) noexcept {
    return begin(version, static_cast<uint16_t>(operation), request_id);
}

IppError IppWriter::begin_response(IppVersion version, IppStatus status,
                                   uint32_t request_id) noexcept {
    return begin(version, static_cast<uint16_t>(status), request_id);
}

IppError IppWriter::validate_attribute_state_() const noexcept {
    if (!started_) return IppError::InvalidState;
    if (ended_) return IppError::InvalidState;
    if (current_group_ == IppGroupTag::None) return IppError::InvalidState;
    return IppError::Ok;
}

IppError IppWriter::begin_group(IppGroupTag group) noexcept {
    if (!started_ || ended_ || !is_valid_group(group)) return IppError::InvalidState;
    const IppError result = append_u8_(static_cast<uint8_t>(group));
    if (result != IppError::Ok) return result;
    current_group_ = group;
    last_name_ = nullptr;
    last_name_length_ = 0;
    return IppError::Ok;
}

IppError IppWriter::add_raw(IppValueTag tag, const char* name, size_t name_length,
                            const uint8_t* value, size_t value_length) noexcept {
    const IppError state = validate_attribute_state_();
    if (state != IppError::Ok) return state;
    if (static_cast<uint8_t>(tag) < static_cast<uint8_t>(IppValueTag::Unsupported) ||
        name == nullptr || name_length == 0 || name_length > 0xFFFFu ||
        (value == nullptr && value_length != 0) || value_length > 0xFFFFu) {
        return IppError::InvalidArgument;
    }
    const size_t needed = 1 + 2 + name_length + 2 + value_length;
    if (needed > capacity_ - length_) return IppError::OutputTooSmall;
    const IppError r1 = append_u8_(static_cast<uint8_t>(tag));
    if (r1 != IppError::Ok) return r1;
    const IppError r2 = append_be16_(static_cast<uint16_t>(name_length));
    if (r2 != IppError::Ok) return r2;
    const IppError r3 = append_(reinterpret_cast<const uint8_t*>(name), name_length);
    if (r3 != IppError::Ok) return r3;
    const IppError r4 = append_be16_(static_cast<uint16_t>(value_length));
    if (r4 != IppError::Ok) return r4;
    const IppError r5 = append_(value, value_length);
    if (r5 == IppError::Ok) {
        last_name_ = name;
        last_name_length_ = static_cast<uint16_t>(name_length);
    }
    return r5;
}

IppError IppWriter::add_continuation(IppValueTag tag, const uint8_t* value,
                                     size_t value_length) noexcept {
    const IppError state = validate_attribute_state_();
    if (state != IppError::Ok) return state;
    if (last_name_ == nullptr) return IppError::InvalidState;
    if ((value == nullptr && value_length != 0) || value_length > 0xFFFFu) {
        return IppError::InvalidArgument;
    }
    const size_t needed = 1 + 2 + 2 + value_length;
    if (needed > capacity_ - length_) return IppError::OutputTooSmall;
    const IppError r1 = append_u8_(static_cast<uint8_t>(tag));
    if (r1 != IppError::Ok) return r1;
    const IppError r2 = append_be16_(0);
    if (r2 != IppError::Ok) return r2;
    const IppError r3 = append_be16_(static_cast<uint16_t>(value_length));
    if (r3 != IppError::Ok) return r3;
    return append_(value, value_length);
}

IppError IppWriter::add_integer(const char* name, int32_t value) noexcept {
    if (name == nullptr) return IppError::InvalidArgument;
    uint8_t bytes[4] = {};
    write_be32(bytes, static_cast<uint32_t>(value));
    return add_raw(IppValueTag::Integer, name, std::strlen(name), bytes, sizeof(bytes));
}

IppError IppWriter::add_enum(const char* name, int32_t value) noexcept {
    if (name == nullptr) return IppError::InvalidArgument;
    uint8_t bytes[4] = {};
    write_be32(bytes, static_cast<uint32_t>(value));
    return add_raw(IppValueTag::Enum, name, std::strlen(name), bytes, sizeof(bytes));
}

IppError IppWriter::add_boolean(const char* name, bool value) noexcept {
    if (name == nullptr) return IppError::InvalidArgument;
    const uint8_t byte = value ? 1 : 0;
    return add_raw(IppValueTag::Boolean, name, std::strlen(name), &byte, 1);
}

IppError IppWriter::add_string(IppValueTag tag, const char* name, const char* value,
                               size_t value_length) noexcept {
    if (name == nullptr) return IppError::InvalidArgument;
    return add_raw(tag, name, std::strlen(name),
                   reinterpret_cast<const uint8_t*>(value), value_length);
}

IppError IppWriter::add_cstr(IppValueTag tag, const char* name, const char* value) noexcept {
    if (value == nullptr) return IppError::InvalidArgument;
    return add_string(tag, name, value, std::strlen(value));
}

IppError IppWriter::add_resolution(const char* name, int32_t x, int32_t y,
                                   uint8_t unit) noexcept {
    if (name == nullptr) return IppError::InvalidArgument;
    uint8_t bytes[9] = {};
    write_be32(bytes, static_cast<uint32_t>(x));
    write_be32(bytes + 4, static_cast<uint32_t>(y));
    bytes[8] = unit;
    return add_raw(IppValueTag::Resolution, name, std::strlen(name), bytes, sizeof(bytes));
}

IppError IppWriter::begin_collection(const char* name) noexcept {
    if (name == nullptr || name[0] == '\0') return IppError::InvalidArgument;
    return add_raw(IppValueTag::BegCollection, name, std::strlen(name), nullptr, 0);
}

IppError IppWriter::add_collection_member_name(const char* name) noexcept {
    const IppError state = validate_attribute_state_();
    if (state != IppError::Ok) return state;
    if (name == nullptr || name[0] == '\0' || std::strlen(name) > 0xFFFFu) {
        return IppError::InvalidArgument;
    }
    const size_t value_length = std::strlen(name);
    const size_t needed = 1 + 2 + 2 + value_length;
    if (needed > capacity_ - length_) return IppError::OutputTooSmall;
    if (append_u8_(static_cast<uint8_t>(IppValueTag::MemberAttrName)) != IppError::Ok ||
        append_be16_(0) != IppError::Ok ||
        append_be16_(static_cast<uint16_t>(value_length)) != IppError::Ok ||
        append_(reinterpret_cast<const uint8_t*>(name), value_length) != IppError::Ok) {
        return IppError::OutputTooSmall;
    }
    // The following collection value is encoded as a continuation value.
    last_name_ = name;
    last_name_length_ = static_cast<uint16_t>(value_length);
    return IppError::Ok;
}

IppError IppWriter::end_collection() noexcept {
    const IppError state = validate_attribute_state_();
    if (state != IppError::Ok) return state;
    if (capacity_ - length_ < 5) return IppError::OutputTooSmall;
    if (append_u8_(static_cast<uint8_t>(IppValueTag::EndCollection)) != IppError::Ok ||
        append_be16_(0) != IppError::Ok || append_be16_(0) != IppError::Ok) {
        return IppError::OutputTooSmall;
    }
    last_name_ = nullptr;
    last_name_length_ = 0;
    return IppError::Ok;
}

IppError IppWriter::end_attributes() noexcept {
    if (!started_ || ended_) return IppError::InvalidState;
    const IppError result = append_u8_(static_cast<uint8_t>(IppGroupTag::EndOfAttributes));
    if (result == IppError::Ok) {
        ended_ = true;
        current_group_ = IppGroupTag::None;
    }
    return result;
}

IppError IppWriter::append_document(const uint8_t* data, size_t length) noexcept {
    if (!started_ || !ended_) return IppError::InvalidState;
    return append_(data, length);
}

IppError IppWriter::append_u8_(uint8_t value) noexcept {
    return append_(&value, 1);
}

IppError IppWriter::append_be16_(uint16_t value) noexcept {
    uint8_t bytes[2] = {};
    write_be16(bytes, value);
    return append_(bytes, sizeof(bytes));
}

IppError IppWriter::append_be32_(uint32_t value) noexcept {
    uint8_t bytes[4] = {};
    write_be32(bytes, value);
    return append_(bytes, sizeof(bytes));
}

IppError IppWriter::append_(const uint8_t* data, size_t length) noexcept {
    if (length > capacity_ - length_) return IppError::OutputTooSmall;
    if (length != 0 && data == nullptr) return IppError::InvalidArgument;
    if (length != 0) std::memcpy(data_ + length_, data, length);
    length_ += length;
    return IppError::Ok;
}

}  // namespace airprint
}  // namespace ijpd
