//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * selftest.cpp —— kmod 引导期自检（串口可观测门禁）
 * ----------------------------------------------------------------------------
 * 在调度器起来之后由内核线程调用（需要真睡眠/epoch 等待）。覆核：
 *   1. 引导期自动加载的模块按拓扑序就绪（依赖先置、init 已执行）；
 *   2. 引用计数：有消费者的模块不可卸载（EBUSY）；
 *   3. 模块级依赖声明可自动拉起 provider（卸载后重载消费者即可重建）；
 *   4. 依赖环被拓扑层拒绝（不产生任何半加载残留）；
 *   5. Tier0 保留能力被能力策略拒绝；
 *   6. init 失败的加载事务完整回滚（无残留、rollback 计数）；
 *   7. 热替换：座位提供者 v1 -> v2 原子切换、看门狗/epoch 路径工作、失败版本回退；
 *   8. syscall 边界：动态命名空间槽注册/热替换/卸载清除，固定槽实现覆写与回退。
 * 输出契约（QEMU 串口门禁依赖）：
 *   [kmod-selftest] case <name>: PASS|FAIL <detail>
 *   KMOD_SELFTEST PASS <n>/<total>    或    KMOD_SELFTEST FAIL <n>/<total>
 * ==========================================================================*/
#include <kmod/kmod_internal.h>

/* 演示模块名（kernel/modules/demo_*.cpp，mod-demos 维护） */
#define DEMO_HELLO    "vendor.demo.hello"
#define DEMO_GREETER  "vendor.demo.greeter"
#define DEMO_PRNG     "vendor.demo.prng"
#define DEMO_SYSCALL  "vendor.demo.syscall"
#define DEMO_BADINIT  "vendor.demo.badinit"
#define DEMO_BADCAP   "vendor.demo.badcap"
#define DEMO_CYCLE_A  "vendor.demo.cyclea"
#define DEMO_CYCLE_B  "vendor.demo.cycleb"
#define DEMO_SCHED    "vendor.demo.sched"
#define DEMO_ALLOC    "vendor.demo.alloc"
#define DEMO_KLIB     "vendor.demo.klib"

#define KMOD_SYSCALL_DYN_DEMO 200u
#define KMOD_ENOSYS_U64 ((uint64_t)(int64_t)(-38))
#define KMOD_EAGAIN_U64 ((uint64_t)(int64_t)(-11))

