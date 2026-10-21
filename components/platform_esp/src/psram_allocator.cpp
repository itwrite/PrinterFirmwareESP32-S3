// psram_allocator.cpp —— PSRAM 分配与容量探测
#include "platform_esp/psram_allocator.h"

#include "esp_heap_caps.h"
#include "esp_psram.h"

#include "platform_esp/log_adapter.h"

namespace ijpd {
namespace {
constexpr const char* kTag = "psram";
}

Err PlatformMemory::init() noexcept {
    deinit();

    if (!esp_psram_is_initialized()) {
        IJPD_LOGE(kTag, "PSRAM not initialized (check sdkconfig/board)");
        return Err::PSRAM_UNAVAILABLE;
    }

    slots = static_cast<uint8_t*>(
        heap_caps_malloc(cfg::SLOT_COUNT * cfg::SLOT_SIZE, MALLOC_CAP_SPIRAM));
    if (slots == nullptr) {
        IJPD_LOGE(kTag, "slot pool alloc failed (%u x %u)",
                  static_cast<unsigned>(cfg::SLOT_COUNT),
                  static_cast<unsigned>(cfg::SLOT_SIZE));
        return Err::PSRAM_UNAVAILABLE;
    }

    for (size_t i = 0; i < cfg::COMPRESSED_BUF_COUNT; ++i) {
        comp[i] = static_cast<uint8_t*>(
            heap_caps_malloc(cfg::COMPRESSED_BUF_SIZE, MALLOC_CAP_SPIRAM));
        if (comp[i] == nullptr) {
            IJPD_LOGE(kTag, "compressed buffer %u alloc failed", static_cast<unsigned>(i));
            deinit();
            return Err::PSRAM_UNAVAILABLE;
        }
    }

    psram_available = true;
    refresh();
    IJPD_LOGI(kTag, "PSRAM ok: total=%u free=%u slots=%uKB comp=%ux%uKB",
              static_cast<unsigned>(psram_total_bytes),
              static_cast<unsigned>(psram_free_bytes),
              static_cast<unsigned>((cfg::SLOT_COUNT * cfg::SLOT_SIZE) / 1024),
              static_cast<unsigned>(cfg::COMPRESSED_BUF_COUNT),
              static_cast<unsigned>(cfg::COMPRESSED_BUF_SIZE / 1024));
    return Err::OK;
}

void PlatformMemory::deinit() noexcept {
    if (slots != nullptr) {
        heap_caps_free(slots);
        slots = nullptr;
    }
    for (auto& c : comp) {
        if (c != nullptr) {
            heap_caps_free(c);
            c = nullptr;
        }
    }
    psram_available = false;
}

void PlatformMemory::refresh() noexcept {
    if (!esp_psram_is_initialized()) {
        psram_available = false;
        return;
    }
    psram_total_bytes = static_cast<uint32_t>(esp_psram_get_size());
    psram_free_bytes = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
}

}  // namespace ijpd
