// tests/kmod/component_test.cpp — 内建组件 (builtin component) 模型宿主测试
// ----------------------------------------------------------------------------
// 覆盖 builtin.cpp / kmod_component.h：
//   1. 描述符/条目解析（KmodBuiltinImportCount/ExportCount/Imports/Exports）：
//      正向合成组件；负向：空指针、BEGIN/END 哨兵缺失、条目数非整除、
//      条目数超限、begin>=end、坏 manifest（magic/abi/name）。
//   2. KmodModuleAllocBuiltin：落成模块节点（BUILTIN 标记/能力/Tier/条目布局）、
//      重名 EXISTS、坏描述符/保留能力位拒绝、释放后槽位回收。
//   3. 宿主描述表（链接脚本 __kbuiltin_desc_start/end 的替身，见 host_platform）：
//      KmodBuiltinCount/At/Find + KmodBuiltinInit 对坏描述符全量预校验 +
//      KmodBuiltinBootLoad 正向（kernel 导入回填、导出可调用、READY、幂等）。
//   4. BUILTIN 治理边界：KmodGovUnload 拒绝（KMOD_ERR_POLICY，模块保持 READY）、
//      热替换到不存在版本返回负码且状态不变；利用 fixtures/fixture_hostcomp_v2.o
//      （init 故意失败的同名 .ko）验证 KmodGovHotReplace 失败后**完整回退**：
//      内建组件恢复 1.0.0/READY/BUILTIN，旧导出仍可调用，新镜像无泄漏。
//
// 注意：本测试直接操作宿主 weak 全局 g_kmod_modules/g_kmod_module_count
// （host_platform.cpp 提供），结束时清空，避免影响后续套件。
// ----------------------------------------------------------------------------
#include <kmod/kmod_internal.h>
#include <kmod/kmod_services.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "host_platform.h"
#include "test_common.h"

extern "C" {
extern kmod_module_t g_kmod_modules[KMOD_MAX_MODULES];
extern uint32_t g_kmod_module_count;
}

