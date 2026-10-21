// display_service.cpp —— ILI9341 240x320 还原显示实现
#include "platform_esp/display_service.h"

#define DISPLAY_SELFTEST 0  // 布局自测(临时)
#define DISPLAY_PAGE_DUMP 0  // 整页 dump(诊断完成,已关闭)

#include <cstring>

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_ili9341.h"
#include "esp_heap_caps.h"
#include "hal/gpio_ll.h"

#include "platform_esp/log_adapter.h"
#include "platform_esp/task_layout.h"

namespace ijpd {
namespace {
constexpr const char* kTag = "disp";

// ---- LCDWIKI 2.8" SPI (ILI9341 240x320) 接线(ESP32-S3 侧 GPIO) ----
// 注意:GPIO 33-37 在带 Octal PSRAM 的模块上是 PSRAM 专用,不可占用;
// 选用 1-21 安全区段(避开 0/3/19/20 strapping 与 USB)。
constexpr int kPinMosi = 11;
constexpr int kPinMiso = 12;   // 可不接
constexpr int kPinSclk = 13;
constexpr int kPinCs = 10;
constexpr int kPinDc = 9;
constexpr int kPinRst = 14;
constexpr int kPinBl = 21;     // 背光(高有效)

constexpr int kLcdW = 240;
constexpr int kLcdH = 320;
constexpr int kSpiHost = 2;    // SPI2_HOST
constexpr unsigned kPclkMhz = 16;  // 面包板+杜邦线:40MHz 信号完整性不足,位错乱导致斜条纹

// 墨点语义(实测确认):0 = 着墨,1 = 空白(引擎 1-bit 半色调为反射率域)。
// 各通道覆盖率对照原图验证:C≈10% M≈50% Y≈49% K≈21%,与照片暖色主调吻合。
constexpr int kInkBitLevel = 0;
// 1-bit 着墨覆盖率
constexpr uint8_t kBitCoverage = 255;

constexpr int kBandLines = 40;   // 分带推屏:避免 esp_lcd 按整帧(150KB)占用内部 DMA RAM

// 分带推屏:每带 kBandLines 行(首帧即分带,内部 DMA 缓冲按小带宽预留)
inline void draw_frame_banded(esp_lcd_panel_handle_t panel, const uint16_t* fb) {
    for (int y = 0; y < kLcdH; y += kBandLines) {
        const int h = (kLcdH - y) < kBandLines ? (kLcdH - y) : kBandLines;
        esp_lcd_panel_draw_bitmap(panel, 0, y, kLcdW, y + h, fb + y * kLcdW);
    }
}

constexpr size_t kFlushStack = 4096;
constexpr uint32_t kFlushPeriodMs = 120;
constexpr uint32_t kBarH = 10;

// 线性 → sRGB 编码近似表(启动构建;index 为线性 0..255)
uint8_t srgb_lut[256];
void build_srgb_lut() {
    for (int i = 0; i < 256; ++i) {
        const double v = i / 255.0;
        const double s = v <= 0.0031308 ? 12.92 * v
                                        : 1.055 * __builtin_pow(v, 1.0 / 2.4) - 0.055;
        srgb_lut[i] = static_cast<uint8_t>(s * 255.0 + 0.5);
    }
}

inline constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return static_cast<uint16_t>((((r & 0xF8) >> 3) << 11) |
                                 (((g & 0xFC) >> 2) << 5) | (b >> 3));
}
constexpr uint16_t kWhite565 = rgb565(255, 255, 255);

// 取像素覆盖率 c ∈ [0,255](0=空白)
inline uint8_t coverage_at(const uint8_t* row, uint32_t x, uint32_t bits) {
    if (bits == 8) return row[x];
    const uint8_t byte = row[x >> 3];
    const uint8_t bit = (byte >> (7 - (x & 7))) & 1;
    return bit == kInkBitLevel ? kBitCoverage : 0;
}

}  // namespace

struct DisplayService::Impl {
    SemaphoreHandle_t page_lock = nullptr;  // 页缓存行写入 vs 渲染读取互斥
    esp_lcd_panel_io_handle_t io = nullptr;
    esp_lcd_panel_handle_t panel = nullptr;
    uint16_t* fb = nullptr;          // RGB565 帧缓存(PSRAM)

