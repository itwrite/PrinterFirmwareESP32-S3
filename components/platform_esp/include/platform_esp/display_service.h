// ============================================================================
// display_service.h —— 2.8" SPI 屏(ILI9341 240x320)还原显示
//
// 打印时流式接收光栅行 → PSRAM 页缓存 → 后台任务周期性降采样渲染
// (最近邻缩放 + 通道合成) → esp_lcd 推屏,实现"边传边看"。
// 未打印时显示待机斜纹。
// ============================================================================
#ifndef IJPD_PLATFORM_DISPLAY_SERVICE_H
#define IJPD_PLATFORM_DISPLAY_SERVICE_H

#include <cstddef>
#include <cstdint>

#include "core/errors.h"

namespace ijpd {

class DisplayService {
public:
    static DisplayService& instance() noexcept;

    // 面板初始化 + 刷屏任务(上电调用一次;失败不致命,显示功能降级关闭)
    Err start() noexcept;

    // 开始一个还原任务:像素宽/高、位深(1|8)、平面数、每行字节数。
    // 页缓存按需分配(PSRAM);分配失败时返回错误且显示降级。
    Err begin_job(uint32_t width_px, uint32_t height_px, uint32_t bits,
                  uint32_t planes, uint32_t bytes_per_row) noexcept;

    // 打印行到达(单写者:print 任务)。非活动任务时为空操作。
    void feed_row(uint32_t row_index, const uint8_t* data, size_t len) noexcept;

    // 只读访问页缓存中的某行(页缓存常驻,任务期内有效);越界返回 nullptr。
    const uint8_t* peek_row(uint32_t row_index) const noexcept;

    // 任务终态:保留最后一帧(含完成进度条),等待下一个任务。
    void end_job() noexcept;

private:
    DisplayService() noexcept = default;
    static void flush_task_(void* arg) noexcept;
    void render_frame_() noexcept;

    struct Impl;
    Impl* impl_ = nullptr;
    volatile bool started_ = false;
};

}  // namespace ijpd

#endif  // IJPD_PLATFORM_DISPLAY_SERVICE_H
