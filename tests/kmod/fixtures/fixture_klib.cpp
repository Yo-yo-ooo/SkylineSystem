// tests/kmod/fixtures/fixture_klib.cpp — 宿主测试 fixture：Tier0 klib 直接符号共享
// ----------------------------------------------------------------------------
// 验证：.ko 代码直接调用 klib 的 C 链接函数（strlen/strcmp/snprintf_）时，
// 这些引用是普通 ELF 未定义符号；加载器用内核导出表（.kout 的宿主替身，
// 由 kernel_exports.cpp + kernel_services.cpp 组成）按名字解析。
// 不需要 .in 导入声明 —— 这正是"klib 直接符号共享给每个模块"的路径。
// ----------------------------------------------------------------------------
#include <kmod/kmod_module.h>

#include <stdint.h>
#include <stddef.h>

// 直接外部引用（C 链接 klib 符号）
extern "C" size_t strlen(const char *s);
extern "C" int32_t strcmp(const char *a, const char *b);
extern "C" int32_t snprintf_(char *buf, size_t n, const char *fmt, ...);

#define FIXTURE_KLIB_MAGIC 0x4B4C49425F5631ull

extern "C" uint64_t fixture_klib_check(void)
{
    if (strlen("kernel") != 6u)
        return 0u;
    if (strcmp("a", "a") != 0)
        return 0u;
    char buf[32];
    if (snprintf_(buf, sizeof(buf), "%s-%u", "klib", 7u) <= 0)
        return 0u;
    if (strcmp(buf, "klib-7") != 0)
        return 0u;
    return FIXTURE_KLIB_MAGIC;
}

static int32_t fixture_klib_init(void)
{
    return KMOD_OK;
}

static void fixture_klib_exit(void)
{
}

KMOD_EXPORT_FN(fixture_klib_check, "vendor.test.klib.v1", 1);

KMOD_MANIFEST("vendor.test.klib", "1.0.0", KMOD_TIER_IMPL, 0u,
              KMOD_CAP_LOG, fixture_klib_init, fixture_klib_exit);