    // 当前还原任务
    bool job_active = false;
    uint32_t w_px = 0, h_px = 0, bits = 1, planes = 1, bpr = 0;
    uint8_t* page = nullptr;         // 页缓存:bpr * h_px * planes(PSRAM)
    volatile uint32_t rows_seen = 0; // 已落页行数(print 任务写)
    uint32_t total_rows = 0;
    uint32_t cap_bpr = 0, cap_rows = 0, cap_planes = 0;  // 页缓存容量(常驻不释放)
    uint32_t last_target = 0;      // 增量渲染游标(已渲染到的目标行)
    volatile bool final_rendered = false;  // 终稿(全色合成)是否已渲染
};

DisplayService& DisplayService::instance() noexcept {
    static DisplayService s;
    return s;
}

Err DisplayService::start() noexcept {
    if (started_) return Err::OK;
    build_srgb_lut();
    impl_ = new Impl();
    impl_->page_lock = xSemaphoreCreateMutex();
    if (impl_->page_lock == nullptr) return Err::OUT_OF_MEMORY;

    // 背光
    gpio_config_t bl = {};
    bl.pin_bit_mask = 1ULL << kPinBl;
    bl.mode = GPIO_MODE_OUTPUT;
    bl.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&bl);
    gpio_set_level(static_cast<gpio_num_t>(kPinBl), 1);

    spi_bus_config_t bus = {};
    (void)bus;
    bus.mosi_io_num = kPinMosi;
    bus.miso_io_num = kPinMiso;
    bus.sclk_io_num = kPinSclk;
    bus.quadwp_io_num = -1;
    bus.max_transfer_sz = kLcdW * 64 * 2;
    const esp_err_t bus_err = spi_bus_initialize(
        static_cast<spi_host_device_t>(kSpiHost), &bus, SPI_DMA_CH_AUTO);
    IJPD_LOGI(kTag, "spi bus init: %s", esp_err_to_name(bus_err));
    if (bus_err != ESP_OK) {
        return Err::INTERNAL_ERROR;
    }

    esp_lcd_panel_io_spi_config_t io_cfg = {};
    io_cfg.cs_gpio_num = kPinCs;
    io_cfg.dc_gpio_num = kPinDc;
    io_cfg.spi_mode = 0;
    io_cfg.pclk_hz = kPclkMhz * 1000 * 1000;
    io_cfg.trans_queue_depth = 10;
    io_cfg.lcd_cmd_bits = 8;    // ILI9341:命令 8-bit
    io_cfg.lcd_param_bits = 8;  // 参数 8-bit(缺省 0 会导致零长度 SPI 事务被拒)
    io_cfg.on_color_trans_done = nullptr;
    const esp_err_t io_err = esp_lcd_new_panel_io_spi(
        static_cast<esp_lcd_spi_bus_handle_t>(kSpiHost), &io_cfg, &impl_->io);
    IJPD_LOGI(kTag, "panel io: %s", esp_err_to_name(io_err));
    if (io_err != ESP_OK) {
        return Err::INTERNAL_ERROR;
    }
    // 控制器 ID 识别:裸 SPI 半双工读(带 dummy 时钟)。
    // 读 MISO 需要独立 spi_device;读完即移除,再交给 esp_lcd。
    {
        spi_device_interface_config_t dev = {};
        dev.clock_speed_hz = 1 * 1000 * 1000;  // 低速可靠读
        dev.mode = 0;
        dev.spics_io_num = kPinCs;
        dev.queue_size = 1;
        dev.flags = SPI_DEVICE_HALFDUPLEX;
        spi_device_handle_t id_dev = nullptr;
        if (spi_bus_add_device(static_cast<spi_host_device_t>(kSpiHost), &dev,
                               &id_dev) == ESP_OK) {
            IJPD_LOGI(kTag, "build v8: 6-read id probe");
            const uint8_t cmds[6] = {0x04, 0x09, 0x0A, 0xD3, 0xDA, 0xDB};
            for (int ci = 0; ci < 6; ++ci) {
                spi_transaction_ext_t t = {};
                t.base.flags = SPI_TRANS_USE_RXDATA;  // 半双工读:命令走 cmd 相,禁止同时挂 TX 缓冲
                t.base.cmd = cmds[ci];
                t.command_bits = 8;
                t.dummy_bits = 8;
                t.base.rxlength = 4 * 8;
                const esp_err_t e = spi_device_polling_transmit(
                    id_dev, reinterpret_cast<spi_transaction_t*>(&t));
                IJPD_LOGI(kTag, "ID(0x%02x): %s -> %02x %02x %02x %02x", cmds[ci],
                          esp_err_to_name(e), t.base.rx_data[0], t.base.rx_data[1],
                          t.base.rx_data[2], t.base.rx_data[3]);
            }
            spi_bus_remove_device(id_dev);
        }
    }

    esp_lcd_panel_dev_config_t dev = {};
    dev.reset_gpio_num = kPinRst;
    dev.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR;
    dev.bits_per_pixel = 16;
    const esp_err_t panel_err = esp_lcd_new_panel_ili9341(impl_->io, &dev,
                                                          &impl_->panel);
    IJPD_LOGI(kTag, "panel create: %s", esp_err_to_name(panel_err));
    if (panel_err != ESP_OK) {
        return Err::INTERNAL_ERROR;
    }
    const esp_err_t rst_err = esp_lcd_panel_reset(impl_->panel);
    const esp_err_t init_err = esp_lcd_panel_init(impl_->panel);
    IJPD_LOGI(kTag, "panel reset: %s, init: %s",
              esp_err_to_name(rst_err), esp_err_to_name(init_err));
    IJPD_LOGI(kTag, "mirror: %s, disp_on: %s",
              esp_err_to_name(esp_lcd_panel_mirror(impl_->panel, false, false)),
              esp_err_to_name(esp_lcd_panel_disp_on_off(impl_->panel, true)));

    impl_->fb = static_cast<uint16_t*>(
        heap_caps_malloc(kLcdW * kLcdH * 2, MALLOC_CAP_SPIRAM));
    if (impl_->fb == nullptr) {
        IJPD_LOGE(kTag, "fb alloc failed");
        return Err::OUT_OF_MEMORY;
    }
