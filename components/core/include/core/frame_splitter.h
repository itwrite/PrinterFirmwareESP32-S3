// ============================================================================
// frame_splitter.h —— 以 '#' 为界的控制帧有界拆帧器（Phase 1）
//
// 协议行为（docs/protocols/ijpd-control.md）：
//   - 第一个 '#' 即帧结束；空帧（'#' 前无字节）静默忽略；
//   - 正文超过 MAX_CONTROL_FRAME_SIZE：丢弃至下一个 '#'，计为 too_large，
//     缓冲区绝不增长（调用方据 too_large 计数逐帧回 FRAME_TOO_LARGE，msg_id=null）；
//   - 半帧超时由调用方（平台层定时器）驱动：has_partial() + clear_partial()。
//
// 线程安全性：非线程安全。每个连接独占一个实例（BLE/TCP 各一）。
// 所有权：    帧内容以内部缓冲区切片指针返回，仅在下次 feed()/clear_partial()
//            调用之前有效；调用方必须在回调内处理或拷贝。
// 容量上限：  内部缓冲 MAX_CONTROL_FRAME_SIZE(2048) 字节，编译期固定。
// 最大耗时：  feed O(len)，无动态分配。
// ============================================================================
#ifndef IJPD_CORE_FRAME_SPLITTER_H
#define IJPD_CORE_FRAME_SPLITTER_H

#include <cstddef>
#include <cstdint>

#include "core/config_limits.h"
#include "core/errors.h"

namespace ijpd {

class FrameSplitter {
public:
    struct Stats {
        size_t frames;         // 本次 feed 产出的完整帧数（长度 >= 1）
        size_t empty_ignored;  // 静默忽略的空帧数
        size_t too_large;      // 超限被丢弃的帧数（每帧应回一个 FRAME_TOO_LARGE）
    };

    // 每个完整帧回调一次。frame 长度 >= 1（空帧已被过滤并计数）。
    using Callback = void (*)(void* user, const char* frame, size_t len);

    FrameSplitter() noexcept = default;

    // 不可拷贝（含 2KB 缓冲），可移动（无指针自引用）。
    FrameSplitter(const FrameSplitter&) = delete;
    FrameSplitter& operator=(const FrameSplitter&) = delete;

    // 投喂一批字节（可为任意切分：半包/粘包/随机）。字节中不含 '#' 语义之外的
    // 解释；'\u0023' 转义不会产生裸 '#'，因此字节级切分即协议级切分。
    Stats feed(const uint8_t* data, size_t len, Callback cb, void* user) noexcept;

    bool   has_partial() const noexcept { return len_ != 0; }
    size_t partial_len() const noexcept { return len_; }   // 恒 <= capacity()
    bool   is_overflowed() const noexcept { return overflow_; }
    void   clear_partial() noexcept { len_ = 0; overflow_ = false; }  // 超时丢弃半帧

    static constexpr size_t capacity() noexcept { return cfg::MAX_CONTROL_FRAME_SIZE; }

private:
    char buf_[cfg::MAX_CONTROL_FRAME_SIZE] = {};
    size_t len_ = 0;
    bool overflow_ = false;  // 积累字节已超上限（不再存储，等待 '#' 复位）
};

}  // namespace ijpd

#endif  // IJPD_CORE_FRAME_SPLITTER_H
