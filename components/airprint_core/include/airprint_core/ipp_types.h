// ============================================================================
// ipp_types.h - Bounded IPP protocol types shared by host tests and ESP-IDF.
//
// IPP uses network byte order (big endian). It is deliberately separate from
// IJPD's little-endian protocol types and error codes.
// ============================================================================
#ifndef IJPD_AIRPRINT_CORE_IPP_TYPES_H
#define IJPD_AIRPRINT_CORE_IPP_TYPES_H

#include <cstddef>
#include <cstdint>

namespace ijpd {
namespace airprint {

enum class IppError : uint8_t {
    Ok = 0,
    EndOfAttributes,
    InvalidArgument,
    InvalidState,
    Truncated,
    InvalidTag,
    InvalidLength,
    OutputTooSmall,
    Unsupported,
};

const char* ipp_error_name(IppError error) noexcept;

struct IppVersion {
    uint8_t major = 2;
    uint8_t minor = 0;
};

struct IppMessageHeader {
    IppVersion version{};
    uint16_t code = 0;
    uint32_t request_id = 0;
};

enum class IppGroupTag : uint8_t {
    None = 0x00,
    OperationAttributes = 0x01,
    JobAttributes = 0x02,
    EndOfAttributes = 0x03,
    PrinterAttributes = 0x04,
    UnsupportedAttributes = 0x05,
    SubscriptionAttributes = 0x06,
    EventNotificationAttributes = 0x07,
};

enum class IppValueTag : uint8_t {
    Unsupported = 0x10,
    Default = 0x11,
    Unknown = 0x12,
    NoValue = 0x13,
    NotSettable = 0x15,
    DeleteAttribute = 0x16,
    AdminDefine = 0x17,

    Integer = 0x21,
    Boolean = 0x22,
    Enum = 0x23,
    OctetString = 0x30,
    DateTime = 0x31,
    Resolution = 0x32,
    RangeOfInteger = 0x33,
    BegCollection = 0x34,
    TextWithLanguage = 0x35,
    NameWithLanguage = 0x36,
    EndCollection = 0x37,

    TextWithoutLanguage = 0x41,
    NameWithoutLanguage = 0x42,
    ReservedString = 0x43,
    Keyword = 0x44,
    Uri = 0x45,
    UriScheme = 0x46,
    Charset = 0x47,
    NaturalLanguage = 0x48,
    MimeMediaType = 0x49,
    MemberAttrName = 0x4A,
};

enum class IppReadKind : uint8_t {
    Attribute,
    GroupStart,
    EndOfAttributes,
};

struct IppAttributeView {
    IppGroupTag group = IppGroupTag::None;
    IppValueTag value_tag = IppValueTag::Unknown;
    const uint8_t* name = nullptr;
    uint16_t name_length = 0;
    const uint8_t* value = nullptr;
    uint16_t value_length = 0;
    bool continuation = false;
};

struct IppReadResult {
    IppError error = IppError::Ok;
    IppReadKind kind = IppReadKind::Attribute;
    IppGroupTag group = IppGroupTag::None;
    IppAttributeView attribute{};

    static IppReadResult attribute_result(const IppAttributeView& value) noexcept {
        IppReadResult result;
        result.kind = IppReadKind::Attribute;
        result.group = value.group;
        result.attribute = value;
        return result;
    }

    static IppReadResult group_result(IppGroupTag value) noexcept {
        IppReadResult result;
        result.kind = IppReadKind::GroupStart;
        result.group = value;
        return result;
    }

    static IppReadResult end_result() noexcept {
        IppReadResult result;
        result.kind = IppReadKind::EndOfAttributes;
        return result;
    }

    static IppReadResult error_result(IppError value) noexcept {
        IppReadResult result;
        result.error = value;
        return result;
    }
};

enum class IppOperation : uint16_t {
    PrintJob = 0x0002,
    ValidateJob = 0x0004,
    CancelJob = 0x0008,
    GetJobAttributes = 0x0009,
    GetJobs = 0x000A,
    GetPrinterAttributes = 0x000B,
};

enum class IppStatus : uint16_t {
    SuccessfulOk = 0x0000,
    SuccessfulOkIgnoredOrSubstitutedAttributes = 0x0001,
    SuccessfulOkConflictingAttributes = 0x0002,

    ClientErrorBadRequest = 0x0400,
    ClientErrorForbidden = 0x0401,
    ClientErrorNotAuthenticated = 0x0402,
    ClientErrorNotAuthorized = 0x0403,
    ClientErrorNotPossible = 0x0404,
    ClientErrorTimeout = 0x0405,
    ClientErrorNotFound = 0x0406,
    ClientErrorGone = 0x0407,
    ClientErrorRequestEntityTooLarge = 0x0408,
    ClientErrorRequestValueTooLong = 0x0409,
    ClientErrorDocumentFormatNotSupported = 0x040A,
    ClientErrorAttributesOrValuesNotSupported = 0x040B,
    ClientErrorUriSchemeNotSupported = 0x040C,
    ClientErrorCharsetNotSupported = 0x040D,
    ClientErrorConflictingAttributes = 0x040E,
    ClientErrorCompressionNotSupported = 0x040F,
    ClientErrorCompressionError = 0x0410,
    ClientErrorDocumentFormatError = 0x0411,
    ClientErrorDocumentAccessError = 0x0412,
    ClientErrorUriTooLong = 0x0413,
    ClientErrorRequestIdConflicts = 0x0414,
    ClientErrorDocumentUnprintable = 0x0415,
    ClientErrorAccountInfoNeeded = 0x0416,
    ClientErrorAccountClosed = 0x0417,
    ClientErrorAccountLimitReached = 0x0418,
    ClientErrorAccountAuthorizationFailed = 0x0419,

    ServerErrorInternalError = 0x0500,
    ServerErrorOperationNotSupported = 0x0501,
    ServerErrorServiceUnavailable = 0x0502,
    ServerErrorVersionNotSupported = 0x0503,
    ServerErrorDeviceError = 0x0504,
    ServerErrorTemporaryError = 0x0505,
    ServerErrorNotAcceptingJobs = 0x0506,
    ServerErrorBusy = 0x0507,
    ServerErrorJobCanceled = 0x0508,
    ServerErrorMultipleDocumentJobsNotSupported = 0x0509,
};

const char* ipp_operation_name(IppOperation operation) noexcept;
const char* ipp_status_name(IppStatus status) noexcept;

inline bool is_group_tag(uint8_t tag) noexcept {
    return tag >= static_cast<uint8_t>(IppGroupTag::OperationAttributes) &&
           tag <= static_cast<uint8_t>(IppGroupTag::EventNotificationAttributes);
}

inline bool is_value_tag(uint8_t tag) noexcept {
    return tag >= static_cast<uint8_t>(IppValueTag::Unsupported);
}

}  // namespace airprint
}  // namespace ijpd

#endif  // IJPD_AIRPRINT_CORE_IPP_TYPES_H
