// display_service.cpp - ILI9341 240x320 preview display (esp_lcd, page cache)
#include "platform_esp/display_service.h"

#include <cstring>

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_ili9341.h"
#include "esp_heap_caps.h"

#include "platform_esp/log_adapter.h"
#include "platform_esp/task_layout.h"

namespace ijpd {
namespace {
constexpr const char* kTag = "disp";

constexpr int kPinMosi = 11;
constexpr int kPinSclk = 13;
constexpr int kPinCs = 10;
constexpr int kPinDc = 9;
constexpr int kPinRst = 14;
constexpr int kPinBl = 21;

constexpr int kLcdW = 240;
constexpr int kLcdH = 320;
constexpr int kSpiHost = 2;
constexpr unsigned kPclkHz = 10 * 1000 * 1000;  // 杜邦线下的保守提速

constexpr int kBandLines = 20;
constexpr size_t kFlushStack = 3072;  // 渲染循环栈占用小,从 4096 缩减给 ctl_tcp
constexpr uint32_t kFlushPeriodMs = 20;
constexpr uint32_t kPreviewPlanes = 4;
constexpr uint32_t kPageCapBpr = 384;
constexpr uint32_t kPageCapRows = 4000;
constexpr uint32_t kPageCapPlanes = 4;

// RasterImage/BitPacker 语义:0=纸面,1=着墨。
constexpr int kInkBitLevel = 1;
constexpr uint8_t kBitCoverage = 255;

uint8_t srgb_lut[256];
void build_srgb_lut() {
    for (int i = 0; i < 256; ++i) {
        double v = i / 255.0;
        double s = v <= 0.0031308 ? 12.92 * v : 1.055 * __builtin_pow(v, 1.0/2.4) - 0.055;
        srgb_lut[i] = static_cast<uint8_t>(s * 255.0 + 0.5);
    }
}

// The SPI panel expects RGB565 MSB first. The framebuffer lives as uint16_t
// on the little-endian ESP32, so store each pixel byte-swapped in memory.
inline uint16_t rgb565_wire(uint8_t r, uint8_t g, uint8_t b) {
    const uint16_t value = static_cast<uint16_t>(
        ((((r & 0xF8) >> 3) << 11) | (((g & 0xFC) >> 2) << 5) | (b >> 3)));
    return __builtin_bswap16(value);
}
const uint16_t kWhite565 = rgb565_wire(255, 255, 255);

}  // namespace

struct DisplayService::Impl {
    esp_lcd_panel_io_handle_t io = nullptr;
    esp_lcd_panel_handle_t panel = nullptr;
    uint16_t* fb = nullptr;
    bool job_active = false;
    uint32_t w_px = 0, h_px = 0, bits = 1, planes = 1, bpr = 0;
    bool interleaved = false;
    bool sequential_channels = false;
    uint32_t bytes_per_pixel = 1;
    uint32_t packed_pixel_bits = 0;
    uint32_t physical_width_um = 0;
    uint32_t dpi = 0;
    bool page_cache_enabled = false;
    uint8_t* page = nullptr;
    volatile uint32_t rows_seen = 0;
    uint32_t total_rows = 0;
    uint32_t cap_bpr = 0, cap_rows = 0, cap_planes = 0;
    uint32_t channel_order_count = 0;
    RasterChannel channel_order[8] = {};
    volatile bool final_render_pending = false;
    uint16_t* preview_acc = nullptr;
    uint16_t preview_counts[kPreviewPlanes * kLcdH] = {};
    volatile uint32_t preview_revision[kLcdH] = {};
    uint32_t preview_drawn_revision[kLcdH] = {};
    uint32_t view_w = 0, view_h = 0, view_x = 0, view_y = 0;
    uint32_t preview_sample_x[kLcdW][2] = {};
    uint8_t preview_sample_count[kLcdW] = {};
};

esp_err_t draw_frame_banded(esp_lcd_panel_handle_t panel, const uint16_t* fb) {
    for (int y = 0; y < kLcdH; y += kBandLines) {
        const int h = (kLcdH - y < kBandLines) ? kLcdH - y : kBandLines;
        const esp_err_t err = esp_lcd_panel_draw_bitmap(
            panel, 0, y, kLcdW, y + h, fb + y * kLcdW);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}

esp_err_t draw_rows_banded(esp_lcd_panel_handle_t panel, const uint16_t* fb,
                           uint32_t y_start, uint32_t y_end) {
    for (uint32_t y = y_start; y < y_end; y += kBandLines) {
        const uint32_t h = (y_end - y < kBandLines) ? y_end - y : kBandLines;
        const esp_err_t err = esp_lcd_panel_draw_bitmap(
            panel, 0, static_cast<int>(y), kLcdW, static_cast<int>(y + h),
            fb + y * kLcdW);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}

DisplayService& DisplayService::instance() noexcept {
    static DisplayService s;
    return s;
}

Err DisplayService::start() noexcept {
    if (started_) return Err::OK;
    build_srgb_lut();
    impl_ = new Impl();
    // page_lock 已移除:多核互斥锁引发 FreeRTOS 断言,无锁方案替代

    gpio_config_t bl = {};
    bl.pin_bit_mask = 1ULL << kPinBl;
    bl.mode = GPIO_MODE_OUTPUT;
    bl.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&bl);
    gpio_set_level(static_cast<gpio_num_t>(kPinBl), 1);

    spi_bus_config_t bus = {};
    bus.mosi_io_num = kPinMosi;
    bus.miso_io_num = -1;
    bus.sclk_io_num = kPinSclk;
    bus.quadwp_io_num = -1;
    bus.max_transfer_sz = kBandLines * kLcdW * 2;  // 分带推屏:只需带大小(19.2KB),整帧 150KB 会挤爆内部 RAM
    auto bus_err = spi_bus_initialize(static_cast<spi_host_device_t>(kSpiHost), &bus, SPI_DMA_CH_AUTO);
    IJPD_LOGI(kTag, "spi bus: %s", esp_err_to_name(bus_err));
    if (bus_err != ESP_OK) {
        return Err::INTERNAL_ERROR;
    }

    esp_lcd_panel_io_spi_config_t io_cfg = {};
    io_cfg.cs_gpio_num = kPinCs;
    io_cfg.dc_gpio_num = kPinDc;
    io_cfg.spi_mode = 0;
    io_cfg.pclk_hz = kPclkHz;
    io_cfg.trans_queue_depth = 2;
    io_cfg.lcd_cmd_bits = 8;      // ILI9341: 命令 8-bit
    io_cfg.lcd_param_bits = 8;    // 参数 8-bit
    auto io_err = esp_lcd_new_panel_io_spi(static_cast<esp_lcd_spi_bus_handle_t>(kSpiHost), &io_cfg, &impl_->io);
    IJPD_LOGI(kTag, "panel io: %s", esp_err_to_name(io_err));
    if (io_err != ESP_OK) return Err::INTERNAL_ERROR;

    esp_lcd_panel_dev_config_t pd = {};
    pd.reset_gpio_num = kPinRst;
    pd.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR;
    pd.bits_per_pixel = 16;
    auto p_err = esp_lcd_new_panel_ili9341(impl_->io, &pd, &impl_->panel);
    IJPD_LOGI(kTag, "panel: %s", esp_err_to_name(p_err));
    if (p_err != ESP_OK) return Err::INTERNAL_ERROR;
    esp_lcd_panel_reset(impl_->panel);
    esp_lcd_panel_init(impl_->panel);
    esp_lcd_panel_mirror(impl_->panel, true, false);
    esp_lcd_panel_disp_on_off(impl_->panel, true);

    impl_->fb = static_cast<uint16_t*>(heap_caps_malloc(kLcdW * kLcdH * 2, MALLOC_CAP_SPIRAM));
    if (!impl_->fb) { IJPD_LOGE(kTag, "fb alloc failed"); return Err::OUT_OF_MEMORY; }

    for (int y = 0; y < kLcdH; ++y)
        for (int x = 0; x < kLcdW; ++x)
            impl_->fb[y * kLcdW + x] = (((x+y)/24)&1)
                ? rgb565_wire(230, 234, 240)
                : rgb565_wire(206, 213, 224);
    const esp_err_t draw_err = draw_frame_banded(impl_->panel, impl_->fb);
    if (draw_err != ESP_OK) {
        IJPD_LOGE(kTag, "initial draw failed: %s", esp_err_to_name(draw_err));
        return Err::INTERNAL_ERROR;
    }
    IJPD_LOGI(kTag, "first draw done");

    if (task_layout::spawn("disp", flush_task_, this, 1, task_layout::kPrioReport, kFlushStack) != Err::OK)
        return Err::OUT_OF_MEMORY;
    started_ = true;
    IJPD_LOGI(kTag, "display ready %dx%d", kLcdW, kLcdH);
    return Err::OK;
}

Err DisplayService::begin_job(uint32_t w_px, uint32_t h_px, uint32_t bits,
                               uint32_t planes, uint32_t bpr,
                               const RasterChannel* channel_order,
                               uint32_t channel_order_count,
                               bool interleaved,
                               uint32_t bytes_per_pixel,
                               uint32_t physical_width_um,
                               uint32_t dpi,
                               uint32_t packed_pixel_bits) noexcept {
    if (!started_ || !impl_) return Err::UNSUPPORTED;
    end_job();
    release_job_buffers_();
    if (bits != 1 && bits != 8) bits = 1;
    if (!planes) planes = 1;
    if (h_px > kPageCapRows || planes > kPageCapPlanes) {
        IJPD_LOGW(kTag, "job exceeds preview geometry, display skip");
        return Err::UNSUPPORTED;
    }
    impl_->page_cache_enabled = !interleaved && bpr <= kPageCapBpr;
    if (impl_->page_cache_enabled && !impl_->page) {
        impl_->page = static_cast<uint8_t*>(heap_caps_malloc(
            size_t(kPageCapBpr) * kPageCapRows * kPageCapPlanes, MALLOC_CAP_SPIRAM));
        if (!impl_->page) { IJPD_LOGW(kTag, "page alloc failed"); return Err::OUT_OF_MEMORY; }
        impl_->cap_bpr = kPageCapBpr; impl_->cap_rows = kPageCapRows; impl_->cap_planes = kPageCapPlanes;
    }
    if (!impl_->preview_acc) {
        impl_->preview_acc = static_cast<uint16_t*>(heap_caps_malloc(
            size_t(kPreviewPlanes) * kLcdH * kLcdW * sizeof(uint16_t),
            MALLOC_CAP_SPIRAM));
        if (!impl_->preview_acc) {
            IJPD_LOGW(kTag, "preview accumulator alloc failed");
            return Err::OUT_OF_MEMORY;
        }
    }
    if (impl_->page_cache_enabled && bpr < impl_->cap_bpr) {
        for (uint32_t r = 0; r < h_px * planes; ++r) {
            std::memset(impl_->page + size_t(r) * impl_->cap_bpr, 0xFF, bpr);
            std::memset(impl_->page + size_t(r) * impl_->cap_bpr + bpr, 0x00, impl_->cap_bpr - bpr);
        }
    }
    impl_->w_px = w_px; impl_->h_px = h_px; impl_->bits = bits;
    impl_->planes = planes; impl_->bpr = bpr;
    impl_->interleaved = interleaved;
    impl_->bytes_per_pixel = bytes_per_pixel;
    impl_->packed_pixel_bits = packed_pixel_bits;
    impl_->sequential_channels = false;
    impl_->physical_width_um = physical_width_um;
    impl_->dpi = dpi;
    impl_->rows_seen = 0;
    impl_->total_rows = interleaved ? h_px : h_px * planes;
    impl_->page_cache_enabled = !interleaved && bpr <= kPageCapBpr;
    impl_->final_render_pending = false;
    std::memset(impl_->preview_acc, 0,
                size_t(kPreviewPlanes) * kLcdH * kLcdW * sizeof(uint16_t));
    std::memset(impl_->preview_counts, 0, sizeof(impl_->preview_counts));
    for (uint32_t y = 0; y < kLcdH; ++y) impl_->preview_revision[y] = 1;
    std::memset(impl_->preview_drawn_revision, 0,
                sizeof(impl_->preview_drawn_revision));
    const uint64_t physical_w = physical_width_um != 0
                                    ? physical_width_um
                                    : (dpi != 0
                                           ? uint64_t(w_px) * 25400u / dpi
                                           : uint64_t(w_px));
    const uint64_t physical_h = dpi != 0
                                    ? uint64_t(h_px) * 25400u / dpi
                                    : uint64_t(h_px);
    if (physical_w * kLcdH >= physical_h * kLcdW) {
        impl_->view_w = kLcdW;
        impl_->view_h = static_cast<uint32_t>(physical_h * kLcdW / physical_w);
    } else {
        impl_->view_h = kLcdH;
        impl_->view_w = static_cast<uint32_t>(physical_w * kLcdH / physical_h);
    }
    if (impl_->view_w == 0) impl_->view_w = 1;
    if (impl_->view_h == 0) impl_->view_h = 1;
    impl_->view_x = (kLcdW - impl_->view_w) / 2;
    impl_->view_y = (kLcdH - impl_->view_h) / 2;
    for (uint32_t display_x = 0; display_x < impl_->view_w; ++display_x) {
        const uint32_t x0 = uint32_t(uint64_t(display_x) * impl_->w_px /
                                     impl_->view_w);
        uint32_t x1 = uint32_t(uint64_t(display_x + 1) * impl_->w_px /
                               impl_->view_w);
        if (x1 <= x0) x1 = x0 + 1;
        if (x1 > impl_->w_px) x1 = impl_->w_px;
        const uint32_t source_width = x1 - x0;
        const uint32_t samples = source_width > 1 ? 2 : 1;
        impl_->preview_sample_count[display_x] = static_cast<uint8_t>(samples);
        for (uint32_t sample = 0; sample < samples; ++sample) {
            impl_->preview_sample_x[display_x][sample] = x0 +
                static_cast<uint32_t>(uint64_t(sample * 2 + 1) * source_width /
                                      (samples * 2));
        }
    }
    for (uint32_t i = 0; i < kLcdW * kLcdH; ++i) impl_->fb[i] = kWhite565;
    impl_->channel_order_count = 0;
    if (channel_order != nullptr) {
        const uint32_t count = channel_order_count > 8 ? 8 : channel_order_count;
        for (uint32_t i = 0; i < count; ++i) {
            impl_->channel_order[i] = channel_order[i];
        }
        impl_->channel_order_count = count;
    }
    impl_->job_active = true;
    IJPD_LOGI(kTag, "job %lux%lu %lub %lup layout=%s width_um=%lu cache=%s",
              static_cast<unsigned long>(w_px),
              static_cast<unsigned long>(h_px), static_cast<unsigned long>(bits),
              static_cast<unsigned long>(planes), interleaved ? "interleaved" : "planar",
              static_cast<unsigned long>(physical_width_um),
              impl_->page_cache_enabled ? "full" : "preview-only");
    return Err::OK;
}

void DisplayService::feed_row(uint32_t idx, const uint8_t* data, size_t len) noexcept {
    if (!started_ || !impl_ || !impl_->job_active || !impl_->preview_acc) return;
    if (idx >= impl_->total_rows) return;
    if (impl_->page_cache_enabled && impl_->page) {
        const size_t off = size_t(idx) * impl_->cap_bpr;
        const size_t n = len < impl_->bpr ? len : impl_->bpr;
        std::memcpy(impl_->page + off, data, n);
    }
    // 无锁写:310B memcpy 亚微秒;rows_seen 32位对齐原子写。
    // FreeRTOS 互斥锁跨核使用时优先级继承簿记可触发
    // xTaskPriorityDisinherit 断言 → 整机崩溃(实测复现),不可接受。
    // 撕裂读对预览显示无视觉影响,数据完整性由逐包 CRC 保证。
    impl_->rows_seen = idx + 1;

    if (impl_->interleaved) {
        const uint32_t display_y = impl_->view_y + static_cast<uint32_t>(
            uint64_t(idx) * impl_->view_h / impl_->h_px);
        if (display_y >= kLcdH) return;
        const uint32_t per_row_bits = impl_->w_px * impl_->packed_pixel_bits;
        const uint32_t pixels_per_plane = per_row_bits == 0 ? 0
            : static_cast<uint32_t>((uint64_t(len) * 8) / per_row_bits);
        if (pixels_per_plane == 0) return;
        for (uint32_t plane = 0; plane < impl_->planes && plane < kPreviewPlanes; ++plane) {
            uint16_t* sums = impl_->preview_acc + size_t(plane) * kLcdH * kLcdW;
            for (uint32_t display_x = 0; display_x < impl_->view_w; ++display_x) {
                const uint32_t sample_count = impl_->preview_sample_count[display_x];
                uint32_t sum = 0;
                const uint32_t logical_plane = plane;
                for (uint32_t sample = 0; sample < sample_count; ++sample) {
                    const uint32_t x = impl_->preview_sample_x[display_x][sample];
                    const uint32_t bit_offset =
                        (x * impl_->packed_pixel_bits) + logical_plane * impl_->bits;
                    const uint32_t byte = bit_offset / 8;
                const uint32_t channel_bits = impl_->bits;
                    const uint32_t shift = 8 - channel_bits - (bit_offset & 7);
                    const uint32_t mask = (1u << channel_bits) - 1u;
                    if (byte < len) {
                        const uint32_t level = (data[byte] >> shift) & mask;
                        sum += level * kBitCoverage / mask;
                    }
                }
                sums[size_t(display_y) * kLcdW + impl_->view_x + display_x] =
                    static_cast<uint16_t>(sums[size_t(display_y) * kLcdW + impl_->view_x + display_x] +
                                          sum / sample_count);
            }
            impl_->preview_counts[plane * kLcdH + display_y]++;
        }
        impl_->preview_revision[display_y]++;
        return;
    }

    const uint32_t plane_height = impl_->h_px;
    const uint32_t source_y = impl_->interleaved ? idx : idx % plane_height;
    const uint32_t first_plane = impl_->interleaved ? 0 : idx / plane_height;
    const uint32_t display_y = impl_->view_y + static_cast<uint32_t>(
        uint64_t(source_y) * impl_->view_h / plane_height);
    if (display_y >= kLcdH) return;
    const uint32_t plane_count = impl_->interleaved ? impl_->planes : 1;
    for (uint32_t local_plane = 0; local_plane < plane_count; ++local_plane) {
        const uint32_t plane = first_plane + local_plane;
        if (plane >= kPreviewPlanes) break;
        const uint32_t count_index = plane * kLcdH + display_y;
        uint16_t* sums = impl_->preview_acc +
            (size_t(plane) * kLcdH + display_y) * kLcdW;
        for (uint32_t display_x = 0; display_x < impl_->view_w; ++display_x) {
            // The LCD is only a 240px preview. Two evenly spaced samples per
            // preview pixel preserve recognizable detail without repeatedly
            // dividing or scanning all source pixels on the TCP RX path.
            const uint32_t sample_count = impl_->preview_sample_count[display_x];
            uint32_t sum = 0;
            for (uint32_t sample = 0; sample < sample_count; ++sample) {
                const uint32_t x = impl_->preview_sample_x[display_x][sample];
                const uint32_t channel_bits = impl_->bits;
                const uint32_t source_plane = plane;
                const uint32_t bit_offset = impl_->interleaved
                    ? x * impl_->packed_pixel_bits + source_plane * channel_bits
                    : x * impl_->bits;
                const uint32_t packed_offset = bit_offset / 8;
                if (packed_offset >= len) continue;
                const uint32_t shift = 8 - channel_bits - (bit_offset & 7);
                const uint32_t mask = (1u << channel_bits) - 1u;
                const uint32_t level = (data[packed_offset] >> shift) & mask;
                sum += level * kBitCoverage / mask;
            }
            sums[impl_->view_x + display_x] = static_cast<uint16_t>(
                sums[impl_->view_x + display_x] + sum / sample_count);
        }
        impl_->preview_counts[count_index]++;
    }
    const uint32_t revision = impl_->preview_revision[display_y];
    impl_->preview_revision[display_y] = revision + 1;
}

void DisplayService::set_raster_contract(const RasterChannel* channel_order,
                                         uint32_t channel_count,
                                         uint32_t bits_per_channel,
                                         bool interleaved,
                                         uint32_t packed_pixel_bits,
                                         bool sequential_channels) noexcept {
    if (!impl_) return;
    impl_->channel_order_count = channel_count > 8 ? 8 : channel_count;
    for (uint32_t i = 0; i < impl_->channel_order_count; ++i) {
        impl_->channel_order[i] = channel_order != nullptr
            ? channel_order[i] : RasterChannel::Unknown;
    }
    impl_->bits = bits_per_channel;
    impl_->planes = channel_count;
    impl_->interleaved = interleaved;
    impl_->packed_pixel_bits = packed_pixel_bits;
    impl_->sequential_channels = sequential_channels;
    impl_->total_rows = impl_->interleaved ? impl_->h_px : impl_->h_px * impl_->planes;
    impl_->page_cache_enabled = !impl_->interleaved && impl_->bpr <= kPageCapBpr;
}

void DisplayService::store_row(uint32_t idx, const uint8_t* data, size_t len) noexcept {
    if (!started_ || !impl_ || !impl_->job_active || data == nullptr ||
        idx >= impl_->total_rows) return;
    if (impl_->page_cache_enabled && impl_->page) {
        const size_t off = size_t(idx) * impl_->cap_bpr;
        const size_t n = len < impl_->bpr ? len : impl_->bpr;
        std::memcpy(impl_->page + off, data, n);
    }
    impl_->rows_seen = idx + 1;
}

const uint8_t* DisplayService::peek_row(uint32_t idx) const noexcept {
    if (!impl_ || !impl_->page || !impl_->page_cache_enabled) return nullptr;
    if (idx >= impl_->total_rows) return nullptr;
    return impl_->page + size_t(idx) * impl_->cap_bpr;
}

void DisplayService::end_job() noexcept {
    if (!impl_) return;
    impl_->job_active = false;
    impl_->final_render_pending = impl_->preview_acc != nullptr;
}

void DisplayService::release_job_buffers_() noexcept {
    if (!impl_) return;
    if (impl_->page != nullptr) {
        heap_caps_free(impl_->page);
        impl_->page = nullptr;
    }
    if (impl_->preview_acc != nullptr) {
        heap_caps_free(impl_->preview_acc);
        impl_->preview_acc = nullptr;
    }
    impl_->page_cache_enabled = false;
    impl_->cap_bpr = 0;
    impl_->cap_rows = 0;
    impl_->cap_planes = 0;
    impl_->final_render_pending = false;
}

// ---- 渲染 ----

void DisplayService::render_preview_row_(uint32_t display_row) noexcept {
    if (!impl_ || !impl_->preview_acc || display_row >= kLcdH) return;
    uint16_t* line = impl_->fb + display_row * kLcdW;
    for (uint32_t sx = 0; sx < kLcdW; ++sx) {
        uint16_t color = kWhite565;
        if (sx >= impl_->view_x && sx < impl_->view_x + impl_->view_w) {
            uint8_t cv[kPreviewPlanes] = {};
            for (uint32_t p = 0; p < impl_->planes && p < kPreviewPlanes; ++p) {
                const uint16_t count = impl_->preview_counts[p * kLcdH + display_row];
                if (count != 0) {
                    const uint16_t* sums = impl_->preview_acc +
                        (size_t(p) * kLcdH + display_row) * kLcdW;
                    const uint32_t average = sums[sx] / count;
                    cv[p] = static_cast<uint8_t>(average > 255 ? 255 : average);
                }
            }
            uint32_t c = 0, m = 0, y = 0, k = 0, other = 0;
            bool has_cmyk = false;
            const uint32_t npl = impl_->planes > kPreviewPlanes ? kPreviewPlanes : impl_->planes;
            for (uint32_t p = 0; p < npl; ++p) {
                RasterChannel channel;
                if (p < impl_->channel_order_count) {
                    channel = impl_->channel_order[p];
                } else if (npl == 4) {
                    channel = p == 0 ? RasterChannel::Cyan
                             : p == 1 ? RasterChannel::Magenta
                             : p == 2 ? RasterChannel::Yellow
                                       : RasterChannel::Black;
                } else if (npl == 3) {
                    channel = p == 0 ? RasterChannel::Cyan
                             : p == 1 ? RasterChannel::Magenta
                                       : RasterChannel::Yellow;
                } else {
                    channel = RasterChannel::Black;
                }
                uint32_t coverage = cv[p];
                switch (channel) {
                    case RasterChannel::Cyan:
                        c = c > coverage ? c : coverage; has_cmyk = true; break;
                    case RasterChannel::Magenta:
                        m = m > coverage ? m : coverage; has_cmyk = true; break;
                    case RasterChannel::Yellow:
                        y = y > coverage ? y : coverage; has_cmyk = true; break;
                    case RasterChannel::Black:
                    case RasterChannel::Gray:
                        k = k > coverage ? k : coverage; has_cmyk = true; break;
                    case RasterChannel::LightCyan:
                        coverage = coverage * 127 / 255;
                        c = c > coverage ? c : coverage; has_cmyk = true; break;
                    case RasterChannel::LightMagenta:
                        coverage = coverage * 127 / 255;
                        m = m > coverage ? m : coverage; has_cmyk = true; break;
                    case RasterChannel::LightGray:
                        coverage = coverage * 127 / 255;
                        k = k > coverage ? k : coverage; has_cmyk = true; break;
                    default:
                        other += coverage;
                        break;
                }
            }
            uint32_t r = 255, g = 255, b = 255;
            if (has_cmyk) {
                r = r * (255 - c) / 255;
                g = g * (255 - m) / 255;
                b = b * (255 - y) / 255;
                r = r * (255 - k) / 255;
                g = g * (255 - k) / 255;
                b = b * (255 - k) / 255;
            } else {
                const uint32_t ink = npl == 0 ? 0 : other / npl;
                r = g = b = 255 - (ink > 255 ? 255 : ink);
            }
            color = rgb565_wire(srgb_lut[r], srgb_lut[g], srgb_lut[b]);
        }
        line[sx] = color;
    }
}

void DisplayService::render_incremental_() noexcept {
    if (!impl_ || !impl_->preview_acc ||
        (!impl_->job_active && !impl_->final_render_pending)) return;

    uint32_t display_row = 0;
    while (display_row < kLcdH) {
        while (display_row < kLcdH &&
               impl_->preview_revision[display_row] ==
                   impl_->preview_drawn_revision[display_row]) {
            ++display_row;
        }
        if (display_row >= kLcdH) break;

        const uint32_t y_start = display_row;
        uint32_t y_end = y_start;
        uint32_t revisions[kBandLines] = {};
        while (y_end < kLcdH && y_end - y_start < kBandLines &&
               impl_->preview_revision[y_end] !=
                   impl_->preview_drawn_revision[y_end]) {
            revisions[y_end - y_start] = impl_->preview_revision[y_end];
            render_preview_row_(y_end);
            ++y_end;
        }
        if (draw_rows_banded(impl_->panel, impl_->fb, y_start, y_end) != ESP_OK) return;
        for (uint32_t y = y_start; y < y_end; ++y) {
            if (impl_->preview_revision[y] == revisions[y - y_start]) {
                impl_->preview_drawn_revision[y] = revisions[y - y_start];
            }
        }
        display_row = y_end;
    }

    if (impl_->final_render_pending) {
        for (uint32_t y = 0; y < kLcdH; ++y) {
            if (impl_->preview_revision[y] != impl_->preview_drawn_revision[y]) return;
        }
        impl_->final_render_pending = false;
        release_job_buffers_();
    }
}

void DisplayService::render_frame_() noexcept {
    // 预览渲染:不持长锁(锁会阻塞 rx_task 的 feed_row,导致 TCP 背压)
    // 偶尔读到半写行对预览无视觉影响;数据完整性由 CRC 保证
    if (!impl_ || !impl_->page ||
        (!impl_->job_active && !impl_->final_render_pending)) return;
    const uint32_t w = impl_->w_px, h = impl_->h_px;
    const uint32_t npl = impl_->planes > 4 ? 4 : impl_->planes;
    const uint32_t capb = impl_->cap_bpr;
    const uint32_t avail = impl_->rows_seen < impl_->total_rows ? impl_->rows_seen : impl_->total_rows;
    uint16_t* fb = impl_->fb;

    uint32_t vw, vh;
    if (uint64_t(w) * kLcdH >= uint64_t(h) * kLcdW) { vw = kLcdW; vh = uint32_t(uint64_t(h)*kLcdW/w); }
    else { vh = kLcdH; vw = uint32_t(uint64_t(w)*kLcdH/h); }
    uint32_t ox = (kLcdW-vw)/2, oy = (kLcdH-vh)/2;
    const uint32_t pxH = h;

    static uint32_t acc[8][kLcdW];
    for (uint32_t sy = 0; sy < kLcdH; ++sy) {
        uint16_t* line = fb + sy * kLcdW;
        if (sy < oy || sy >= oy+vh || !avail) {
            for (uint32_t sx = 0; sx < kLcdW; ++sx) line[sx] = kWhite565;
            continue;
        }
        uint32_t y0 = uint32_t(uint64_t(sy-oy)*h/vh);
        uint32_t y1 = uint32_t(uint64_t(sy+1-oy)*h/vh);
        if (y1 <= y0) y1 = y0+1;
        if (y1 > h) y1 = h;
        std::memset(acc, 0, sizeof(uint32_t)*8*vw);
        uint32_t pcnt[8] = {0};
        for (uint32_t yy = y0; yy < y1; ++yy) {
            for (uint32_t p = 0; p < npl; ++p) {
                const uint32_t stored_row = impl_->interleaved ? yy
                    : (impl_->sequential_channels
                        ? p * impl_->h_px + yy
                        : (impl_->planes > 1 ? p * pxH + yy : yy));
                if (stored_row >= avail) continue;
                pcnt[p]++;
                const uint8_t* rowp = impl_->page + size_t(stored_row) * capb;
                for (uint32_t sx = 0; sx < vw; ++sx) {
                    const uint32_t x0 = uint32_t(uint64_t(sx) * w / vw);
                    uint32_t x1 = uint32_t(uint64_t(sx + 1) * w / vw);
                    if (x1 <= x0) x1 = x0 + 1;
                    if (x1 > w) x1 = w;
                    uint32_t sum = 0;
                    for (uint32_t x = x0; x < x1; ++x) {
                        const uint32_t display_plane = p;
                        const uint32_t bit_offset = impl_->interleaved
                            ? x * impl_->packed_pixel_bits + display_plane * impl_->bits
                            : x * impl_->bits;
                        const uint32_t row_offset = bit_offset >> 3;
                        const uint32_t shift = 8 - impl_->bits - (bit_offset & 7);
                        const uint32_t mask = (1u << impl_->bits) - 1u;
                        const uint32_t level = (rowp[row_offset] >> shift) & mask;
                        sum += level * kBitCoverage / mask;
                    }
                    acc[p][sx] += sum / (x1 - x0);
                }
            }
        }
        for (uint32_t sx = 0; sx < kLcdW; ++sx) {
            uint16_t color = kWhite565;
            if (sx>=ox && sx<ox+vw) {
                uint32_t vx = sx-ox;
                uint8_t cv[8] = {0};
                for (uint32_t p = 0; p < npl; ++p)
                    if (pcnt[p]) cv[p] = uint8_t(acc[p][vx]/pcnt[p]);
                uint32_t c = 0, m = 0, y = 0, k = 0, other = 0;
                bool has_cmyk = false;
                for (uint32_t p = 0; p < npl; ++p) {
                    RasterChannel channel;
                    if (p < impl_->channel_order_count) {
                        channel = impl_->channel_order[p];
                    } else if (npl == 4) {
                        channel = p == 0 ? RasterChannel::Cyan
                                 : p == 1 ? RasterChannel::Magenta
                                 : p == 2 ? RasterChannel::Yellow
                                           : RasterChannel::Black;
                    } else if (npl == 3) {
                        channel = p == 0 ? RasterChannel::Cyan
                                 : p == 1 ? RasterChannel::Magenta
                                           : RasterChannel::Yellow;
                    } else {
                        channel = RasterChannel::Black;
                    }
                    uint32_t coverage = cv[p];
                    switch (channel) {
                        case RasterChannel::Cyan:
                            c = c > coverage ? c : coverage; has_cmyk = true; break;
                        case RasterChannel::Magenta:
                            m = m > coverage ? m : coverage; has_cmyk = true; break;
                        case RasterChannel::Yellow:
                            y = y > coverage ? y : coverage; has_cmyk = true; break;
                        case RasterChannel::Black:
                        case RasterChannel::Gray:
                            k = k > coverage ? k : coverage; has_cmyk = true; break;
                        case RasterChannel::LightCyan:
                            coverage = coverage * 127 / 255;
                            c = c > coverage ? c : coverage; has_cmyk = true; break;
                        case RasterChannel::LightMagenta:
                            coverage = coverage * 127 / 255;
                            m = m > coverage ? m : coverage; has_cmyk = true; break;
                        case RasterChannel::LightGray:
                            coverage = coverage * 127 / 255;
                            k = k > coverage ? k : coverage; has_cmyk = true; break;
                        default:
                            other += coverage;
                            break;
                    }
                }
                uint32_t r = 255, g = 255, b = 255;
                if (has_cmyk) {
                    r = r * (255 - c) / 255;
                    g = g * (255 - m) / 255;
                    b = b * (255 - y) / 255;
                    r = r * (255 - k) / 255;
                    g = g * (255 - k) / 255;
                    b = b * (255 - k) / 255;
                } else {
                    const uint32_t ink = npl == 0 ? 0 : (other / npl);
                    r = g = b = 255 - (ink > 255 ? 255 : ink);
                }
                color = rgb565_wire(srgb_lut[r], srgb_lut[g], srgb_lut[b]);
            }
            line[sx] = color;
        }
    }
}

void DisplayService::flush_task_(void* arg) noexcept {
    auto* self = static_cast<DisplayService*>(arg);
    while (true) {
        if (self->started_ && self->impl_) {
            if ((self->impl_->page || self->impl_->preview_acc) &&
                (self->impl_->job_active || self->impl_->final_render_pending)) {
                self->render_incremental_();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(kFlushPeriodMs));
    }
}

}  // namespace ijpd
