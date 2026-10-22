// display_service.cpp —— ILI9341 240x320 还原显示实现(esp_lcd,页缓存直通)
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
constexpr unsigned kPclkHz = 5 * 1000 * 1000;  // 10MHz 面包板安全

constexpr int kBandLines = 20;
constexpr size_t kFlushStack = 3072;  // 渲染循环栈占用小,从 4096 缩减给 ctl_tcp
constexpr uint32_t kFlushPeriodMs = 120;
constexpr uint32_t kBarH = 10;

// 墨点:0=着墨(实测确认)
constexpr int kInkBitLevel = 0;
constexpr uint8_t kBitCoverage = 255;

uint8_t srgb_lut[256];
void build_srgb_lut() {
    for (int i = 0; i < 256; ++i) {
        double v = i / 255.0;
        double s = v <= 0.0031308 ? 12.92 * v : 1.055 * __builtin_pow(v, 1.0/2.4) - 0.055;
        srgb_lut[i] = static_cast<uint8_t>(s * 255.0 + 0.5);
    }
}

inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return static_cast<uint16_t>((((r & 0xF8) >> 3) << 11) | (((g & 0xFC) >> 2) << 5) | (b >> 3));
}
const uint16_t kWhite565 = rgb565(255, 255, 255);

}  // namespace

struct DisplayService::Impl {
    SemaphoreHandle_t page_lock = nullptr;
    esp_lcd_panel_io_handle_t io = nullptr;
    esp_lcd_panel_handle_t panel = nullptr;
    uint16_t* fb = nullptr;
    bool job_active = false;
    uint32_t w_px = 0, h_px = 0, bits = 1, planes = 1, bpr = 0;
    uint8_t* page = nullptr;
    volatile uint32_t rows_seen = 0;
    uint32_t total_rows = 0;
    uint32_t cap_bpr = 0, cap_rows = 0, cap_planes = 0;
    uint32_t last_target = 0;
    bool final_rendered = false;
};

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
            impl_->fb[y * kLcdW + x] = (((x+y)/24)&1) ? rgb565(230,234,240) : rgb565(206,213,224);
    esp_lcd_panel_draw_bitmap(impl_->panel, 0, 0, kLcdW, kLcdH, impl_->fb);
    IJPD_LOGI(kTag, "first draw done");

    if (task_layout::spawn("disp", flush_task_, this, 1, task_layout::kPrioReport, kFlushStack) != Err::OK)
        return Err::OUT_OF_MEMORY;
    started_ = true;
    IJPD_LOGI(kTag, "display ready %dx%d", kLcdW, kLcdH);
    return Err::OK;
}

constexpr uint32_t kPageCapBpr = 384;
constexpr uint32_t kPageCapRows = 4000;
constexpr uint32_t kPageCapPlanes = 4;

Err DisplayService::begin_job(uint32_t w_px, uint32_t h_px, uint32_t bits,
                              uint32_t planes, uint32_t bpr) noexcept {
    if (!started_ || !impl_) return Err::OK;
    end_job();
    if (bits != 1 && bits != 8) bits = 1;
    if (!planes) planes = 1;
    if (bpr > kPageCapBpr || h_px > kPageCapRows || planes > kPageCapPlanes) {
        IJPD_LOGW(kTag, "job exceeds page cache, display skip");
        return Err::OK;
    }
    if (!impl_->page) {
        impl_->page = static_cast<uint8_t*>(heap_caps_malloc(
            size_t(kPageCapBpr) * kPageCapRows * kPageCapPlanes, MALLOC_CAP_SPIRAM));
        if (!impl_->page) { IJPD_LOGW(kTag, "page alloc failed"); return Err::OUT_OF_MEMORY; }
        impl_->cap_bpr = kPageCapBpr; impl_->cap_rows = kPageCapRows; impl_->cap_planes = kPageCapPlanes;
    }
    if (bpr < impl_->cap_bpr) {
        for (uint32_t r = 0; r < h_px * planes; ++r) {
            std::memset(impl_->page + size_t(r) * impl_->cap_bpr, 0xFF, bpr);
            std::memset(impl_->page + size_t(r) * impl_->cap_bpr + bpr, 0x00, impl_->cap_bpr - bpr);
        }
    }
    impl_->w_px = w_px; impl_->h_px = h_px; impl_->bits = bits;
    impl_->planes = planes; impl_->bpr = bpr;
    impl_->rows_seen = 0; impl_->total_rows = h_px * planes;
    impl_->last_target = 0; impl_->final_rendered = false;
    impl_->job_active = true;
    IJPD_LOGI(kTag, "job %lux%lu %lub %lup", static_cast<unsigned long>(w_px),
              static_cast<unsigned long>(h_px), static_cast<unsigned long>(bits),
              static_cast<unsigned long>(planes));
    return Err::OK;
}

