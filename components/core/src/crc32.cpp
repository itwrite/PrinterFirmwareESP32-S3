// crc32.cpp —— 查表实现，表在编译期生成（无运行时初始化开销）
#include "core/crc32.h"

#include <array>
#include <utility>

namespace ijpd {
namespace {

constexpr uint32_t crc_entry(uint32_t index) noexcept {
    uint32_t c = index;
    for (int i = 0; i < 8; ++i) {
        c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    }
    return c;
}

template <std::size_t... I>
constexpr std::array<uint32_t, 256> gen_table(std::index_sequence<I...>) noexcept {
    return std::array<uint32_t, 256>{{crc_entry(I)...}};
}

constexpr std::array<uint32_t, 256> kTable =
    gen_table(std::make_index_sequence<256>{});

}  // namespace

uint32_t crc32_update(uint32_t state, const uint8_t* data, size_t len) noexcept {
    for (size_t i = 0; i < len; ++i) {
        state = kTable[(state ^ data[i]) & 0xFFu] ^ (state >> 8);
    }
    return state;
}

uint32_t crc32(const uint8_t* data, size_t len) noexcept {
    return crc32_final(crc32_update(CRC32_INIT_STATE, data, len));
}

}  // namespace ijpd
