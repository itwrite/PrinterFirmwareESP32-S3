// ============================================================================
// assert.h —— 主机侧极简自写断言（无第三方测试框架）
// 用法：测试 main() 末尾 return ::ijpd::test::summary("test_name");
// ============================================================================
#pragma once

#include <atomic>
#include <cstdio>
#include <cstdint>
#include <cstring>

namespace ijpd {
namespace test {

// 多线程压力测试中会从工作线程调用断言 -> 计数器为原子
inline std::atomic<int> g_passed{0};
inline std::atomic<int> g_failed{0};

inline void report(bool cond, const char* expr, const char* file, int line) {
    if (cond) {
        g_passed.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_failed.fetch_add(1, std::memory_order_relaxed);
        std::printf("FAIL %s:%d: %s\n", file, line, expr);
    }
}

inline void report_eq_i(long long a, long long b, const char* expr,
                        const char* file, int line) {
    if (a == b) {
        g_passed.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_failed.fetch_add(1, std::memory_order_relaxed);
        std::printf("FAIL %s:%d: %s -> got %lld, want %lld\n", file, line, expr, a, b);
    }
}

inline void report_eq_hex(unsigned long long a, unsigned long long b, const char* expr,
                          const char* file, int line) {
    if (a == b) {
        g_passed.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_failed.fetch_add(1, std::memory_order_relaxed);
        std::printf("FAIL %s:%d: %s -> got 0x%llX, want 0x%llX\n", file, line, expr, a, b);
    }
}

inline void report_eq_str(const char* a, const char* b, const char* expr,
                          const char* file, int line) {
    if (a != nullptr && b != nullptr && std::strcmp(a, b) == 0) {
        g_passed.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_failed.fetch_add(1, std::memory_order_relaxed);
        std::printf("FAIL %s:%d: %s -> got \"%s\", want \"%s\"\n", file, line, expr,
                    (a != nullptr) ? a : "(null)", (b != nullptr) ? b : "(null)");
    }
}

inline void report_eq_mem(const void* a, const void* b, size_t n, const char* expr,
                          const char* file, int line) {
    if (a != nullptr && b != nullptr && std::memcmp(a, b, n) == 0) {
        g_passed.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_failed.fetch_add(1, std::memory_order_relaxed);
        std::printf("FAIL %s:%d: %s -> memory mismatch (%zu bytes)\n", file, line, expr, n);
    }
}

inline int summary(const char* name) {
    std::printf("%s: %d passed, %d failed\n", name, g_passed.load(),
                g_failed.load());
    return (g_failed.load() == 0) ? 0 : 1;
}

}  // namespace test
}  // namespace ijpd

#define CHECK_TRUE(expr) \
    ::ijpd::test::report((expr), #expr, __FILE__, __LINE__)

#define CHECK_EQ(expr, want) \
    ::ijpd::test::report_eq_i(static_cast<long long>(expr), static_cast<long long>(want), \
                              #expr " == " #want, __FILE__, __LINE__)

#define CHECK_EQ_HEX(expr, want) \
    ::ijpd::test::report_eq_hex(static_cast<unsigned long long>(expr), \
                                static_cast<unsigned long long>(want), \
                                #expr " == " #want, __FILE__, __LINE__)

#define CHECK_EQ_STR(expr, want) \
    ::ijpd::test::report_eq_str((expr), (want), #expr " == " #want, __FILE__, __LINE__)

#define CHECK_EQ_MEM(expr, want, n) \
    ::ijpd::test::report_eq_mem((expr), (want), (n), #expr " == " #want, __FILE__, __LINE__)

// 编译期断言（失败即编译错误）
#define CHECK_STATIC(cond) static_assert(cond, #cond)
