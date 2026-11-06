// tests/kmod/gov_test.cpp —— kmod 事务治理层宿主测试（governance.cpp）
// ----------------------------------------------------------------------------
// 唯一 main 在 test_main.cpp；本文件只提供 extern "C" int32_t test_gov_run()。
//
// 覆盖范围（宿主机可独立验证的部分，不依赖内核 arch 头）：
//   1. KmodCheckLoadable 的清单矩阵：Tier0/命名/版本/flags/能力/策略/HOT_REPLACE
//      规则（含 STRICT/OPEN/DISABLED 与 LOAD_FORCE）；
//   2. KmodExportsHash 的确定性与敏感性；
//   3. 未加载/不存在模块的 GovLoad/GovUnload/GovHotReplace/epoch 错误路径与统计；
//   4. 白盒：注册表槽位上的卸载门禁（BUSY 优先于 UNLOADABLE POLICY）与
//      epoch __atomic 双检（READY 可进入、QUIESCING 拒绝）；
//   5. 可选：用 fixtures/fixture_a.o 走 KmodPoolAdd + KmodGovLoad，验证
//      prepare 阶段失败时事务零残留、计数正确（fixture 缺失则跳过该子项）。
//
// 说明：governance.cpp 依赖 kmod_core.cpp 的全局符号，宿主链接时由
// tests/kmod/host_platform.cpp 提供 weak 定义（构建脚本保证）。
// ----------------------------------------------------------------------------
#include <kmod/kmod_internal.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "test_common.h"

/* kmod_core.cpp 未参与宿主链接：直接引用 weak 定义的注册表/统计符号
 * （tests/kmod/host_platform.cpp 提供；kmod_pool.cpp 提供镜像池强定义）。 */
extern "C" {
extern volatile int32_t g_kmod_lock;
extern kmod_stats_t g_kmod_stats;
extern kmod_module_t g_kmod_modules[KMOD_MAX_MODULES];
extern uint32_t g_kmod_module_count;
extern kmod_image_t g_kmod_images[KMOD_MAX_IMAGES];
extern uint32_t g_kmod_image_count;
}

