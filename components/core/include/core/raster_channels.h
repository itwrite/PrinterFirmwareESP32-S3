// ============================================================================
// raster_channels.h - Shared raster channel names for start_print metadata.
// ============================================================================
#ifndef IJPD_CORE_RASTER_CHANNELS_H
#define IJPD_CORE_RASTER_CHANNELS_H

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace ijpd {

enum class RasterChannel : uint8_t {
    Unknown = 0,
    Black,
    Cyan,
    Magenta,
    Yellow,
    LightCyan,
    LightMagenta,
    Gray,
    LightGray,
    Red,
    Green,
    Orange,
    Violet,
    White,
    Clear,
    Custom,
};

inline RasterChannel raster_channel_from_name(const char* name, size_t len) noexcept {
    struct Name {
        const char* value;
        RasterChannel channel;
    };
    static constexpr Name names[] = {
        {"black", RasterChannel::Black},
        {"cyan", RasterChannel::Cyan},
        {"magenta", RasterChannel::Magenta},
        {"yellow", RasterChannel::Yellow},
        {"lightCyan", RasterChannel::LightCyan},
        {"lightMagenta", RasterChannel::LightMagenta},
        {"gray", RasterChannel::Gray},
        {"lightGray", RasterChannel::LightGray},
        {"red", RasterChannel::Red},
        {"green", RasterChannel::Green},
        {"orange", RasterChannel::Orange},
        {"violet", RasterChannel::Violet},
        {"white", RasterChannel::White},
        {"clear", RasterChannel::Clear},
        {"custom", RasterChannel::Custom},
    };
    for (const Name& item : names) {
        const size_t item_len = std::strlen(item.value);
        if (item_len == len && std::memcmp(item.value, name, len) == 0) {
            return item.channel;
        }
    }
    return RasterChannel::Unknown;
}

inline const char* raster_channel_name(RasterChannel channel) noexcept {
    switch (channel) {
        case RasterChannel::Black:        return "black";
        case RasterChannel::Cyan:         return "cyan";
        case RasterChannel::Magenta:      return "magenta";
        case RasterChannel::Yellow:       return "yellow";
        case RasterChannel::LightCyan:    return "lightCyan";
        case RasterChannel::LightMagenta: return "lightMagenta";
        case RasterChannel::Gray:         return "gray";
        case RasterChannel::LightGray:    return "lightGray";
        case RasterChannel::Red:          return "red";
        case RasterChannel::Green:        return "green";
        case RasterChannel::Orange:       return "orange";
        case RasterChannel::Violet:       return "violet";
        case RasterChannel::White:        return "white";
        case RasterChannel::Clear:        return "clear";
        case RasterChannel::Custom:       return "custom";
        case RasterChannel::Unknown:      return "unknown";
    }
    return "unknown";
}

}  // namespace ijpd

#endif  // IJPD_CORE_RASTER_CHANNELS_H
