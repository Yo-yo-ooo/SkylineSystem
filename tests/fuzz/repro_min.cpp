// tests/fuzz/repro_min.cpp — 崩溃格式最小复现 (round 93 调查)
#include <cstdio>
#include <cstdint>
#include <cstdarg>
extern "C" int32_t vsnprintf_(char* buffer, size_t count,
                              const char* format, va_list va);
/* 桩由 fuzz_shim.cpp 提供 (本文件不再重复定义) */

static int wrap(char* buf, size_t n, const char* fmt, ...) {
    va_list v; va_start(v, fmt);
    int r = vsnprintf_(buf, n, fmt, v);
    va_end(v);
    return r;
}

int main() {
    char buf[2048];
    const char* fmt = "%llu%%-8d%.0s%lu\n%lu%%o%p";
    /* 与 fuzz 相同的槽位序列 */
    int r = wrap(buf, sizeof(buf), fmt,
                 (uint64_t)(uintptr_t)"fuzz", 0x123456789abcdef0ULL,
                 (uint64_t)(uintptr_t)"fuzz", 0xdeadbeefcafebabeULL,
                 (uint64_t)(uintptr_t)"fuzz", (uint64_t)0,
                 (uint64_t)(uintptr_t)"fuzz", (uint64_t)0,
                 (uint64_t)(uintptr_t)"fuzz", (uint64_t)0,
                 (uint64_t)(uintptr_t)"fuzz", (uint64_t)0);
    printf("done r=%d [%.*s]\n", r, r > 0 ? r : 0, buf);
    return 0;
}