namespace {

#define GCHECK(cond)                                                          \
    do {                                                                      \
        if (!(cond)) {                                                        \
            KT_FAIL("%s", #cond);                                             \
            return 1;                                                         \
        }                                                                     \
    } while (0)

#define GCHECK_RC(expr, want)                                                 \
    do {                                                                      \
        int32_t _rc = (int32_t)(expr);                                        \
        if (_rc != (int32_t)(want)) {                                         \
            KT_FAIL("%s = %d, want %d", #expr, (int)_rc, (int)(want));        \
            return 1;                                                         \
        }                                                                     \
    } while (0)

#define GCHECK_ERR(expr)                                                      \
    do {                                                                      \
        int32_t _rc = (int32_t)(expr);                                        \
        if (_rc >= 0) {                                                       \
            KT_FAIL("%s = %d, want negative", #expr, (int)_rc);               \
            return 1;                                                         \
        }                                                                     \
    } while (0)

/* test_common.h 已提供 KCHECK_EQ_U64（带 return 1 语义，只用于 int 返回函数） */
#define GCHECK_EQ_U64(a, b) KCHECK_EQ_U64(a, b)

void mzero(void *p, size_t n)
{
    memset(p, 0, n);
}

void mcopy_str(char *dst, size_t cap, const char *src)
{
    size_t i = 0;
    for (; i + 1 < cap && src[i]; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

kmod_manifest_t make_manifest(const char *name, const char *version, uint16_t tier,
                              uint32_t flags, uint64_t required,
                              uint64_t optional)
{
    kmod_manifest_t m;
    mzero(&m, sizeof(m));
    m.magic = KMOD_MANIFEST_MAGIC;
    m.abi_version = KMOD_ABI_VERSION;
    m.tier = tier;
    m.flags = flags;
    mcopy_str(m.name, sizeof(m.name), name);
    mcopy_str(m.version, sizeof(m.version), version);
    m.required_caps = required;
    m.optional_caps = optional;
    return m;
}

int check_loadable_matrix(void)
{
    /* 基线：可信模块 + required LOG 应可加载 */
    KmodCapInit();
    KmodCapSetMode(KMOD_POLICY_STRICT);
    GCHECK_RC(KmodCapTrustAdd("vendor.test.gov", KMOD_CAP_LOG | KMOD_CAP_HOT_REPLACE |
                                                   KMOD_CAP_MEM_ALLOC),
              KMOD_OK);

    kmod_manifest_t m = make_manifest("vendor.test.gov", "1.0.0", KMOD_TIER_IMPL,
                                      0, KMOD_CAP_LOG, 0);
    GCHECK_RC(KmodCheckLoadable(&m, 0), KMOD_OK);

    /* Tier0 不可模块化 */
    m.tier = KMOD_TIER_CORE;
    GCHECK_RC(KmodCheckLoadable(&m, 0), KMOD_ERR_TIER);
    m.tier = KMOD_TIER_POLICY;

    /* 命名 / 版本 */
    m = make_manifest("Bad.Name", "1.0.0", KMOD_TIER_IMPL, 0, 0, 0);
    GCHECK_RC(KmodCheckLoadable(&m, 0), KMOD_ERR_NAME);
    m = make_manifest("vendor.test.gov", "1.2.x", KMOD_TIER_IMPL, 0, 0, 0);
    GCHECK_RC(KmodCheckLoadable(&m, 0), KMOD_ERR_VERSION);
    m = make_manifest("vendor.test.gov", "1.0.0", KMOD_TIER_IMPL, 0, 0, 0);
    GCHECK_RC(KmodCheckLoadable(&m, 0), KMOD_OK);

    /* 未知 flags 位 */
    m = make_manifest("vendor.test.gov", "1.0.0", KMOD_TIER_IMPL, (1u << 20), 0, 0);
    GCHECK_RC(KmodCheckLoadable(&m, 0), KMOD_ERR_FORMAT);

    /* 能力：保留位 / 未定义位 / 未声明但 required */
    m = make_manifest("vendor.test.gov", "1.0.0", KMOD_TIER_IMPL, 0,
                      KMOD_CAP_RESERVED_CORE, 0);
    GCHECK_RC(KmodCheckLoadable(&m, 0), KMOD_ERR_CAP);
    m = make_manifest("vendor.test.gov", "1.0.0", KMOD_TIER_IMPL, 0,
                      (1ull << 40), 0);
    GCHECK_RC(KmodCheckLoadable(&m, 0), KMOD_ERR_CAP);
    m = make_manifest("vendor.test.gov", "1.0.0", KMOD_TIER_IMPL, 0,
                      KMOD_CAP_MEM_DMA, 0); /* 可信 mask 未含 MEM_DMA */
    GCHECK_RC(KmodCheckLoadable(&m, 0), KMOD_ERR_CAP);

    /* HOTREPLACE 硬规则：必须在 required_caps 里申请 HOT_REPLACE */
    m = make_manifest("vendor.test.gov", "1.0.0", KMOD_TIER_IMPL,
                      KMOD_MF_HOTREPLACE, KMOD_CAP_LOG, 0);
    GCHECK_RC(KmodCheckLoadable(&m, 0), KMOD_ERR_CAP);
    m = make_manifest("vendor.test.gov", "1.0.0", KMOD_TIER_IMPL,
                      KMOD_MF_HOTREPLACE,
                      KMOD_CAP_LOG | KMOD_CAP_HOT_REPLACE, 0);
    GCHECK_RC(KmodCheckLoadable(&m, 0), KMOD_OK);

    /* 未知模块：STRICT 拒绝（POLICY），DISABLED 一律 POLICY */
    m = make_manifest("vendor.test.unknown", "1.0.0", KMOD_TIER_IMPL, 0, 0, 0);
    GCHECK_RC(KmodCheckLoadable(&m, 0), KMOD_ERR_POLICY);
    KmodCapSetMode(KMOD_POLICY_OPEN);
    GCHECK_RC(KmodCheckLoadable(&m, 0), KMOD_OK);
    m = make_manifest("vendor.test.unknown", "1.0.0", KMOD_TIER_IMPL, 0,
                      KMOD_CAP_RESERVED_CORE, 0);
    GCHECK_RC(KmodCheckLoadable(&m, 0), KMOD_ERR_CAP); /* OPEN 仍拒保留位 */
    KmodCapSetMode(KMOD_POLICY_DISABLED);
    m = make_manifest("vendor.test.gov", "1.0.0", KMOD_TIER_IMPL, 0, 0, 0);
    GCHECK_RC(KmodCheckLoadable(&m, 0), KMOD_ERR_POLICY);
    KmodCapSetMode(KMOD_POLICY_STRICT);

    /* 实验性模块：STRICT 默认拒绝，LOAD_FORCE 放行 */
    m = make_manifest("vendor.test.gov", "1.0.0", KMOD_TIER_IMPL,
                      KMOD_MF_EXPERIMENTAL, 0, 0);
    GCHECK_RC(KmodCheckLoadable(&m, 0), KMOD_ERR_POLICY);
    GCHECK_RC(KmodCheckLoadable(&m, KMOD_LOAD_FORCE), KMOD_OK);

    /* 清单魔数/ABI */
    m = make_manifest("vendor.test.gov", "1.0.0", KMOD_TIER_IMPL, 0, 0, 0);
    m.magic = 0xDEADBEEFu;
    GCHECK_RC(KmodCheckLoadable(&m, 0), KMOD_ERR_FORMAT);
    m = make_manifest("vendor.test.gov", "1.0.0", KMOD_TIER_IMPL, 0, 0, 0);
    m.abi_version = (uint16_t)(KMOD_ABI_VERSION + 1u);
    GCHECK_RC(KmodCheckLoadable(&m, 0), KMOD_ERR_VERSION);
    GCHECK_RC(KmodCheckLoadable(nullptr, 0), KMOD_ERR_INVAL);
    return 0;
}

int check_exports_hash(void)
{
    static const char *names[] = { "sym_a", "sym_b", "sym_c" };
    kmod_export_t a[3];
    kmod_export_t b[3];
    mzero(a, sizeof(a));
    mzero(b, sizeof(b));
    for (uint32_t i = 0; i < 3; i++) {
        a[i].symbol = names[i];
        a[i].contract = "vendor.test.v1";
        a[i].size = 0;
        a[i].abi_version = 1;
        a[i].kind = KMOD_SYM_FUNC;
        b[i] = a[i];
    }
    uint64_t ha = KmodExportsHash(a, 3);
    GCHECK(ha != 0u);
    GCHECK_EQ_U64(ha, KmodExportsHash(b, 3)); /* 内容相同 -> 指纹相同（不掺地址） */
    b[1].symbol = "sym_x";                    /* 内容变化 -> 指纹变化 */
    GCHECK(ha != KmodExportsHash(b, 3));
    GCHECK(KmodExportsHash(nullptr, 0) != 0u);
    GCHECK(KmodExportsHash(a, 0) == KmodExportsHash(nullptr, 0));
    return 0;
}

int check_missing_module_paths(void)
{
    g_kmod_stats.loads_fail = 0;
    g_kmod_stats.unloads_fail = 0;
    g_kmod_stats.hotswaps_fail = 0;

    GCHECK_RC(KmodGovLoad("vendor.test.absent", nullptr, 0), KMOD_ERR_NOTFOUND);
    GCHECK_EQ_U64(g_kmod_stats.loads_fail, 1u);
    GCHECK_RC(KmodGovLoad("vendor.test.absent", "9.9.9", 0), KMOD_ERR_NOTFOUND);
    GCHECK_EQ_U64(g_kmod_stats.loads_fail, 2u);
    GCHECK_RC(KmodGovUnload("vendor.test.absent", 0), KMOD_ERR_NOTFOUND);
    GCHECK_EQ_U64(g_kmod_stats.unloads_fail, 1u);
    GCHECK_RC(KmodGovHotReplace("vendor.test.absent", "1.0.0", 0),
              KMOD_ERR_NOTFOUND);
    GCHECK_EQ_U64(g_kmod_stats.hotswaps_fail, 1u);

    GCHECK(KmodModuleFind("vendor.test.absent") == nullptr);
    GCHECK_RC(KmodModuleIndexOf("vendor.test.absent"), KMOD_ERR_NOTFOUND);
    GCHECK_RC(KmodEpochEnterInternal("vendor.test.absent"), KMOD_ERR_NOTFOUND);
    KmodEpochExitInternal("vendor.test.absent"); /* 无模块时安全 no-op */

    int32_t idx = 123;
    GCHECK_ERR(KmodModuleAllocFromImage(-1, 0, &idx));
    GCHECK(idx == -1);
    GCHECK_ERR(KmodModuleAllocFromImage(9999, 0, &idx));

    /* 提交原语对空指针必须安全（事务回滚兜底路径） */
    GCHECK_RC(KmodBindImports(nullptr), KMOD_ERR_INVAL);
    GCHECK_RC(KmodApplyExports(nullptr), KMOD_ERR_INVAL);
    GCHECK_RC(KmodCallInit(nullptr), KMOD_ERR_INVAL);
    GCHECK_RC(KmodQuiesceAndDrain(nullptr, 0), KMOD_ERR_INVAL);
    GCHECK_RC(KmodRebindConsumers(nullptr, nullptr, 0, nullptr, 0), KMOD_ERR_INVAL);
    KmodUnbindAll(nullptr);
    KmodRevertExports(nullptr);
    KmodCallExit(nullptr);
    return 0;
}

/* 白盒：槽位门禁/epoch 双检（宿主弱定义注册表；用完清空） */
int check_registry_gates(void)
{
    /* 从干净注册表开始（gov 测试是唯一直接操作槽位的测试） */
    mzero(g_kmod_modules, sizeof(kmod_module_t) * KMOD_MAX_MODULES);
    g_kmod_module_count = 0u;

    kmod_module_t *m = &g_kmod_modules[0];
    mcopy_str(m->name, sizeof(m->name), "vendor.test.fake");
    mcopy_str(m->version, sizeof(m->version), "1.0.0");
    m->state = KMOD_STATE_READY;
    m->flags = 0u; /* 未声明 MF_UNLOADABLE */
    m->pool_index = -1;
    g_kmod_module_count = 1u;

    GCHECK_RC(KmodModuleIndexOf("vendor.test.fake"), 0);
    GCHECK(KmodModuleFind("vendor.test.fake") == m);

    /* (a) 无 UNLOADABLE -> POLICY */
    g_kmod_stats.reject_policy = 0;
    GCHECK_RC(KmodGovUnload("vendor.test.fake", 0), KMOD_ERR_POLICY);
    GCHECK_EQ_U64(g_kmod_stats.reject_policy, 1u);
    GCHECK_EQ_U64(m->state, KMOD_STATE_READY);

    /* (b) refcount/消费者门禁优先于 UNLOADABLE -> BUSY */
    m->refcount = 1u;
    g_kmod_stats.reject_busy = 0;
    GCHECK_RC(KmodGovUnload("vendor.test.fake", 0), KMOD_ERR_BUSY);
    GCHECK_EQ_U64(g_kmod_stats.reject_busy, 1u);
    m->refcount = 0u;
    m->flags = KMOD_MF_UNLOADABLE;

    /* (c) epoch：READY 可进入，QUIESCING 拒绝，退出正确回减 */
    GCHECK_RC(KmodEpochEnterInternal("vendor.test.fake"), KMOD_OK);
    GCHECK_EQ_U64(m->epoch_active, 1u);
    m->state = KMOD_STATE_QUIESCING;
    GCHECK_RC(KmodEpochEnterInternal("vendor.test.fake"), KMOD_ERR_BUSY);
    GCHECK_EQ_U64(m->epoch_active, 1u); /* 第二轮检查失败必须回滚计数 */
    KmodEpochExitInternal("vendor.test.fake");
    GCHECK_EQ_U64(m->epoch_active, 0u);
    m->state = KMOD_STATE_READY;

    /* 清理白盒槽位 */
    mzero(g_kmod_modules, sizeof(kmod_module_t) * KMOD_MAX_MODULES);
    g_kmod_module_count = 0u;
    return 0;
}

/* 可选集成：fixture_a.o 有直接未定义符号，prepare 必须失败且零残留 */
int check_prepare_failure_is_atomic(void)
{
    std::vector<uint8_t> blob;
    if (!kmod_read_fixture("fixture_a.o", blob)) {
        KT_INFO("fixture_a.o not found, skip prepare-atomic case");
        return 0;
    }
    GCHECK_RC(KmodCapTrustAdd("vendor.test.a", KMOD_CAP_MEM_ALLOC), KMOD_OK);
    int32_t pool = KmodPoolAdd(blob.data(), (uint64_t)blob.size(), false);
    if (pool < 0) {
        KT_INFO("fixture_a.o rejected by inspect rc=%d, skip", (int)pool);
        return 0;
    }
    uint32_t count_before = g_kmod_module_count;
    uint64_t fail_before = g_kmod_stats.loads_fail;
    int32_t rc = KmodGovLoad("vendor.test.a", nullptr, 0);
    /* fixture_a 直接引用 kmod_fixture_kexp_probe，治理层的 kexp 表里没有该符号：
     * prepare 必须失败；无论具体错误码，事务都不得留下模块残留。 */
    GCHECK(rc < 0);
    GCHECK_EQ_U64(g_kmod_module_count, count_before);
    GCHECK(g_kmod_stats.loads_fail > fail_before);
    GCHECK(KmodModuleFind("vendor.test.a") == nullptr);
    return 0;
}

} /* namespace */

extern "C" int32_t test_gov_run(void)
{
    std::printf("  [gov] governance transactional layer\n");

    KmodCapInit();
    KmodCapSetMode(KMOD_POLICY_STRICT);
    GCHECK_RC(KmodGovInit(), KMOD_OK);
    GCHECK_RC(KmodGovInit(), KMOD_OK); /* 幂等 */

    if (check_loadable_matrix() != 0)
        return 1;
    if (check_exports_hash() != 0)
        return 1;
    if (check_missing_module_paths() != 0)
        return 1;
    if (check_registry_gates() != 0)
        return 1;
    if (check_prepare_failure_is_atomic() != 0)
        return 1;

    /* 收尾：恢复默认策略，避免影响后续 syscall 测试 */
    KmodCapSetMode(KMOD_POLICY_STRICT);
    KT_INFO("gov pass (loadable matrix / exports hash / gates / atomic prepare)");
    return 0;
}