namespace {

int g_pass = 0;
int g_fail = 0;

void report(const char *name, bool ok, const char *detail)
{
    if (ok)
        g_pass++;
    else
        g_fail++;
    kmod_log("[kmod-selftest] case %s: %s %s\n", name,
             ok ? "PASS" : "FAIL", detail ? detail : "");
}

bool state_is(const char *name, uint16_t want)
{
    return KmodModuleState(name) == (int32_t)want;
}

bool str_eq_local(const char *a, const char *b)
{
    if (!a || !b)
        return a == b;
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

void why_set(char *why, uint32_t cap, const char *s)
{
    uint32_t i = 0;
    if (!why || cap == 0u)
        return;
    for (; s && s[i] && i + 1u < cap; i++)
        why[i] = s[i];
    why[i] = '\0';
}

/* 构造最小 syscall 帧（rax 槽位；a0..a5 清零） */
struct FakeFrame {
    uint64_t r[24];
};

uint64_t call_slot(uint32_t slot)
{
    FakeFrame f;
    for (uint32_t i = 0; i < 24; i++)
        f.r[i] = 0;
    f.r[14] = slot;      /* syscall_frame_t.rax 偏移 14*8 */
    return KmodSyscallEntry(&f);
}

bool case_boot_topology(void)
{
    char detail[160];
    bool ok = state_is(DEMO_HELLO, KMOD_STATE_READY) &&
              state_is(DEMO_GREETER, KMOD_STATE_READY);
    /* greeter 必须显式依赖 hello（.in 模块级边） */
    uint32_t n = KmodModuleCount();
    uint32_t greeter_deps = 0;
    for (uint32_t i = 0; i < n; i++) {
        kmod_module_info_t info;
        if (KmodModuleInfoAt(i, &info) != KMOD_OK)
            continue;
        const char *a = info.name;
        const char *b = DEMO_GREETER;
        while (*a && *a == *b) { a++; b++; }
        if (*a == '\0' && *b == '\0')
            greeter_deps = info.n_deps;
    }
    ok = ok && greeter_deps >= 1 && KmodStats()->loads_ok >= 2;
    const char *d = ok ? "" : "expected hello+greeter READY, greeter->hello edge";
    (void)detail;
    report("boot-topology", ok, d);
    return ok;
}

bool case_unload_busy(void)
{
    kmod_stats_t before = *KmodStats();
    int32_t rc = KmodUnload(DEMO_HELLO);
    bool ok = (rc == KMOD_ERR_BUSY) &&
              (KmodStats()->reject_busy > before.reject_busy) &&
              state_is(DEMO_HELLO, KMOD_STATE_READY);
    report("unload-refcount-busy", ok,
           ok ? "" : "consumer not enforcing refcount");
    return ok;
}

bool case_dep_autoload(void)
{
    bool ok = true;
    char why[96] = "";
    if (KmodUnload(DEMO_GREETER) != KMOD_OK) {
        ok = false;
        const char *s = "unload greeter failed";
        uint32_t i = 0;
        for (; s[i] && i + 1 < sizeof(why); i++) why[i] = s[i];
        why[i] = '\0';
    }
    if (ok && KmodUnload(DEMO_HELLO) != KMOD_OK) {
        ok = false;
        const char *s = "unload hello failed";
        uint32_t i = 0;
        for (; s[i] && i + 1 < sizeof(why); i++) why[i] = s[i];
        why[i] = '\0';
    }
    if (ok) {
        /* 只请求消费者：provider 必须由拓扑事务自动拉起 */
        ok = KmodLoad(DEMO_GREETER, KMOD_LOAD_NORMAL) == KMOD_OK &&
             state_is(DEMO_HELLO, KMOD_STATE_READY) &&
             state_is(DEMO_GREETER, KMOD_STATE_READY);
        if (!ok) {
            const char *s = "dependency autoload failed";
            uint32_t i = 0;
            for (; s[i] && i + 1 < sizeof(why); i++) why[i] = s[i];
            why[i] = '\0';
        }
    }
    report("dependency-autoload", ok, why);
    return ok;
}

bool case_cycle_reject(void)
{
    uint32_t before = KmodModuleCount();
    kmod_stats_t st0 = *KmodStats();
    int32_t rc = KmodLoad(DEMO_CYCLE_A, KMOD_LOAD_NORMAL);
    bool ok = (rc == KMOD_ERR_CYCLE) &&
              (KmodStats()->reject_cycle > st0.reject_cycle) &&
              (KmodModuleCount() == before) &&
              KmodModuleState(DEMO_CYCLE_A) < 0;
    report("dependency-cycle-reject", ok,
           ok ? "" : "cycle not rejected / residue left");
    return ok;
}

bool case_cap_reject(void)
{
    kmod_stats_t st0 = *KmodStats();
    int32_t rc = KmodLoad(DEMO_BADCAP, KMOD_LOAD_NORMAL);
    bool ok = (rc == KMOD_ERR_CAP || rc == KMOD_ERR_POLICY) &&
              (KmodStats()->reject_cap > st0.reject_cap);
    report("capability-reserved-reject", ok,
           ok ? "" : "Tier0 reserved capability not rejected");
    return ok;
}

bool case_rollback_badinit(void)
{
    uint32_t before = KmodModuleCount();
    kmod_stats_t st0 = *KmodStats();
    int32_t rc = KmodLoad(DEMO_BADINIT, KMOD_LOAD_NORMAL);
    bool ok = (rc != KMOD_OK) &&
              (KmodStats()->rollbacks > st0.rollbacks) &&
              (KmodModuleCount() == before) &&
              (KmodModuleState(DEMO_BADINIT) < 0);
    report("load-transaction-rollback", ok,
           ok ? "" : "failed init left residue or no rollback counter");
    return ok;
}

bool case_seat_hotswap(void)
{
    bool ok = true;
    char why[96] = "";
    if (KmodLoadVersion(DEMO_PRNG, "1.0.0", KMOD_LOAD_NORMAL) != KMOD_OK) {
        ok = false;
        const char *s = "load prng v1 failed";
        uint32_t i = 0;
        for (; s[i] && i + 1 < sizeof(why); i++) why[i] = s[i];
        why[i] = '\0';
    }
    uint64_t v1 = ok ? KmodSeatRandomU64() : 0;
    if (ok && (v1 >> 60) != 1ull) {
        ok = false;
        const char *s = "seat v1 provider not active";
        uint32_t i = 0;
        for (; s[i] && i + 1 < sizeof(why); i++) why[i] = s[i];
        why[i] = '\0';
    }
    if (ok) {
        int32_t rc = KmodHotReplace(DEMO_PRNG, "2.0.0");
        uint64_t v2 = KmodSeatRandomU64();
        ok = rc == KMOD_OK && (v2 >> 60) == 2ull &&
             KmodStats()->hotswaps_ok >= 1;
        if (!ok) {
            const char *s = "hot replace v1->v2 failed";
            uint32_t i = 0;
            for (; s[i] && i + 1 < sizeof(why); i++) why[i] = s[i];
            why[i] = '\0';
        }
    }
    if (ok) {
        /* 失败版本必须回退：模块仍 READY 且仍是 v2 提供者 */
        int32_t rc = KmodHotReplace(DEMO_PRNG, "9.9.9");
        uint64_t v3 = KmodSeatRandomU64();
        ok = (rc == KMOD_ERR_NOTFOUND) && (v3 >> 60) == 2ull &&
             state_is(DEMO_PRNG, KMOD_STATE_READY);
        if (!ok) {
            const char *s = "failed hot replace did not roll back";
            uint32_t i = 0;
            for (; s[i] && i + 1 < sizeof(why); i++) why[i] = s[i];
            why[i] = '\0';
        }
    }
    if (ok) {
        /* 卸载提供者 -> 座位回退 Tier0 内核默认实现 */
        int32_t rc = KmodUnload(DEMO_PRNG);
        ok = rc == KMOD_OK && KmodModuleState(DEMO_PRNG) < 0;
        if (!ok) {
            const char *s = "seat provider unload failed";
            uint32_t i = 0;
            for (; s[i] && i + 1 < sizeof(why); i++) why[i] = s[i];
            why[i] = '\0';
        }
    }
    report("seat-hotswap-rollback", ok, why);
    return ok;
}

bool case_syscall_governance(void)
{
    bool ok = true;
    char why[112] = "";
    uint64_t base_probe = call_slot(KMOD_SYSCALL_PROBE);
    if (base_probe != KMOD_SYSCALL_PROBE_MAGIC) {
        ok = false;
        const char *s = "tier0 probe default wrong";
        uint32_t i = 0;
        for (; s[i] && i + 1 < sizeof(why); i++) why[i] = s[i];
        why[i] = '\0';
    }
    if (ok && KmodLoadVersion(DEMO_SYSCALL, "1.0.0", KMOD_LOAD_NORMAL) != KMOD_OK) {
        ok = false;
        const char *s = "load syscall module v1 failed";
        uint32_t i = 0;
        for (; s[i] && i + 1 < sizeof(why); i++) why[i] = s[i];
        why[i] = '\0';
    }
    if (ok) {
        uint64_t dyn = call_slot(KMOD_SYSCALL_DYN_DEMO);
        uint64_t probe = call_slot(KMOD_SYSCALL_PROBE);
        ok = dyn == (KMOD_SYSCALL_PROBE_MAGIC | 0x11110000ull) &&
             probe == (KMOD_SYSCALL_PROBE_MAGIC | (1ull << 32));
        if (!ok) {
            const char *s = "dynamic slot/override v1 wrong";
            uint32_t i = 0;
            for (; s[i] && i + 1 < sizeof(why); i++) why[i] = s[i];
            why[i] = '\0';
        }
    }
    if (ok) {
        int32_t rc = KmodHotReplace(DEMO_SYSCALL, "2.0.0");
        uint64_t dyn = call_slot(KMOD_SYSCALL_DYN_DEMO);
        uint64_t probe = call_slot(KMOD_SYSCALL_PROBE);
        ok = rc == KMOD_OK &&
             dyn == (KMOD_SYSCALL_PROBE_MAGIC | 0x22220000ull) &&
             probe == (KMOD_SYSCALL_PROBE_MAGIC | (2ull << 32)) &&
             KmodStats()->syscall_overrides >= 1;
        if (!ok) {
            const char *s = "syscall hot replace v2 wrong";
            uint32_t i = 0;
            for (; s[i] && i + 1 < sizeof(why); i++) why[i] = s[i];
            why[i] = '\0';
        }
    }
    if (ok) {
        int32_t rc = KmodUnload(DEMO_SYSCALL);
        uint64_t dyn = call_slot(KMOD_SYSCALL_DYN_DEMO);
        uint64_t probe = call_slot(KMOD_SYSCALL_PROBE);
        ok = rc == KMOD_OK && dyn == KMOD_ENOSYS_U64 &&
             probe == KMOD_SYSCALL_PROBE_MAGIC;
        if (!ok) {
            const char *s = "syscall unload did not restore Tier0 boundary";
            uint32_t i = 0;
            for (; s[i] && i + 1 < sizeof(why); i++) why[i] = s[i];
            why[i] = '\0';
        }
    }
    (void)KMOD_EAGAIN_U64;
    report("syscall-namespace-and-override", ok, why);
    return ok;
}

bool case_namespace_reject(void)
{
    /* 动态槽 200 在模块卸载后必须不可调用（ENOSYS），且槽位信息不残留 */
    uint64_t v = call_slot(200);
    bool ok = v == KMOD_ENOSYS_U64;
    report("syscall-namespace-cleanup", ok,
           ok ? "" : "dynamic syscall slot not cleaned on unload");
    return ok;
}

bool case_sched_policy_seat(void)
{
    bool ok = true;
    char why[112] = "";
    const kmod_seat_sched_policy_ops_t *ops =
        (const kmod_seat_sched_policy_ops_t *)KmodSeatCurrent(
            KMOD_SEAT_SCHED_POLICY);
    /* 未加载 .ko 提供者时应看到 Tier0 内建默认策略（policy_version=0） */
    if (!ops || ops->abi_version != KMOD_SEAT_SCHED_POLICY_ABI ||
        ops->ops_size != sizeof(kmod_seat_sched_policy_ops_t)) {
        ok = false;
        const char *s = "default sched policy seat not registered";
        uint32_t i = 0;
        for (; s[i] && i + 1 < sizeof(why); i++) why[i] = s[i];
        why[i] = '\0';
    }
    if (ok && KmodLoadVersion(DEMO_SCHED, "1.0.0", KMOD_LOAD_NORMAL) != KMOD_OK) {
        ok = false;
        const char *s = "load vendor.demo.sched v1 failed";
        uint32_t i = 0;
        for (; s[i] && i + 1 < sizeof(why); i++) why[i] = s[i];
        why[i] = '\0';
    }
    if (ok) {
        ops = (const kmod_seat_sched_policy_ops_t *)KmodSeatCurrent(
            KMOD_SEAT_SCHED_POLICY);
        ok = ops && ops->abi_version == KMOD_SEAT_SCHED_POLICY_ABI &&
             ops->ops_size == sizeof(kmod_seat_sched_policy_ops_t) &&
             ops->policy_version == 1u && ops->identity != nullptr &&
             ops->identity[0] != '\0' && KmodStats()->seat_swaps >= 1;
        if (!ok) {
            const char *s = "seat did not switch to .ko policy v1";
            uint32_t i = 0;
            for (; s[i] && i + 1 < sizeof(why); i++) why[i] = s[i];
            why[i] = '\0';
        }
    }
    if (ok) {
        int32_t rc = KmodHotReplace(DEMO_SCHED, "2.0.0");
        ops = (const kmod_seat_sched_policy_ops_t *)KmodSeatCurrent(
            KMOD_SEAT_SCHED_POLICY);
        ok = rc == KMOD_OK &&
             ops && ops->policy_version == 2u &&
             KmodStats()->hotswaps_ok >= 1;
        if (!ok) {
            const char *s = "sched policy hot replace v1->v2 failed";
            uint32_t i = 0;
            for (; s[i] && i + 1 < sizeof(why); i++) why[i] = s[i];
            why[i] = '\0';
        }
    }
    if (ok) {
        /* 失败版本必须回退：仍为 v2 且模块 READY */
        int32_t rc = KmodHotReplace(DEMO_SCHED, "9.9.9");
        ops = (const kmod_seat_sched_policy_ops_t *)KmodSeatCurrent(
            KMOD_SEAT_SCHED_POLICY);
        ok = (rc == KMOD_ERR_NOTFOUND) && ops && ops->policy_version == 2u &&
             state_is(DEMO_SCHED, KMOD_STATE_READY);
        if (!ok) {
            const char *s = "failed sched policy hot replace not rolled back";
            uint32_t i = 0;
            for (; s[i] && i + 1 < sizeof(why); i++) why[i] = s[i];
            why[i] = '\0';
        }
    }
    if (ok) {
        /* 卸载 .ko -> 座位回退 Tier0 内建默认策略（policy_version=0） */
        int32_t rc = KmodUnload(DEMO_SCHED);
        ops = (const kmod_seat_sched_policy_ops_t *)KmodSeatCurrent(
            KMOD_SEAT_SCHED_POLICY);
        ok = rc == KMOD_OK && ops && ops->policy_version == 0u &&
             KmodModuleState(DEMO_SCHED) < 0 &&
             KmodStats()->seat_swaps >= 2;
        if (!ok) {
            const char *s = "sched policy unload did not restore default";
            uint32_t i = 0;
            for (; s[i] && i + 1 < sizeof(why); i++) why[i] = s[i];
            why[i] = '\0';
        }
    }
    report("sched-policy-seat-hotswap", ok, why);
    return ok;
}

/* B6：分配器策略座位（kernel.seat.alloc.policy.v1）——默认 = 内核 heap 策略
 * （policy_version=0/identity="kernel.heap.default"），.ko 提供者可热替换。
 * 与 sched 座位同构：机制固定（kmalloc/SLAB/SLUB 核心），策略参数可替换。 */
bool case_alloc_policy_seat(void)
{
    bool ok = true;
    char why[112] = "";
    const kmod_seat_alloc_policy_ops_t *ops =
        (const kmod_seat_alloc_policy_ops_t *)KmodSeatCurrent(
            KMOD_SEAT_ALLOC_POLICY);
    if (!ops || ops->abi_version != KMOD_SEAT_ALLOC_POLICY_ABI ||
        ops->ops_size != sizeof(kmod_seat_alloc_policy_ops_t)) {
        ok = false;
        const char *s = "default alloc policy seat not registered";
        uint32_t i = 0;
        for (; s[i] && i + 1 < sizeof(why); i++) why[i] = s[i];
        why[i] = '\0';
    }
    if (ok && KmodLoadVersion(DEMO_ALLOC, "1.0.0", KMOD_LOAD_NORMAL) != KMOD_OK) {
        ok = false;
        const char *s = "load vendor.demo.alloc v1 failed";
        uint32_t i = 0;
        for (; s[i] && i + 1 < sizeof(why); i++) why[i] = s[i];
        why[i] = '\0';
    }
    if (ok) {
        ops = (const kmod_seat_alloc_policy_ops_t *)KmodSeatCurrent(
            KMOD_SEAT_ALLOC_POLICY);
        ok = ops && ops->abi_version == KMOD_SEAT_ALLOC_POLICY_ABI &&
             ops->ops_size == sizeof(kmod_seat_alloc_policy_ops_t) &&
             ops->policy_version == 1u && ops->identity != nullptr &&
             KmodStats()->seat_swaps >= 1;
        if (!ok) {
            const char *s = "seat did not switch to .ko alloc policy v1";
            uint32_t i = 0;
            for (; s[i] && i + 1 < sizeof(why); i++) why[i] = s[i];
            why[i] = '\0';
        }
    }
    if (ok) {
        int32_t rc = KmodHotReplace(DEMO_ALLOC, "2.0.0");
        ops = (const kmod_seat_alloc_policy_ops_t *)KmodSeatCurrent(
            KMOD_SEAT_ALLOC_POLICY);
        ok = rc == KMOD_OK && ops && ops->policy_version == 2u &&
             KmodStats()->hotswaps_ok >= 1;
        if (!ok) {
            const char *s = "alloc policy hot replace v1->v2 failed";
            uint32_t i = 0;
            for (; s[i] && i + 1 < sizeof(why); i++) why[i] = s[i];
            why[i] = '\0';
        }
    }
    if (ok) {
        int32_t rc = KmodHotReplace(DEMO_ALLOC, "9.9.9");
        ops = (const kmod_seat_alloc_policy_ops_t *)KmodSeatCurrent(
            KMOD_SEAT_ALLOC_POLICY);
        ok = (rc == KMOD_ERR_NOTFOUND) && ops && ops->policy_version == 2u &&
             state_is(DEMO_ALLOC, KMOD_STATE_READY);
        if (!ok) {
            const char *s = "failed alloc policy hot replace not rolled back";
            uint32_t i = 0;
            for (; s[i] && i + 1 < sizeof(why); i++) why[i] = s[i];
            why[i] = '\0';
        }
    }
    if (ok) {
        int32_t rc = KmodUnload(DEMO_ALLOC);
        ops = (const kmod_seat_alloc_policy_ops_t *)KmodSeatCurrent(
            KMOD_SEAT_ALLOC_POLICY);
        ok = rc == KMOD_OK && ops && ops->policy_version == 0u &&
             KmodModuleState(DEMO_ALLOC) < 0 &&
             KmodStats()->seat_swaps >= 2;
        if (!ok) {
            const char *s = "alloc policy unload did not restore default";
            uint32_t i = 0;
            for (; s[i] && i + 1 < sizeof(why); i++) why[i] = s[i];
            why[i] = '\0';
        }
    }
    report("alloc-policy-seat-hotswap", ok, why);
    return ok;
}

/* B7：Tier0 共享服务（klib/sync）——.ko 同时使用「直接符号解析」与
 * 「契约 ops 导入」；模块 init 内自检全部通过后导出 magic，本用例从模块
 * 导出表读回 magic 校验，再卸载（导入边释放、ops 槽清零）。 */
bool case_shared_klib(void)
{
    bool ok = true;
    char why[112] = "";
    int32_t rc = KmodLoadVersion(DEMO_KLIB, "1.0.0", KMOD_LOAD_NORMAL);
    if (rc != KMOD_OK) {
        ok = false;
        why_set(why, sizeof(why), "load vendor.demo.klib failed");
    }
    if (ok) {
        kmod_module_t *m = KmodModuleFind(DEMO_KLIB);
        bool found = false;
        uint64_t magic = 0u;
        if (m) {
            for (uint32_t i = 0; i < m->n_exports; i++) {
                kmod_export_t *e = &m->exports[i];
                if (!e->symbol ||
                    !str_eq_local(e->symbol, "klib_selfcheck_magic"))
                    continue;
                if (e->kind != KMOD_SYM_DATA ||
                    e->size != (uint32_t)sizeof(uint64_t) || !e->addr) {
                    why_set(why, sizeof(why),
                            "klib_selfcheck_magic export layout invalid");
                    break;
                }
                magic = *(const uint64_t *)e->addr;
                found = true;
                break;
            }
        }
        ok = found && magic == 0x4B4C49425F5631ull;
        if (!ok && why[0] == '\0')
            why_set(why, sizeof(why),
                    "shared klib/sync selfcheck failed (direct or ops path)");
    }
    if (KmodModuleState(DEMO_KLIB) == (int32_t)KMOD_STATE_READY) {
        int32_t urc = KmodUnload(DEMO_KLIB);
        if (ok && urc != KMOD_OK) {
            ok = false;
            why_set(why, sizeof(why), "unload vendor.demo.klib failed");
        }
    }
    report("shared-klib-contract", ok, why);
    return ok;
}

} /* namespace */

extern "C" int32_t KmodSelfTest(void)
{
    g_pass = 0;
    g_fail = 0;
    kmod_log("[kmod-selftest] begin (abi=%u)\n", (unsigned)KMOD_ABI_VERSION);

    case_boot_topology();
    case_unload_busy();
    case_dep_autoload();
    case_cycle_reject();
    case_cap_reject();
    case_rollback_badinit();
    case_seat_hotswap();
    case_syscall_governance();
    case_namespace_reject();
    case_sched_policy_seat();
    case_alloc_policy_seat();
    case_shared_klib();

    KmodDump();
    g_kmod_stats.selftest_pass = (uint64_t)g_pass;
    g_kmod_stats.selftest_fail = (uint64_t)g_fail;
    if (g_fail == 0) {
        kmod_log("KMOD_SELFTEST PASS %d/%d\n", g_pass, g_pass + g_fail);
        return KMOD_OK;
    }
    kmod_log("KMOD_SELFTEST FAIL %d/%d\n", g_pass, g_pass + g_fail);
    return KMOD_ERR_INVAL;
}
