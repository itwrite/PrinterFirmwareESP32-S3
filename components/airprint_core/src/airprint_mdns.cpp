// airprint_mdns.cpp - Deterministic AirPrint Bonjour record builder.
#include "airprint_core/airprint_mdns.h"

#include <cstdio>
#include <cstring>

namespace ijpd {
namespace airprint {
namespace {

bool copy_text(char* destination, size_t capacity, const char* source) noexcept {
    if (destination == nullptr || capacity == 0 || source == nullptr) return false;
    const size_t length = std::strlen(source);
    if (length >= capacity) return false;
    std::memcpy(destination, source, length + 1);
    return true;
}

bool format_text(char* destination, size_t capacity, const char* format,
                 const char* value) noexcept {
    if (destination == nullptr || capacity == 0 || format == nullptr || value == nullptr) {
        return false;
    }
    const int length = std::snprintf(destination, capacity, format, value);
    return length >= 0 && static_cast<size_t>(length) < capacity;
}

bool add_txt(AirPrintMdnsRecord* record, const char* key, const char* value) noexcept {
    if (record == nullptr || key == nullptr || value == nullptr ||
        record->txt_count >= kAirPrintMaxTxtItems) {
        return false;
    }
    AirPrintTxtItem& item = record->txt[record->txt_count];
    if (!copy_text(item.key, sizeof(item.key), key) ||
        !copy_text(item.value, sizeof(item.value), value)) {
        return false;
    }
    ++record->txt_count;
    return true;
}

uint32_t fnv1a(const char* value, uint32_t seed) noexcept {
    uint32_t hash = seed;
    for (size_t i = 0; value[i] != '\0'; ++i) {
        hash ^= static_cast<uint8_t>(value[i]);
        hash *= 16777619u;
    }
    return hash;
}

bool make_uuid(const char* device_id, char* output, size_t capacity) noexcept {
    const uint32_t h0 = fnv1a(device_id, 2166136261u);
    const uint32_t h1 = fnv1a(device_id, 2166136261u ^ 0x9E3779B9u);
    const uint32_t h2 = fnv1a(device_id, 2166136261u ^ 0xA5A5A5A5u);
    const uint32_t h3 = fnv1a(device_id, 2166136261u ^ 0x3C6EF372u);
    const int length = std::snprintf(
        output, capacity, "%08x-%04x-%04x-%04x-%04x%08x",
        static_cast<unsigned>(h0), static_cast<unsigned>(h1 >> 16),
        static_cast<unsigned>(h1 & 0x0FFFu) | 0x5000u,
        static_cast<unsigned>((h2 >> 16) & 0x3FFFu) | 0x8000u,
        static_cast<unsigned>(h2 & 0xFFFFu), static_cast<unsigned>(h3));
    return length == 36;
}

}  // namespace

const char* airprint_mdns_error_name(AirPrintMdnsError error) noexcept {
    switch (error) {
        case AirPrintMdnsError::Ok: return "ok";
        case AirPrintMdnsError::InvalidArgument: return "invalid_argument";
        case AirPrintMdnsError::InvalidCapabilities: return "invalid_capabilities";
        case AirPrintMdnsError::OutputTooSmall: return "output_too_small";
    }
    return "unknown";
}

AirPrintMdnsError build_airprint_mdns_record(
    const AirPrintCapabilities& capabilities, const char* device_id,
    const char* hostname, AirPrintMdnsRecord* out) noexcept {
    if (out == nullptr || device_id == nullptr || hostname == nullptr ||
        device_id[0] == '\0' || hostname[0] == '\0') {
        return AirPrintMdnsError::InvalidArgument;
    }
    const AirPrintResponseError valid = validate_airprint_capabilities(capabilities);
    if (valid != AirPrintResponseError::Ok) return AirPrintMdnsError::InvalidCapabilities;
    *out = AirPrintMdnsRecord{};
    out->port = capabilities.ipp_port;
    if (!copy_text(out->instance_name, sizeof(out->instance_name), capabilities.printer_name) ||
        !copy_text(out->hostname, sizeof(out->hostname), hostname) ||
        !make_uuid(device_id, out->uuid, sizeof(out->uuid))) {
        return AirPrintMdnsError::OutputTooSmall;
    }

    char product[kAirPrintMaxTxtValue] = {};
    char admin_url[kAirPrintMaxTxtValue] = {};
    char color[2] = {capabilities.color_supported ? 'T' : 'F', '\0'};
    char duplex[2] = {capabilities.duplex_supported ? 'T' : 'F', '\0'};
    if (!format_text(product, sizeof(product), "(%s)", capabilities.make_and_model) ||
        !format_text(admin_url, sizeof(admin_url), "http://%s/", hostname)) {
        return AirPrintMdnsError::OutputTooSmall;
    }

    const char* pdl = std::strcmp(capabilities.document_format, "image/urf") == 0
                          ? "image/urf,application/octet-stream"
                          : capabilities.document_format;
    if (!add_txt(out, "txtvers", "1") || !add_txt(out, "qtotal", "1") ||
        !add_txt(out, "ty", capabilities.printer_name) ||
        !add_txt(out, "product", product) ||
        !add_txt(out, "note", capabilities.printer_info) ||
        !add_txt(out, "adminurl", admin_url) || !add_txt(out, "priority", "0") ||
        !add_txt(out, "UUID", out->uuid) || !add_txt(out, "rp", "ipp/print") ||
        !add_txt(out, "pdl", pdl) ||
        !add_txt(out, "URF", capabilities.urf_supported) ||
        !add_txt(out, "Color", color) || !add_txt(out, "Duplex", duplex) ||
        !add_txt(out, "Scan", "F")) {
        return AirPrintMdnsError::OutputTooSmall;
    }
    return AirPrintMdnsError::Ok;
}

}  // namespace airprint
}  // namespace ijpd