#if DISPLAY_CALIBRATION
    // 色彩标定:六条竖带 R/G/B/白/黑/灰点 —— 用户报告所见颜色即可推定
    // 面板 RGB/BGR 顺序与墨点位义。
    const uint16_t bands[6] = {
        rgb565(255, 0, 0), rgb565(0, 255, 0), rgb565(0, 0, 255),
        kWhite565, kBlack565,
        rgb565(128, 128, 128),
    };
    for (int y = 0; y < kLcdH; ++y) {
        for (int x = 0; x < kLcdW; ++x) {
            const int band = x / (kLcdW / 6);
            uint16_t c = bands[band];
            if (band == 5) {
                c = (((x + y) & 1) == 0) ? kWhite565 : kBlack565;  // 50% 棋盘灰
            }
            impl_->fb[y * kLcdW + x] = c;
        }
    }
#else
    // 待机画面:对角斜纹
    for (int y = 0; y < kLcdH; ++y) {
        for (int x = 0; x < kLcdW; ++x) {
            impl_->fb[y * kLcdW + x] =
                (((x + y) / 24) & 1) ? rgb565(230, 234, 240) : rgb565(206, 213, 224);
        }
    }
#endif
    draw_frame_banded(impl_->panel, impl_->fb);
    IJPD_LOGI(kTag, "first draw done");

    if (task_layout::spawn("disp", flush_task_, this, 1,
                           task_layout::kPrioReport, kFlushStack) != Err::OK) {
        return Err::OUT_OF_MEMORY;
    }
    started_ = true;

#if DISPLAY_SELFTEST
    // 布局自测:确定性图案(i*7+13)喂满 310x3508x4 页缓存,
    // end_job 打印 hexdump 与 Mac 侧理论值逐字节对照。
    if (begin_job(2480, 3508, 1, 4, 310) == Err::OK) {
        uint32_t idx = 0;
        uint8_t row[310];
        for (uint32_t r = 0; r < 14032; ++r) {
            for (int i = 0; i < 310; ++i, ++idx) row[i] = (idx * 7 + 13) & 0xFF;
            feed_row(r, row, 310);
        }
        end_job();
        IJPD_LOGI(kTag, "selftest done");
    }
#endif
    IJPD_LOGI(kTag, "ili9341 %dx%d ready (mosi=%d sclk=%d cs=%d dc=%d rst=%d)",
              kLcdW, kLcdH, kPinMosi, kPinSclk, kPinCs, kPinDc, kPinRst);
    return Err::OK;
}

