// SPDX-FileCopyrightText: 2026 Yo-yo-ooo
// SPDX-License-Identifier: MIT

// tests/kmod/inject_test.cpp — 统一 DynModule 注入层 (kdll) 宿主测试
// ----------------------------------------------------------------------------
// 覆盖：
//   1. KdllInit 登记 resident 核心服务；KdllInject 幂等 + PIN 引用计数；
//   2. 常驻服务 KdllEject 只释放 PIN、永不卸载；
//   3. KdllRegisterResident 动态登记新 resident；
//   4. KdllInjectImage 从 ET_REL 镜像注入，依赖 .in provider 自动拉起；
//   5. KdllResolve 做 GetProcAddress 式解析；
//   6. KdllInjectFile 在宿主无 VFS 时明确拒绝；
//   7. 未知名注入/未持有 PIN 的 eject 负向。
// ----------------------------------------------------------------------------
#include <kmod/kmod_internal.h>
#include <kmod/kdll.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "host_platform.h"
#include "test_common.h"

extern "C" {
extern kmod_module_t g_kmod_modules[KMOD_MAX_MODULES];
extern uint32_t g_kmod_module_count;
extern uint32_t g_kmod_image_count;
}

namespace {

const char *kInjA = "vendor.test.inj.a";
const char *kInjB = "vendor.test.inj.b";

int32_t state_of(const char *name)
{
    kmod_module_t *m = KmodModuleFind(name);
    return m ? (int32_t)__atomic_load_n(&m->state, __ATOMIC_ACQUIRE)
             : (int32_t)KMOD_ERR_NOTFOUND;
}

void reset_all()
{
    KmodCapInit();
    KmodCapSetMode(KMOD_POLICY_OPEN); /* fixture 不在 STRICT 可信清单里 */
    (void)KmodSeatInit();
    (void)KmodSyscallInit();
    (void)KmodGovInit();
    std::memset(g_kmod_modules, 0, sizeof(kmod_module_t) * KMOD_MAX_MODULES);
    g_kmod_module_count = 0u;
    g_kmod_image_count = 0u;
    std::memset(&g_kmod_stats, 0, sizeof(g_kmod_stats));
}

int32_t test_resident(void)
{
    KCHECK_EQ_U64((uint64_t)KdllResidentCount(), 4u);
    KCHECK_EQ((int32_t)KdllInject(KDLL_CORE_PMM, KMOD_INJ_PIN | KMOD_INJ_SILENT),
              (int32_t)KMOD_OK);
    KCHECK_EQ_U64((uint64_t)KdllPinCount(KDLL_CORE_PMM), 1u);
    KCHECK_EQ((int32_t)state_of(KDLL_CORE_PMM),
              (int32_t)KMOD_STATE_READY);

    /* 幂等注入：不带 PIN 不改变引用 */
    KCHECK_EQ((int32_t)KdllInject(KDLL_CORE_PMM, KMOD_INJ_SILENT),
              (int32_t)KMOD_OK);
    KCHECK_EQ_U64((uint64_t)KdllPinCount(KDLL_CORE_PMM), 1u);
    KCHECK(KdllResolve(KDLL_CORE_PMM, "anything") == nullptr);

    KCHECK_EQ((int32_t)KdllEject(KDLL_CORE_PMM), (int32_t)KMOD_OK);
    KCHECK_EQ_U64((uint64_t)KdllPinCount(KDLL_CORE_PMM), 0u);
    KCHECK_EQ((int32_t)state_of(KDLL_CORE_PMM),
              (int32_t)KMOD_STATE_READY);
    KCHECK_EQ((int32_t)KdllEject(KDLL_CORE_PMM), (int32_t)KMOD_ERR_STATE);

    KCHECK_EQ((int32_t)KdllRegisterResident("kernel.test.resident", "2.0.0",
                                            0u, "host resident"),
              (int32_t)KMOD_OK);
    KCHECK_EQ((int32_t)KdllInject("kernel.test.resident", KMOD_INJ_PIN),
              (int32_t)KMOD_OK);
    KCHECK_EQ((int32_t)KdllEject("kernel.test.resident"), (int32_t)KMOD_OK);
    KCHECK_EQ((int32_t)state_of("kernel.test.resident"),
              (int32_t)KMOD_STATE_READY);
    return 0;
}

int32_t test_image_inject(void)
{
    std::vector<uint8_t> a;
    std::vector<uint8_t> b;
    if (!kmod_read_fixture("fixture_inj_a.o", a) ||
        !kmod_read_fixture("fixture_inj_b.o", b)) {
        KT_INFO("injection fixtures not built, skip image case");
        return 0;
    }

    /* a 先入池但不加载；b 通过 KdllInjectImage 注入时，治理事务必须按
     * b 的 .in provider 自动拉起 a —— 单请求消费端、依赖自动注入。 */
    int32_t pool_a = KmodPoolAdd(a.data(), (uint64_t)a.size(), false);
    KCHECK_MSG(pool_a >= 0, "KmodPoolAdd(a) rc=%d", (int)pool_a);

    KCHECK_EQ((int32_t)KdllInjectImage(b.data(), (uint64_t)b.size(),
                                      KMOD_INJ_PIN | KMOD_INJ_SILENT),
              (int32_t)KMOD_OK);
    KCHECK_EQ((int32_t)state_of(kInjA), (int32_t)KMOD_STATE_READY);
    KCHECK_EQ((int32_t)state_of(kInjB), (int32_t)KMOD_STATE_READY);
    KCHECK_EQ_U64((uint64_t)KdllPinCount(kInjB), 1u);

    const uint64_t *magic = (const uint64_t *)KdllResolve(kInjA, "inj_a_magic");
    KCHECK(magic != nullptr && *magic == 0x494E4A41ull);
    const uint64_t *b_ok = (const uint64_t *)KdllResolve(kInjB, "inj_b_ok");
    KCHECK(b_ok != nullptr && *b_ok == 0x494E4A42ull);

    /* a 作为依赖被自动加载、没有 PIN：显式注入一次拿 PIN，才能 KdllEject
     * 走真正的卸载路径。 */
    KCHECK_EQ((int32_t)KdllInject(kInjA, KMOD_INJ_PIN | KMOD_INJ_SILENT),
              (int32_t)KMOD_OK);
    KCHECK_EQ((int32_t)KdllEject(kInjB), (int32_t)KMOD_OK);
    KCHECK_EQ((int32_t)state_of(kInjB), (int32_t)KMOD_ERR_NOTFOUND);
    KCHECK_EQ((int32_t)state_of(kInjA), (int32_t)KMOD_STATE_READY);

    KCHECK_EQ((int32_t)KdllEject(kInjA), (int32_t)KMOD_OK);
    KCHECK_EQ((int32_t)state_of(kInjA), (int32_t)KMOD_ERR_NOTFOUND);
    return 0;
}

int32_t test_negative(void)
{
    KCHECK_EQ((int32_t)KdllInject("vendor.test.missing", KMOD_INJ_SILENT),
              (int32_t)KMOD_ERR_NOTFOUND);
    KCHECK_EQ((int32_t)KdllEject("vendor.test.missing"),
              (int32_t)KMOD_ERR_NOTFOUND);
    KCHECK_EQ((int32_t)KdllInjectImage(nullptr, 0u, 0u),
              (int32_t)KMOD_ERR_INVAL);
    KCHECK_EQ((int32_t)KdllInjectFile("/tmp/does-not-exist.ko", 0u),
              (int32_t)KMOD_ERR_POLICY);
    return 0;
}

} /* namespace */

extern "C" int32_t test_inject_run(void)
{
    reset_all();
    KCHECK_EQ((int32_t)KdllInit(), (int32_t)KMOD_OK);
    if (test_resident() != 0)
        return 1;
    if (test_image_inject() != 0)
        return 1;
    if (test_negative() != 0)
        return 1;

    (void)KdllDump();
    std::printf("  [inject] resident+image+negative cases PASS\n");
    return 0;
}
