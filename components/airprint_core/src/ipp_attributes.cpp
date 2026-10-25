// ipp_attributes.cpp - First-slice IPP attribute registry.
#include "airprint_core/ipp_attributes.h"

#include <cstring>

namespace ijpd {
namespace airprint {
namespace {

constexpr IppAttributeSpec kSpecs[] = {
    {"attributes-charset", IppValueTag::Charset},
    {"attributes-natural-language", IppValueTag::NaturalLanguage},
    {"printer-uri", IppValueTag::Uri},
    {"requesting-user-name", IppValueTag::NameWithoutLanguage},
    {"job-name", IppValueTag::NameWithoutLanguage},
    {"document-format", IppValueTag::MimeMediaType},
    {"document-format-version", IppValueTag::TextWithoutLanguage},
    {"ipp-attribute-fidelity", IppValueTag::Boolean},
    {"requested-attributes", IppValueTag::Keyword},
    {"job-id", IppValueTag::Integer},
    {"job-uri", IppValueTag::Uri},
    {"printer-state", IppValueTag::Enum},
    {"printer-state-reasons", IppValueTag::Keyword},
    {"printer-is-accepting-jobs", IppValueTag::Boolean},
    {"queued-job-count", IppValueTag::Integer},
    {"operations-supported", IppValueTag::Enum},
    {"ipp-versions-supported", IppValueTag::Keyword},
    {"document-format-supported", IppValueTag::MimeMediaType},
    {"media-supported", IppValueTag::Keyword},
    {"urf-supported", IppValueTag::Keyword},
    {"color-supported", IppValueTag::Boolean},
    {"sides-supported", IppValueTag::Keyword},
    {"printer-make-and-model", IppValueTag::TextWithoutLanguage},
    {"printer-name", IppValueTag::NameWithoutLanguage},
    {"printer-location", IppValueTag::TextWithoutLanguage},
    {"printer-info", IppValueTag::TextWithoutLanguage},
    {"job-state", IppValueTag::Enum},
    {"job-state-reasons", IppValueTag::Keyword},
    {"job-originating-user-name", IppValueTag::NameWithoutLanguage},
    {"time-at-creation", IppValueTag::DateTime},
    {"time-at-processing", IppValueTag::DateTime},
    {"time-at-completed", IppValueTag::DateTime},
    {"pages-completed", IppValueTag::Integer},
    {"job-k-octets", IppValueTag::Integer},
};

}  // namespace

const IppAttributeSpec* find_attribute_spec(const char* name,
                                            size_t name_length) noexcept {
    if (name == nullptr) return nullptr;
    for (const IppAttributeSpec& spec : kSpecs) {
        const size_t length = std::strlen(spec.name);
        if (length == name_length && std::memcmp(spec.name, name, name_length) == 0) {
            return &spec;
        }
    }
    return nullptr;
}

bool attribute_name_equals(const IppAttributeView& attribute,
                           const char* name) noexcept {
    if (name == nullptr || attribute.name == nullptr) return false;
    const size_t length = std::strlen(name);
    return length == attribute.name_length &&
           std::memcmp(attribute.name, name, length) == 0;
}

}  // namespace airprint
}  // namespace ijpd