// 页缓存容量上限(常驻单次分配,跨任务复用,永不释放 → 无生命周期竞争):
// A4 300dpi 1-bit 四通道 = 310 x 3508 x 4 ≈ 4.35MB;上限放宽到 6.14MB。
constexpr uint32_t kPageCapBpr = 384;     // 3072px @1-bit
constexpr uint32_t kPageCapRows = 4000;   // 单平面行数
constexpr uint32_t kPageCapPlanes = 4;

Err DisplayService::begin_job(uint32_t width_px, uint32_t height_px, uint32_t bits,
                              uint32_t planes, uint32_t bytes_per_row) noexcept {
    if (!started_ || impl_ == nullptr) return Err::OK;  // 显示降级:不影响打印
    end_job();  // 结束上一任务(仅置 inactive,页保留)
    if (bits != 1 && bits != 8) bits = 1;
    if (planes == 0) planes = 1;
    // 超出常驻容量 → 本次显示降级(打印不受影响),绝不释放/重分配
    if (bytes_per_row > kPageCapBpr || height_px > kPageCapRows ||
        planes > kPageCapPlanes) {
        IJPD_LOGW(kTag, "job %lux%lux%lup超出显示缓存容量,本次不还原",
                  static_cast<unsigned long>(bytes_per_row),
                  static_cast<unsigned long>(height_px),
                  static_cast<unsigned long>(planes));
        return Err::OK;
    }
    if (impl_->page == nullptr) {
        impl_->page = static_cast<uint8_t*>(heap_caps_malloc(
            static_cast<size_t>(kPageCapBpr) * kPageCapRows * kPageCapPlanes,
            MALLOC_CAP_SPIRAM));
        if (impl_->page == nullptr) {
            IJPD_LOGW(kTag, "page cache alloc failed - display disabled");
            return Err::OUT_OF_MEMORY;
        }
        impl_->cap_bpr = kPageCapBpr;
        impl_->cap_rows = kPageCapRows;
        impl_->cap_planes = kPageCapPlanes;
    }
    // 行间空隙(容量 bpr 与任务 bpr 不同时)填白,避免残留旧数据
    if (bytes_per_row < impl_->cap_bpr) {
        for (uint32_t r = 0; r < height_px * planes; ++r) {
            std::memset(impl_->page + static_cast<size_t>(r) * impl_->cap_bpr,
                        0xFF, bytes_per_row);
            std::memset(impl_->page + static_cast<size_t>(r) * impl_->cap_bpr +
                            bytes_per_row,
                        0x00, impl_->cap_bpr - bytes_per_row);
        }
    }
    impl_->w_px = width_px;
    impl_->h_px = height_px;
    impl_->bits = bits;
    impl_->planes = planes;
    impl_->bpr = bytes_per_row;
    impl_->rows_seen = 0;
    impl_->total_rows = height_px * planes;
    impl_->last_target = 0;
    impl_->final_rendered = false;
    impl_->job_active = true;
    IJPD_LOGI(kTag, "display job: %lux%lupx %lubit x%luplane",
              static_cast<unsigned long>(width_px),
              static_cast<unsigned long>(height_px),
              static_cast<unsigned long>(bits),
              static_cast<unsigned long>(planes));
    return Err::OK;
}

void DisplayService::feed_row(uint32_t row_index, const uint8_t* data,
                              size_t len) noexcept {
    if (!started_ || impl_ == nullptr || !impl_->job_active || impl_->page == nullptr) {
        return;
    }
    if (row_index >= impl_->total_rows) return;
    const size_t off = static_cast<size_t>(row_index) * impl_->cap_bpr;
    const size_t n = len < impl_->bpr ? len : impl_->bpr;
    if (impl_->page_lock != nullptr) xSemaphoreTake(impl_->page_lock, portMAX_DELAY);
    std::memcpy(impl_->page + off, data, n);
    impl_->rows_seen = row_index + 1;
    if (impl_->page_lock != nullptr) xSemaphoreGive(impl_->page_lock);
}

const uint8_t* DisplayService::peek_row(uint32_t row_index) const noexcept {
    if (impl_ == nullptr || impl_->page == nullptr) return nullptr;
    if (row_index >= impl_->total_rows) return nullptr;
    return impl_->page + static_cast<size_t>(row_index) * impl_->cap_bpr;
}