void DisplayService::feed_row(uint32_t idx, const uint8_t* data, size_t len) noexcept {
    if (!started_ || !impl_ || !impl_->job_active || !impl_->page) return;
    if (idx >= impl_->total_rows) return;
    size_t off = size_t(idx) * impl_->cap_bpr;
    size_t n = len < impl_->bpr ? len : impl_->bpr;
    // 无锁写:310B memcpy 亚微秒;rows_seen 32位对齐原子写。
    // FreeRTOS 互斥锁跨核使用时优先级继承簿记可触发
    // xTaskPriorityDisinherit 断言 → 整机崩溃(实测复现),不可接受。
    // 撕裂读对预览显示无视觉影响,数据完整性由逐包 CRC 保证。
    std::memcpy(impl_->page + off, data, n);
    impl_->rows_seen = idx + 1;
}

const uint8_t* DisplayService::peek_row(uint32_t idx) const noexcept {
    if (!impl_ || !impl_->page) return nullptr;
    if (idx >= impl_->total_rows) return nullptr;
    return impl_->page + size_t(idx) * impl_->cap_bpr;
}

void DisplayService::end_job() noexcept {
    if (!impl_) return;
    impl_->job_active = false;
}

// ---- 渲染 ----

void DisplayService::render_frame_() noexcept {
    // 预览渲染:不持长锁(锁会阻塞 rx_task 的 feed_row,导致 TCP 背压)
    // 偶尔读到半写行对预览无视觉影响;数据完整性由 CRC 保证
    if (!impl_ || !impl_->page || !impl_->job_active) return;
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
                if (p*pxH+yy >= avail) continue;
                pcnt[p]++;
                const uint8_t* rowp = impl_->page + size_t(p)*pxH*capb + yy*capb;
                uint32_t x = 0;
                for (uint32_t sx = 0; sx < vw; ++sx) {
                    uint32_t x1 = uint32_t(uint64_t(sx+1)*w/vw);
                    if (x1<=x) x1=x+1;
                    if (x1>w) x1=w;
                    for (; x < x1;) {
                        uint32_t bi = x>>3, bo = x&7;
                        uint32_t tk = (8-bo)<(x1-x)?(8-bo):(x1-x);
                        uint8_t byte = rowp[bi];
                        if (tk==8) { acc[p][sx]+=uint32_t(__builtin_popcount(byte)*255)/8; x+=8; }
                        else {
                            uint8_t mask = uint8_t(((0xFF<<(8-tk))&0xFF)>>bo);
                            acc[p][sx]+=__builtin_popcount(byte&mask)?kBitCoverage:0;
                            x+=tk;
                        }
                    }
                }
            }
        }
        uint32_t cnt = y1-y0;
        for (uint32_t sx = 0; sx < kLcdW; ++sx) {
            uint16_t color = kWhite565;
            if (sx>=ox && sx<ox+vw) {
                uint32_t vx = sx-ox;
                uint8_t cv[8] = {0};
                for (uint32_t p = 0; p < npl; ++p)
                    if (pcnt[p]) cv[p] = uint8_t(acc[p][vx]/pcnt[p]);
                uint32_t r=255,g=255,b=255;
                if (npl==4) {
                    // C 减 R,M 减 G,Y 减 B,K 减全部(乘法透过率)
                    r=r*(255-cv[0])/255;
                    g=g*(255-cv[1])/255;
                    b=b*(255-cv[2])/255;
                    r=r*(255-cv[3])/255;
                    g=g*(255-cv[3])/255;
                    b=b*(255-cv[3])/255;
                } else if (npl==3) {
                    r=r*(255-cv[0])/255; g=g*(255-cv[1])/255; b=b*(255-cv[2])/255;
                } else {
                    uint32_t s=0; for(uint32_t p=0;p<npl;++p)s+=cv[p];
                    uint32_t ink=s/npl; r=255-ink;g=255-ink;b=255-ink;
                }
                color = rgb565(srgb_lut[r>255?255:r],srgb_lut[g>255?255:g],srgb_lut[b>255?255:b]);
            }
            line[sx] = color;
        }
    }
}

void DisplayService::flush_task_(void* arg) noexcept {
    auto* self = static_cast<DisplayService*>(arg);
    while (true) {
        if (self->started_ && self->impl_) {
            if (self->impl_->page && self->impl_->job_active) {
                self->render_frame_();
                esp_lcd_panel_draw_bitmap(self->impl_->panel, 0, 0, kLcdW, kLcdH, self->impl_->fb);
            }
        }
        const uint32_t period = self->impl_->job_active ? 500 : kFlushPeriodMs;
        vTaskDelay(pdMS_TO_TICKS(period));
    }
}

}  // namespace ijpd