namespace {

uint64_t g_live_base = 0;
const char *kCompName = "vendor.host.comp";
const char *kCompVer = "1.0.0";

int g_init_calls = 0;
int g_exit_calls = 0;
void *g_log_slot = nullptr; // 合成组件的 kernel 导入槽（kprintf_）

int32_t comp_init(void)
{
    g_init_calls++;
    return KMOD_OK;
}
void comp_exit(void)
{
    g_exit_calls++;
}

int comp_fn(int x)
{
    return x + 0x5A5A;
}

struct comp_ops {
    int (*hello)(int x);
    int (*dummy)(void);
};
comp_ops g_ops = {comp_fn, nullptr};

// ---- 合成组件：manifest + .in(1 个 kernel 导入) + .out(2 个导出) ----
kmod_manifest_t g_manifest;
struct InBlob {
    uint64_t begin;
    kmod_import_t entries[1];
    uint64_t end;
} g_in;
struct OutBlob {
    uint64_t begin;
    kmod_export_t entries[2];
    uint64_t end;
} g_out;
kmod_builtin_raw_t g_raw;

void build_synthetic_component()
{
    std::memset(&g_manifest, 0, sizeof(g_manifest));
    g_manifest.magic = KMOD_MANIFEST_MAGIC;
    g_manifest.abi_version = KMOD_ABI_VERSION;
    g_manifest.tier = KMOD_TIER_IMPL;
    g_manifest.flags = 0u;
    std::strcpy(g_manifest.name, kCompName);
    std::strcpy(g_manifest.version, kCompVer);
    std::strcpy(g_manifest.description, "host synthetic builtin component");
    g_manifest.required_caps = KMOD_CAP_LOG;
    g_manifest.optional_caps = 0u;
    g_manifest.init = comp_init;
    g_manifest.exit_hook = comp_exit;
    g_manifest.init_budget_ms = 100u;
    g_manifest.quiesce_budget_ms = 50u;

    std::memset(&g_in, 0, sizeof(g_in));
    g_in.begin = KMOD_BUILTIN_BEGIN_MAGIC;
    g_in.entries[0].provider = "kernel";
    g_in.entries[0].symbol = "kprintf_";
    g_in.entries[0].contract = "kernel.log.v1";
    g_in.entries[0].slot = &g_log_slot;
    g_in.entries[0].abi_min = 1u;
    g_in.entries[0].flags = KMOD_IMP_REQUIRED;
    g_in.end = KMOD_BUILTIN_END_MAGIC;

    std::memset(&g_out, 0, sizeof(g_out));
    g_out.begin = KMOD_BUILTIN_BEGIN_MAGIC;
    g_out.entries[0].symbol = "host_comp_fn";
    g_out.entries[0].contract = "vendor.host.comp.v1";
    g_out.entries[0].addr = (void *)&comp_fn;
    g_out.entries[0].abi_version = 1u;
    g_out.entries[0].kind = KMOD_SYM_FUNC;
    g_out.entries[1].symbol = "vendor.host.comp.ops.v1";
    g_out.entries[1].contract = "vendor.host.comp.ops.v1";
    g_out.entries[1].addr = (void *)&g_ops;
    g_out.entries[1].size = (uint32_t)sizeof(g_ops);
    g_out.entries[1].abi_version = 1u;
    g_out.entries[1].kind = KMOD_SYM_CONTRACT;
    g_out.end = KMOD_BUILTIN_END_MAGIC;

    std::memset(&g_raw, 0, sizeof(g_raw));
    g_raw.magic = KMOD_BUILTIN_MAGIC;
    g_raw.manifest = &g_manifest;
    g_raw.in_begin = (const uint8_t *)&g_in;
    g_raw.in_end = (const uint8_t *)&g_in.end;
    g_raw.out_begin = (const uint8_t *)&g_out;
    g_raw.out_end = (const uint8_t *)&g_out.end;
}

void reset_registry()
{
    std::memset(g_kmod_modules, 0, sizeof(kmod_module_t) * KMOD_MAX_MODULES);
    g_kmod_module_count = 0u;
    std::memset(&g_kmod_stats, 0, sizeof(g_kmod_stats));
}

void reset_all()
{
    KmodCapInit();
    KmodCapSetMode(KMOD_POLICY_STRICT);
    (void)KmodSeatInit();
    (void)KmodSyscallInit();
    (void)KmodGovInit();
    reset_registry();
    g_init_calls = 0;
    g_exit_calls = 0;
    g_log_slot = nullptr;
}

bool live_ok(const char *what)
{
    uint64_t live = kmod_host_live_alloc_count();
    if (live != g_live_base) {
        KT_FAIL("%s: live kmod allocations %llu (baseline %llu)", what,
                (unsigned long long)live, (unsigned long long)g_live_base);
        return false;
    }
    return true;
}

const void *kernel_export_addr(const char *name)
{
    const kmod_kernel_export_t *kexp = nullptr;
    uint32_t n = 0;
    KmodKernelExports(&kexp, &n);
    for (uint32_t i = 0; i < n; i++) {
        if (kexp[i].name && std::strcmp(kexp[i].name, name) == 0)
            return kexp[i].addr;
    }
    return nullptr;
}

// ============================================================================
// 1) 条目解析负向（不依赖全局描述表）
// ============================================================================
int32_t test_parse_negative()
{
    build_synthetic_component();
    // 正向基线
    KCHECK_EQ_U64((uint64_t)KmodBuiltinImportCount(&g_raw), 1u);
    KCHECK_EQ_U64((uint64_t)KmodBuiltinExportCount(&g_raw), 2u);
    KCHECK(KmodBuiltinImports(&g_raw) == &g_in.entries[0]);
    KCHECK(KmodBuiltinExports(&g_raw) == &g_out.entries[0]);
    KCHECK_EQ_U64((uint64_t)KmodBuiltinImportCount(nullptr), 0u);
    KCHECK_EQ_U64((uint64_t)KmodBuiltinExportCount(nullptr), 0u);
    KCHECK(KmodBuiltinImports(nullptr) == nullptr);
    KCHECK(KmodBuiltinExports(nullptr) == nullptr);

    // bad raw magic / null manifest / bad manifest magic / abi / name
    {
        kmod_builtin_raw_t r = g_raw;
        r.magic = 0;
        KCHECK_EQ_U64((uint64_t)KmodBuiltinImportCount(&r), 0u);
        r = g_raw;
        r.manifest = nullptr;
        KCHECK_EQ_U64((uint64_t)KmodBuiltinImportCount(&r), 0u);
        r = g_raw;
        kmod_manifest_t man = g_manifest;
        man.magic = 0;
        r.manifest = &man;
        KCHECK_EQ_U64((uint64_t)KmodBuiltinImportCount(&r), 0u);
        man = g_manifest;
        man.abi_version = (uint16_t)(KMOD_ABI_VERSION + 1u);
        r.manifest = &man;
        KCHECK_EQ_U64((uint64_t)KmodBuiltinImportCount(&r), 0u);
        man = g_manifest;
        std::strcpy(man.name, "Bad.Name");
        r.manifest = &man;
        KCHECK_EQ_U64((uint64_t)KmodBuiltinImportCount(&r), 0u);
        // 反向：out 边界坏也必须整体失败（返回 0 / nullptr）
        r = g_raw;
        r.out_begin = nullptr;
        KCHECK_EQ_U64((uint64_t)KmodBuiltinImportCount(&r), 0u);
        KCHECK(KmodBuiltinExports(&r) == nullptr);
    }

    // BEGIN/END 哨兵缺失、begin>=end、条目数非整除、条目数超限
    // 布局：[BEGIN 8B][entries][END 8B]；raw.in_end 指向 END 哨兵本身
    // （builtin.cpp parse_entries: bytes = end - begin - 8）。
    {
        // 128B 缓冲：覆盖 BEGIN/END 哨兵读取（sizeof(g_in)=64）且留余量
        alignas(8) static uint8_t buf_begin[128];
        alignas(8) static uint8_t buf_end[128];
        alignas(8) static uint8_t buf_odd[128];
        const size_t in_end_off = sizeof(g_in) - sizeof(uint64_t); // 56
        std::memset(buf_begin, 0, sizeof(buf_begin));
        std::memcpy(buf_begin, &g_in, sizeof(g_in));
        uint64_t bad = 0;
        std::memcpy(buf_begin, &bad, 8); // 破坏 BEGIN
        kmod_builtin_raw_t r = g_raw;
        r.in_begin = buf_begin;
        r.in_end = buf_begin + in_end_off;
        KCHECK_EQ_U64((uint64_t)KmodBuiltinImportCount(&r), 0u);

        std::memset(buf_end, 0, sizeof(buf_end));
        std::memcpy(buf_end, &g_in, sizeof(g_in));
        std::memcpy(buf_end + in_end_off, &bad, 8); // 破坏 END
        r = g_raw;
        r.in_begin = buf_end;
        r.in_end = buf_end + in_end_off;
        KCHECK_EQ_U64((uint64_t)KmodBuiltinImportCount(&r), 0u);

        // begin == end / begin > end
        r = g_raw;
        r.in_begin = buf_end;
        r.in_end = buf_end;
        KCHECK_EQ_U64((uint64_t)KmodBuiltinImportCount(&r), 0u);
        r.in_begin = buf_end + 8;
        r.in_end = buf_end;
        KCHECK_EQ_U64((uint64_t)KmodBuiltinImportCount(&r), 0u);

        // 非整除：BEGIN + 1 条 entry + 4 字节 padding + END
        std::memset(buf_odd, 0, sizeof(buf_odd));
        uint64_t bm = KMOD_BUILTIN_BEGIN_MAGIC, em = KMOD_BUILTIN_END_MAGIC;
        std::memcpy(buf_odd, &bm, 8);
        std::memcpy(buf_odd + 8, &g_in.entries[0], sizeof(kmod_import_t));
        size_t odd_end_off = 8 + sizeof(kmod_import_t) + 4;
        std::memcpy(buf_odd + odd_end_off, &em, 8);
        r = g_raw;
        r.in_begin = buf_odd;
        r.in_end = buf_odd + odd_end_off;
        KCHECK_EQ_U64((uint64_t)KmodBuiltinImportCount(&r), 0u);
    }

    // 条目数超限：65 条 import（> KMOD_MAX_IMPORTS=64）
    {
        alignas(8) static uint8_t big[8 + 65 * sizeof(kmod_import_t) + 8];
        std::memset(big, 0, sizeof(big));
        uint64_t bm = KMOD_BUILTIN_BEGIN_MAGIC, em = KMOD_BUILTIN_END_MAGIC;
        std::memcpy(big, &bm, 8);
        size_t big_end_off = 8 + 65 * sizeof(kmod_import_t);
        std::memcpy(big + big_end_off, &em, 8);
        kmod_builtin_raw_t r = g_raw;
        r.in_begin = big;
        r.in_end = big + big_end_off;
        KCHECK_EQ_U64((uint64_t)KmodBuiltinImportCount(&r), 0u);
    }
    return 0;
}

// ============================================================================
// 2) KmodModuleAllocBuiltin：模块节点落成与负向
// ============================================================================
int32_t test_module_alloc_builtin()
{
    reset_all();
    build_synthetic_component();
    int32_t idx = 123;
    int32_t rc = KmodModuleAllocBuiltin(&g_raw, &idx);
    KCHECK_MSG(rc == KMOD_OK, "AllocBuiltin rc=%d", (int)rc);
    KCHECK(idx >= 0);
    kmod_module_t *m = &g_kmod_modules[idx];
    KCHECK(std::strcmp(m->name, kCompName) == 0);
    KCHECK(std::strcmp(m->version, kCompVer) == 0);
    KCHECK_EQ(m->state, KMOD_STATE_LOADING);
    KCHECK_EQ(m->tier, KMOD_TIER_IMPL);
    KCHECK((m->internal_flags & KMOD_MODF_BUILTIN) != 0u);
    KCHECK_EQ_U64(m->caps_granted, KMOD_CAP_LOG);
    KCHECK_EQ(m->pool_index, -1);
    KCHECK(m->img.mem == nullptr); // 内建组件无独立镜像内存
    KCHECK_EQ_U64(m->n_imports, 1u);
    KCHECK_EQ_U64(m->n_exports, 2u);
    KCHECK(m->imports == &g_in.entries[0]);
    KCHECK(m->exports == &g_out.entries[0]);
    KCHECK(m->gen_exports_hash != 0u);

    // 重名 -> EXISTS
    int32_t idx2 = -1;
    KCHECK_EQ(KmodModuleAllocBuiltin(&g_raw, &idx2), KMOD_ERR_EXISTS);
    KCHECK_EQ(idx2, -1);

    // 空/坏描述符
    int32_t idx3 = -1;
    KCHECK_EQ(KmodModuleAllocBuiltin(nullptr, &idx3), KMOD_ERR_INVAL);
    KCHECK_EQ(KmodModuleAllocBuiltin(&g_raw, nullptr), KMOD_ERR_INVAL);
    kmod_builtin_raw_t bad = g_raw;
    kmod_manifest_t man = g_manifest;
    man.tier = KMOD_TIER_CORE; // Tier0 不可作为组件
    bad.manifest = &man;
    KCHECK_EQ(KmodModuleAllocBuiltin(&bad, &idx3), KMOD_ERR_TIER);
    man = g_manifest;
    man.required_caps = KMOD_CAP_RESERVED_CORE; // Tier0 保留位
    bad.manifest = &man;
    KCHECK_EQ(KmodModuleAllocBuiltin(&bad, &idx3), KMOD_ERR_CAP);
    man = g_manifest;
    man.abi_version = (uint16_t)(KMOD_ABI_VERSION + 1u);
    bad.manifest = &man;
    // raw_view() 先做描述符级 ABI 预校验，因此在进入完整 manifest 校验前即 FORMAT
    KCHECK_EQ(KmodModuleAllocBuiltin(&bad, &idx3), KMOD_ERR_FORMAT);

    // 回收槽位
    KmodModuleRelease(m);
    reset_registry();
    return 0;
}

// ============================================================================
// 3) 全局描述表（host 替身）+ KmodBuiltinInit/BootLoad 正向
// ============================================================================
int32_t test_builtin_boot()
{
    reset_all();
    build_synthetic_component();
    KCHECK_EQ_U64((uint64_t)kmod_host_builtin_slots(), 1u);

    // 默认空槽位 -> Count=1 但描述符非法，Init 必须全量预校验失败
    kmod_host_builtin_set_raw(nullptr);
    KCHECK_EQ_U64(KmodBuiltinCount(), 1u);
    KCHECK(KmodBuiltinAt(0) != nullptr);
    KCHECK(KmodBuiltinAt(1) == nullptr);
    KCHECK_EQ(KmodBuiltinInit(), KMOD_ERR_FORMAT);

    // 装入合成描述符
    kmod_host_builtin_set_raw(&g_raw);
    KCHECK_EQ(KmodBuiltinInit(), KMOD_OK);
    KCHECK(KmodBuiltinFind(kCompName) == KmodBuiltinAt(0));
    KCHECK(KmodBuiltinFind("vendor.host.none") == nullptr);
    KCHECK(KmodBuiltinFind(nullptr) == nullptr);
    KCHECK_EQ_U64((uint64_t)KmodBuiltinImportCount(KmodBuiltinAt(0)), 1u);
    KCHECK_EQ_U64((uint64_t)KmodBuiltinExportCount(KmodBuiltinAt(0)), 2u);

    // 引导期拓扑化加载：bind(kernel kprintf_) -> apply -> init -> READY
    uint64_t loads_before = g_kmod_stats.loads_ok;
    int32_t rc = KmodBuiltinBootLoad();
    KCHECK_MSG(rc == KMOD_OK, "BuiltinBootLoad rc=%d", (int)rc);
    kmod_module_t *m = KmodModuleFind(kCompName);
    KCHECK(m != nullptr);
    KCHECK_EQ(m->state, KMOD_STATE_READY);
    KCHECK((m->internal_flags & KMOD_MODF_BUILTIN) != 0u);
    KCHECK_EQ(g_init_calls, 1);
    KCHECK(g_kmod_stats.loads_ok > loads_before);

    // kernel 导入槽已回填到宿主内核导出表里的 kprintf_
    const void *kprintf_addr = kernel_export_addr("kprintf_");
    KCHECK(kprintf_addr != nullptr);
    KCHECK_EQ_U64((uint64_t)g_log_slot, (uint64_t)kprintf_addr);

    // 导出函数可调用（内建组件直接是内核地址空间里的函数）
    KCHECK(m->n_exports == 2u);
    typedef int (*fn_t)(int);
    KCHECK_EQ(((fn_t)m->exports[0].addr)(7), 7 + 0x5A5A);

    // 幂等：二次 BootLoad 不重复 init、不报错
    KCHECK_EQ(KmodBuiltinBootLoad(), KMOD_OK);
    KCHECK_EQ(g_init_calls, 1);
    KCHECK_EQ(KmodBuiltinLoad(kCompName), KMOD_ERR_EXISTS);
    KCHECK_EQ(KmodBuiltinLoad("vendor.host.none"), KMOD_ERR_NOTFOUND);
    KCHECK_EQ(KmodBuiltinLoad(nullptr), KMOD_ERR_INVAL);
    if (!live_ok("builtin boot"))
        return 1;
    return 0;
}

// ============================================================================
// 3.x) provider="kernel" 共享服务导入的强校验：
//      kernel.klib.v1 正确导入 -> 槽回填到内核 ops；契约/尺寸/版本不匹配
//      -> 引导加载失败、槽保持 NULL（事务回滚，不允许半绑定）。
// ============================================================================
int32_t test_kernel_service_import_validation()
{
    // 正向：精确 expect_size 的 kernel.klib.v1 ops 导入
    reset_all();
    build_synthetic_component();
    g_in.entries[0].provider = "kernel";
    g_in.entries[0].symbol = "kernel.klib.v1";
    g_in.entries[0].contract = "kernel.klib.v1";
    g_in.entries[0].slot = &g_log_slot;
    g_in.entries[0].abi_min = 1u;
    g_in.entries[0].abi_max = 0u;
    g_in.entries[0].expect_size = (uint32_t)sizeof(kmod_klib_ops_t);
    g_in.entries[0].flags = KMOD_IMP_REQUIRED;
    kmod_host_builtin_set_raw(&g_raw);
    KCHECK_EQ(KmodBuiltinInit(), KMOD_OK);
    KCHECK_EQ(KmodBuiltinBootLoad(), KMOD_OK);
    const void *ops_addr = kernel_export_addr("kernel.klib.v1");
    KCHECK(ops_addr != nullptr);
    KCHECK_EQ_U64((uint64_t)g_log_slot, (uint64_t)ops_addr);

    // 负向：契约名 / expect_size / abi_min 不匹配
    struct ImportCase {
        const char *symbol;
        const char *contract;
        uint32_t expect;
        uint32_t abi_min;
        int32_t want;
        const char *what;
    };
    const ImportCase cases[] = {
        {"kernel.klib.v1", "kernel.klib.v1",
         (uint32_t)sizeof(kmod_klib_ops_t) + 8u, 1u, KMOD_ERR_CONTRACT,
         "expect_size mismatch must be rejected"},
        {"kernel.klib.v1", "kernel.mem.v1",
         (uint32_t)sizeof(kmod_klib_ops_t), 1u, KMOD_ERR_CONTRACT,
         "contract mismatch must be rejected"},
        {"kernel.klib.v1", "kernel.klib.v1",
         (uint32_t)sizeof(kmod_klib_ops_t), 2u, KMOD_ERR_VERSION,
         "abi_min too high must be rejected"},
    };
    for (const ImportCase &c : cases) {
        kmod_host_builtin_set_raw(nullptr);
        reset_all();
        build_synthetic_component();
        g_in.entries[0].provider = "kernel";
        g_in.entries[0].symbol = c.symbol;
        g_in.entries[0].contract = c.contract;
        g_in.entries[0].slot = &g_log_slot;
        g_in.entries[0].abi_min = c.abi_min;
        g_in.entries[0].abi_max = 0u;
        g_in.entries[0].expect_size = c.expect;
        g_in.entries[0].flags = KMOD_IMP_REQUIRED;
        kmod_host_builtin_set_raw(&g_raw);
        KCHECK_EQ(KmodBuiltinInit(), KMOD_OK);
        g_log_slot = nullptr;
        int32_t rc = KmodBuiltinBootLoad();
        KCHECK_MSG(rc == c.want, "%s: rc=%d want=%d", c.what, (int)rc,
                   (int)c.want);
        KCHECK_MSG(g_log_slot == nullptr, "%s: slot must stay NULL", c.what);
    }

    // 未解析的 symbol 同样失败（负向兜底）
    kmod_host_builtin_set_raw(nullptr);
    reset_all();
    build_synthetic_component();
    g_in.entries[0].provider = "kernel";
    g_in.entries[0].symbol = "kernel.klib.v999";
    g_in.entries[0].contract = "kernel.klib.v1";
    g_in.entries[0].slot = &g_log_slot;
    g_in.entries[0].abi_min = 1u;
    g_in.entries[0].abi_max = 0u;
    g_in.entries[0].expect_size = 0u;
    g_in.entries[0].flags = KMOD_IMP_REQUIRED;
    kmod_host_builtin_set_raw(&g_raw);
    KCHECK_EQ(KmodBuiltinInit(), KMOD_OK);
    int32_t rc = KmodBuiltinBootLoad();
    KCHECK_MSG(rc < 0, "unknown kernel symbol should fail, rc=%d", (int)rc);
    KCHECK(g_log_slot == nullptr);

    kmod_host_builtin_set_raw(nullptr);
    reset_all();
    return 0;
}

// ============================================================================
// 4) BUILTIN 治理边界：unload 拒绝 / hot replace 不存在版本
// ============================================================================
int32_t test_builtin_governance()
{
    // 复用 test_builtin_boot 已加载的组件（按顺序单独跑时自己加载）
    if (KmodModuleFind(kCompName) == nullptr) {
        build_synthetic_component();
        kmod_host_builtin_set_raw(&g_raw);
        KCHECK_EQ(KmodBuiltinBootLoad(), KMOD_OK);
    }
    kmod_module_t *m = KmodModuleFind(kCompName);
    KCHECK(m != nullptr);
    KCHECK_EQ(m->state, KMOD_STATE_READY);

    uint64_t rej_before = g_kmod_stats.reject_policy;
    KCHECK_EQ(KmodGovUnload(kCompName, 0), KMOD_ERR_POLICY);
    KCHECK(g_kmod_stats.reject_policy > rej_before);
    KCHECK_EQ(m->state, KMOD_STATE_READY); // 拒绝后状态不变
    KCHECK((m->internal_flags & KMOD_MODF_BUILTIN) != 0u);

    // 热替换到不存在版本：负错误码，元件保持 READY 且版本不变
    uint64_t fail_before = g_kmod_stats.hotswaps_fail;
    int32_t rc = KmodGovHotReplace(kCompName, "9.9.9", 0);
    KCHECK_MSG(rc < 0, "hot replace missing version should fail, rc=%d", (int)rc);
    KCHECK(g_kmod_stats.hotswaps_fail > fail_before);
    KCHECK_EQ(m->state, KMOD_STATE_READY);
    KCHECK(std::strcmp(m->version, kCompVer) == 0);
    KCHECK_EQ(KmodGovHotReplace(nullptr, nullptr, 0), KMOD_ERR_INVAL);
    return 0;
}

// ============================================================================
// 5) 热替换失败回退：同名 .ko（init 故意失败）不得破坏内建组件
// ============================================================================
int32_t test_hotswap_rollback()
{
    if (KmodModuleFind(kCompName) == nullptr) {
        build_synthetic_component();
        kmod_host_builtin_set_raw(&g_raw);
        KCHECK_EQ(KmodBuiltinBootLoad(), KMOD_OK);
    }
    kmod_module_t *m = KmodModuleFind(kCompName);
    KCHECK(m != nullptr);
    KCHECK_EQ(m->state, KMOD_STATE_READY);

    std::vector<uint8_t> blob;
    if (!kmod_read_fixture("fixture_hostcomp_v2.o", blob)) {
        KT_INFO("fixture_hostcomp_v2.o not found, skip hotswap rollback case");
        return 0;
    }
    // STRICT 策略下 .ko 需可信；cap 必须覆盖新清单 required（HOT_REPLACE）
    KCHECK_EQ(KmodCapTrustAdd(kCompName, KMOD_CAP_HOT_REPLACE | KMOD_CAP_LOG),
              KMOD_OK);
    int32_t pool = KmodPoolAdd(blob.data(), (uint64_t)blob.size(), false);
    KCHECK_MSG(pool >= 0, "pool add fixture_hostcomp_v2 rc=%d", (int)pool);

    uint64_t fail_before = g_kmod_stats.hotswaps_fail;
    uint64_t rollback_before = g_kmod_stats.rollbacks;
    int32_t rc = KmodGovHotReplace(kCompName, "2.0.0", 0);
    // 新版本 init 返回 KMOD_ERR_INVAL -> 全量回退，返回该错误码
    KCHECK_MSG(rc < 0, "hot replace should fail on bad init, rc=%d", (int)rc);

    // 回退后内建组件必须完全恢复
    KCHECK(m == KmodModuleFind(kCompName));
    KCHECK_EQ(m->state, KMOD_STATE_READY);
    KCHECK(std::strcmp(m->version, kCompVer) == 0);
    KCHECK((m->internal_flags & KMOD_MODF_BUILTIN) != 0u);
    KCHECK(g_kmod_stats.hotswaps_fail > fail_before);
    KCHECK(g_kmod_stats.rollbacks > rollback_before);
    // 旧导出仍可调用
    typedef int (*fn_t)(int);
    KCHECK_EQ(((fn_t)m->exports[0].addr)(11), 11 + 0x5A5A);
    // 新镜像已释放（无 kmod_alloc 泄漏）
    if (!live_ok("hotswap rollback"))
        return 1;
    return 0;
}

// ============================================================================
// 6) 清理：清空描述表与注册表，避免影响其他套件（本套件最后执行）
// ============================================================================
void cleanup()
{
    kmod_host_builtin_set_raw(nullptr);
    reset_registry();
}

} // namespace

extern "C" int32_t test_component_run(void)
{
    g_live_base = kmod_host_live_alloc_count();
    int32_t bad = 0;
    if (test_parse_negative() != 0)
        bad++;
    if (test_module_alloc_builtin() != 0)
        bad++;
    if (test_builtin_boot() != 0)
        bad++;
    if (test_kernel_service_import_validation() != 0)
        bad++;
    if (test_builtin_governance() != 0)
        bad++;
    if (test_hotswap_rollback() != 0)
        bad++;
    cleanup();
    if (!live_ok("component test end"))
        bad++;
    return bad == 0 ? 0 : 1;
}