void DisplayService::end_job() noexcept {
    if (impl_ == nullptr) return;
    // 仅置 inactive:页缓存常驻保留(容量内跨任务复用,永不释放),
    // 屏幕定格最后一帧;下一次 begin_job 复用同一缓存,零分配零竞争。
    impl_->job_active = false;
}

// 增量灰度渲染(打印中):只渲染新到达的行(平面 0 覆盖率 → 灰度),
// 每次仅处理少量行,PSRAM 带宽占用可忽略,不打扰打印流水线。
// 返回本次渲染到的目标行(供分带推屏);无新行返回 last_target。
uint32_t DisplayService::render_progressive_() noexcept {
    if (impl_ == nullptr || impl_->page == nullptr) return 0;
    const uint32_t w = impl_->w_px, h = impl_->h_px;
    const uint32_t bpr = impl_->bpr, capb = impl_->cap_bpr;
    const uint32_t rows_avail =
        impl_->rows_seen < impl_->total_rows ? impl_->rows_seen : impl_->total_rows;
    if (rows_avail == 0 || w == 0 || h == 0) return impl_->last_target;

    // 视口(与终稿一致的等比适配)
    uint32_t viewW, viewH;
    if (static_cast<uint64_t>(w) * kLcdH >= static_cast<uint64_t>(h) * kLcdW) {
        viewW = kLcdW;
        viewH = static_cast<uint32_t>(static_cast<uint64_t>(h) * kLcdW / w);
    } else {
        viewH = kLcdH;
        viewW = static_cast<uint32_t>(static_cast<uint64_t>(w) * kLcdH / h);
    }
    const uint32_t ox = (kLcdW - viewW) / 2;
    const uint32_t oy = (kLcdH - viewH) / 2;

    // 本次可完整覆盖的目标行区间
    uint32_t max_target = 0;
    for (uint32_t sy = 0; sy < viewH; ++sy) {
        const uint32_t y1 = static_cast<uint32_t>(
            static_cast<uint64_t>(sy + 1) * h / viewH);
        if (y1 > rows_avail) break;
        max_target = sy + 1;
    }
    static uint32_t acc1[kLcdW];
    for (uint32_t sy = impl_->last_target; sy < max_target; ++sy) {
        const uint32_t y0 = static_cast<uint32_t>(
            static_cast<uint64_t>(sy) * h / viewH);
        uint32_t y1 = static_cast<uint32_t>(
            static_cast<uint64_t>(sy + 1) * h / viewH);
        if (y1 <= y0) y1 = y0 + 1;
        std::memset(acc1, 0, sizeof(uint32_t) * viewW);
        uint32_t rcnt = 0;
        for (uint32_t yy = y0; yy < y1; yy += 2) {  // 隔行采样
            rcnt++;
            const uint8_t* rowp = impl_->page + static_cast<size_t>(yy) * capb;
            uint32_t x = 0;
            for (uint32_t sx = 0; sx < viewW; ++sx) {
                uint32_t x1 = static_cast<uint32_t>(
                    static_cast<uint64_t>(sx + 1) * w / viewW);
                if (x1 <= x) x1 = x + 1;
                if (x1 > w) x1 = w;
                for (; x < x1;) {
                    const uint32_t byte_idx = x >> 3;
                    const uint32_t bit_off = x & 7;
                    const uint32_t take = (8 - bit_off) < (x1 - x)
                                              ? (8 - bit_off) : (x1 - x);
                    const uint8_t byte = rowp[byte_idx];
                    const uint8_t mask = static_cast<uint8_t>(
                        ((0xFF << (8 - take)) & 0xFF) >> bit_off);
                    // 位义:0=墨:墨点数按比例折算覆盖率
                    const uint32_t zeros = take - __builtin_popcount(byte & mask);
                    acc1[sx] += zeros * 255 / take;
                    x += take;
                }
            }
        }
        // 平面 0 写行(灰度)
        uint16_t* line = impl_->fb + (oy + sy) * kLcdW;
        for (uint32_t sx = 0; sx < kLcdW; ++sx) {
            uint16_t color = kWhite565;
            if (sx >= ox && sx < ox + viewW) {
                const uint32_t vx = sx - ox;
                const uint32_t cov = acc1[vx] / (rcnt ? rcnt : 1);
                const uint8_t g = static_cast<uint8_t>(255 - cov);
                color = rgb565(srgb_lut[g], srgb_lut[g], srgb_lut[g]);
            }
            line[sx] = color;
        }
    }
    impl_->last_target = max_target;
    return max_target;
}

