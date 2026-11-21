// tests/kmod/fixtures/fixture_inj_b.cpp — DynModule 注入测试 consumer
// ----------------------------------------------------------------------------
// 依赖 vendor.test.inj.a 的导出数据；被 KdllInject 注入时治理层必须按 .in
// 的 provider 自动拉起 a。init 校验 magic 并导出 inj_b_ok 供测试读回。
// ----------------------------------------------------------------------------
#include <kmod/kmod_module.h>

#include <stdint.h>

#define INJ_A_MAGIC 0x494E4A41ull
#define INJ_B_OK    0x494E4A42ull /* "INJB" */

KMOD_IMPORT(inj_a_magic, const uint64_t *, "vendor.test.inj.a", "inj_a_magic",
            "vendor.test.inj.a.v1");

static uint64_t inj_b_ok = 0u;

static int32_t inj_b_init(void)
{
    if (!inj_a_magic || *inj_a_magic != INJ_A_MAGIC)
        return KMOD_ERR_CONTRACT;
    inj_b_ok = INJ_B_OK;
    return KMOD_OK;
}

static void inj_b_exit(void)
{
    inj_b_ok = 0u;
}

KMOD_EXPORT_DATA(inj_b_ok, "vendor.test.inj.b.v1", 1u, sizeof(inj_b_ok));

KMOD_MANIFEST_FULL("vendor.test.inj.b", "1.0.0", KMOD_TIER_DYN,
                   (uint32_t)(KMOD_MF_UNLOADABLE | KMOD_MF_DEMAND), 0u, 0,
                   "host injection fixture consumer", inj_b_init, inj_b_exit,
                   0, 0, 0u, 0u);
