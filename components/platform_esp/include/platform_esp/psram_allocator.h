// ============================================================================
// psram_allocator.h —— PSRAM 一次性分配器（Phase 5）
//
// 启动期一次性分配槽位池与压缩缓冲（运行期零动态分配）；运行期只读探测
// 容量/余量（heap_caps）。PSRAM 不可用：分配失败 -> psram_available=false，
// 打印能力保持关闭（协议 D-13：默认拒绝打印任务）。
//
// 线程安全性：init/deinit 单线程（启动）；refresh 只读原子量（heap_caps
//             内部加锁）；slots/comp 指针初始化后只读。
// ============================================================================
#ifndef IJPD_PLATFORM_PSRAM_ALLOCATOR_H
#define IJPD_PLATFORM_PSRAM_ALLOCATOR_H

#include <cstddef>
#include <cstdint>

#include "core/config_limits.h"
#include "core/errors.h"

namespace ijpd {

struct PlatformMemory {
    uint8_t* slots = nullptr;                              // SLOT_COUNT * SLOT_SIZE
    uint8_t* comp[cfg::COMPRESSED_BUF_COUNT] = {nullptr};  // 压缩接收缓冲
    bool psram_available = false;
    uint32_t psram_total_bytes = 0;
    uint32_t psram_free_bytes = 0;

    // 启动期分配（单线程）；重复调用安全（先释放）
    Err init() noexcept;
    void deinit() noexcept;

    // 运行期刷新余量（get_device_info 用）
    void refresh() noexcept;
};

}  // namespace ijpd

#endif  // IJPD_PLATFORM_PSRAM_ALLOCATOR_H