// 页缓存 → 帧缓存:区域平均降采样 + 四通道减色合成(终稿,任务结束后一次)。
void DisplayService::render_full_() noexcept {
    if (impl_ == nullptr) return;
    // 页指针仅由本任务(刷屏)分配/释放,渲染期间不会被释放;
    // 与 feed_row 的行写入竞争由行级锁覆盖。任务中止则立即退出渲染。
    if (impl_->page == nullptr || !impl_->job_active) return;
    const uint32_t w = impl_->w_px, h = impl_->h_px;
    const uint32_t planes = impl_->planes, bits = impl_->bits;
    const uint32_t bpr = impl_->bpr;
    const uint32_t rows_seen = impl_->rows_seen;
    uint16_t* fb = impl_->fb;

    // 视口:等比适配并居中
    uint32_t viewW, viewH;
    if (static_cast<uint64_t>(w) * kLcdH >= static_cast<uint64_t>(h) * kLcdW) {
        viewW = kLcdW;
        viewH = static_cast<uint32_t>(static_cast<uint64_t>(h) * kLcdW / w);
    } else {
        viewH = kLcdH;
        viewW = static_cast<uint32_t>(static_cast<uint64_t>(w) * kLcdH / h);
    }
    const uint32_t ox = (kLcdW - viewW) / 2;
    const uint32_t oy = (kLcdH - viewH) / 2;

    const uint32_t pxH = h;  // 单平面像素高
    const uint32_t npl = planes > 8 ? 8 : planes;
    // 已到达行数钳位:未到达行一律按空白渲染(页缓存未初始化区域是垃圾数据)
    const uint32_t total_rows = impl_->total_rows;
    const uint32_t rows_avail = rows_seen < total_rows ? rows_seen : total_rows;

    // 区域平均缩放:每个目标像素取源区域内各平面平均覆盖率。
    // 抖动点阵经平均后还原为连续灰度/色彩层次,预览观感接近原照片。
    // 逐目标行处理:行带内源行按字节累加到列桶,避免随机访问 PSRAM。
    // 多平面按各自到达进度独立合成(打印中可见通道逐层上色)。
    static uint32_t acc[8][kLcdW];
    for (uint32_t sy = 0; sy < kLcdH; ++sy) {
        uint16_t* line = fb + sy * kLcdW;
        if (sy < oy || sy >= oy + viewH || rows_avail == 0) {
            for (uint32_t sx = 0; sx < kLcdW; ++sx) line[sx] = kWhite565;
            continue;
        }
        const uint32_t y0 = static_cast<uint32_t>(
            static_cast<uint64_t>(sy - oy) * h / viewH);
        uint32_t y1 = static_cast<uint32_t>(
            static_cast<uint64_t>(sy + 1 - oy) * h / viewH);
        if (y1 <= y0) y1 = y0 + 1;
        if (y1 > h) y1 = h;
        std::memset(acc, 0, sizeof(uint32_t) * 8 * viewW);
        uint32_t pcnt[8] = {0};

        for (uint32_t yy = y0; yy < y1; yy += 2) {  // 隔行采样:成本减半,平均覆盖率几乎不变
            for (uint32_t p = 0; p < npl; ++p) {
                // 平面 p 的行 yy 已到达才计入(行全局顺序 = 平面顺序)
                if (p * pxH + yy >= rows_avail) continue;
                if (impl_->page_lock != nullptr) {
                    xSemaphoreTake(impl_->page_lock, portMAX_DELAY);
                }
                pcnt[p]++;
                const uint8_t* rowp = impl_->page +
                    static_cast<size_t>(p) * pxH * impl_->cap_bpr + yy * impl_->cap_bpr;
                uint32_t x = 0;
                for (uint32_t sx = 0; sx < viewW; ++sx) {
                    uint32_t x1 = static_cast<uint32_t>(
                        static_cast<uint64_t>(sx + 1) * w / viewW);
                    if (x1 <= x) x1 = x + 1;
                    if (x1 > w) x1 = w;
                    if (bits == 8) {
                        uint32_t sum = 0;
                        for (; x < x1; ++x) sum += rowp[x];
                        acc[p][sx] += sum / (x1 - x);
                    } else {
                        // 1-bit:按字节 popcount 加速(MSB-first)
                        for (; x < x1;) {
                            const uint32_t byte_idx = x >> 3;
                            const uint32_t bit_off = x & 7;
                            const uint32_t take = (8 - bit_off) < (x1 - x)
                                                      ? (8 - bit_off) : (x1 - x);
                            const uint8_t byte = rowp[byte_idx];
                            if (take == 8) {
                                acc[p][sx] += static_cast<uint32_t>(
                                    __builtin_popcount(byte) * 255) / 8;
                                x += 8;
                            } else {
                                const uint8_t mask = static_cast<uint8_t>(
                                    (0xFF << (8 - take)) >> bit_off);
                                const uint32_t ones =
                                    __builtin_popcount(byte & mask);
                                acc[p][sx] += ones ? kBitCoverage : 0u;
                                x += take;
                            }
                        }
                    }
                }
                if (impl_->page_lock != nullptr) {
                    xSemaphoreGive(impl_->page_lock);
                }
            }
        }
        // 合成写行(各平面独立计数,未到达平面按空白)
        for (uint32_t sx = 0; sx < kLcdW; ++sx) {
            uint16_t color = kWhite565;
            if (sx >= ox && sx < ox + viewW) {
                const uint32_t vx = sx - ox;
                uint8_t c[8] = {0};
                for (uint32_t p = 0; p < npl; ++p) {
                    if (pcnt[p] != 0) {
                        c[p] = static_cast<uint8_t>(acc[p][vx] / pcnt[p]);
                    }
                }
                if (npl == 1) {
                    const uint8_t g = 255 - c[0];
                    color = rgb565(g, g, g);
                } else if (npl == 4) {
                    const uint8_t k = c[3];
                    const uint8_t r = static_cast<uint8_t>(
                        255u - (static_cast<uint16_t>(c[0]) * (255 - k)) / 255u);
                    const uint8_t g = static_cast<uint8_t>(
                        255u - (static_cast<uint16_t>(c[1]) * (255 - k)) / 255u);
                    const uint8_t b = static_cast<uint8_t>(
                        255u - (static_cast<uint16_t>(c[2]) * (255 - k)) / 255u);
                    color = rgb565(r, g, b);
                } else if (npl == 3) {
                    color = rgb565(255 - c[0], 255 - c[1], 255 - c[2]);
                } else {
                    uint32_t sum = 0;
                    for (uint32_t p = 0; p < npl; ++p) sum += c[p];
                    const uint8_t g = 255 - static_cast<uint8_t>(sum / npl);
                    color = rgb565(g, g, g);
                }
            }
            line[sx] = color;
        }
    }
    // 底部进度条
    const uint32_t fill = impl_->total_rows
                              ? kLcdW * rows_seen / impl_->total_rows
                              : 0;
    for (uint32_t sy = kLcdH - kBarH; sy < kLcdH; ++sy) {
        for (uint32_t sx = 0; sx < kLcdW; ++sx) {
            fb[sy * kLcdW + sx] =
                (sx < fill) ? rgb565(30, 160, 90) : rgb565(216, 220, 228);
        }
    }
    if (impl_->page_lock != nullptr) xSemaphoreGive(impl_->page_lock);
}

void DisplayService::flush_task_(void* arg) noexcept {
    auto* self = static_cast<DisplayService*>(arg);
    while (true) {
        if (self->started_ && self->impl_ != nullptr &&
            self->impl_->page != nullptr) {
            if (self->impl_->job_active) {
                // 打印中:增量灰度渲染(仅新到的行),开销微小,不与流水线抢带宽
                if (self->render_progressive_() > 0) {
                    esp_lcd_panel_draw_bitmap(self->impl_->panel, 0, 0, kLcdW,
                                              kLcdH, self->impl_->fb);
                }
            } else if (!self->impl_->final_rendered) {
                // 任务结束:流水线已停,做一次完整四色合成(终稿)
                self->render_full_();
                draw_frame_banded(self->impl_->panel, self->impl_->fb);
                self->impl_->final_rendered = true;
                IJPD_LOGI(kTag, "final composite rendered");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(kFlushPeriodMs));
    }
}

}  // namespace ijpd
