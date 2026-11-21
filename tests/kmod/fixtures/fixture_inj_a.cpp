// tests/kmod/fixtures/fixture_inj_a.cpp — DynModule 注入测试 provider
// ----------------------------------------------------------------------------
// 无任何未定义外部符号，可直接被 KmodElfLoadImage 加载；导出 inj_a_magic。
// manifest: vendor.test.inj.a / 1.0.0 / KMOD_TIER_DYN / UNLOADABLE。
// ----------------------------------------------------------------------------
#include <kmod/kmod_module.h>

#include <stdint.h>

#define INJ_A_MAGIC 0x494E4A41ull /* "INJA" */

static uint64_t inj_a_magic = INJ_A_MAGIC;

static int32_t inj_a_init(void)
{
    inj_a_magic = INJ_A_MAGIC;
    return KMOD_OK;
}

static void inj_a_exit(void)
{
}

KMOD_EXPORT_DATA(inj_a_magic, "vendor.test.inj.a.v1", 1u, sizeof(inj_a_magic));

KMOD_MANIFEST_FULL("vendor.test.inj.a", "1.0.0", KMOD_TIER_DYN,
                   (uint32_t)KMOD_MF_UNLOADABLE, 0u, 0,
                   "host injection fixture provider", inj_a_init, inj_a_exit,
                   0, 0, 0u, 0u);
