//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * capability.cpp —— kmod 能力策略层（Tier0 治理的一部分，可宿主测试）
 * ----------------------------------------------------------------------------
 * 职责：把模块清单里的 required_caps / optional_caps 机械地映射成授权结果。
 * 这里不是安全沙箱边界，而是**可审计的授权记录**：谁声明了什么、内核给了
 * 什么、拒了什么，全部写回 kmod_module_t.caps_* 与统计计数器。
 *
 * 策略（kmod_internal.h 定义）：
 *   KMOD_POLICY_STRICT   默认。只有内置可信清单/TrustAdd 登记的模块可加载，
 *                        最终授予 = 清单能力 ∩ 声明能力；required 有缺口即拒绝。
 *   KMOD_POLICY_OPEN     诊断用。未知模块按声明授予（required+optional 全给），
 *                        但仍拒绝 KMOD_CAP_RESERVED_CORE 与未定义能力位。
 *   KMOD_POLICY_DISABLED 关闭模块加载。所有请求返回 KMOD_ERR_POLICY。
 *
 * 能力位合法性：KMOD_CAP_ALL_KNOWN(=0x7FFFFFFFF) 是"预留"范围，其中真正定义
 * 的只有 bit0..bit18；bit19..bit34 与 bit35+ 都属于未定义位，声明即拒绝
 * （前向兼容只能通过提升 KMOD_ABI_VERSION 完成）。
 * ==========================================================================*/
#include <kmod/kmod_internal.h>

#include <stdint.h>
#include <stddef.h>

namespace {

constexpr uint32_t CAP_TRUST_MAX = 32u;

/* 已定义的合法能力位（bit0..bit18）。 */
constexpr uint64_t CAP_DEFINED_MASK = (1ull << 19) - 1ull;

struct cap_name_pair {
    uint64_t bit;
    const char *name;
};

const cap_name_pair kCapNames[] = {
    { KMOD_CAP_MEM_ALLOC, "mem_alloc" },
    { KMOD_CAP_MEM_DMA, "mem_dma" },
    { KMOD_CAP_LOG, "log" },
    { KMOD_CAP_IRQ, "irq" },
    { KMOD_CAP_MMIO, "mmio" },
    { KMOD_CAP_DEVICE, "device" },
    { KMOD_CAP_BLOCK, "block" },
    { KMOD_CAP_FS, "fs" },
    { KMOD_CAP_NET, "net" },
    { KMOD_CAP_SCHED_POLICY, "sched_policy" },
    { KMOD_CAP_ALLOC_POLICY, "alloc_policy" },
    { KMOD_CAP_SYSCALL_IMPL, "syscall_impl" },
    { KMOD_CAP_SYSCALL_OVERRIDE, "syscall_override" },
    { KMOD_CAP_SEAT_PROVIDER, "seat_provider" },
    { KMOD_CAP_HOT_REPLACE, "hot_replace" },
    { KMOD_CAP_MODULE_LOAD, "module_load" },
    { KMOD_CAP_MODULE_UNLOAD, "module_unload" },
    { KMOD_CAP_DEBUG_MEM, "debug_mem" },
    { KMOD_CAP_EXPERIMENTAL, "experimental" },
    { KMOD_CAP_RESERVED_CORE, "reserved_core" },
};

/* 内置可信清单：demo 模块按任务约定的最小能力集登记。清单之外的模块在
 * STRICT 策略下一律拒绝；测试/扩展用 KmodCapTrustAdd 追加。 */
struct builtin_trust {
    const char *name;
    uint64_t caps;
};

const builtin_trust kBuiltinTrust[] = {
    { "vendor.demo.hello", KMOD_CAP_LOG | KMOD_CAP_MEM_ALLOC },
    { "vendor.demo.greeter", KMOD_CAP_LOG | KMOD_CAP_MEM_ALLOC },
    { "vendor.demo.prng", KMOD_CAP_SEAT_PROVIDER | KMOD_CAP_HOT_REPLACE |
                              KMOD_CAP_LOG | KMOD_CAP_MEM_ALLOC },
    { "vendor.demo.seat", KMOD_CAP_SEAT_PROVIDER | KMOD_CAP_HOT_REPLACE |
                              KMOD_CAP_LOG | KMOD_CAP_MEM_ALLOC },
    { "vendor.demo.syscall", KMOD_CAP_SYSCALL_IMPL | KMOD_CAP_SYSCALL_OVERRIDE |
                                 KMOD_CAP_HOT_REPLACE | KMOD_CAP_LOG |
                                 KMOD_CAP_MEM_ALLOC },
    { "vendor.demo.badinit", KMOD_CAP_LOG | KMOD_CAP_MEM_ALLOC },
    { "vendor.demo.badcap", KMOD_CAP_LOG | KMOD_CAP_MEM_ALLOC },
    { "vendor.demo.cyclea", KMOD_CAP_LOG | KMOD_CAP_MEM_ALLOC },
    { "vendor.demo.cycleb", KMOD_CAP_LOG | KMOD_CAP_MEM_ALLOC },
    { "vendor.demo.cycle.a", KMOD_CAP_LOG | KMOD_CAP_MEM_ALLOC },
    { "vendor.demo.cycle.b", KMOD_CAP_LOG | KMOD_CAP_MEM_ALLOC },
    /* B5：调度策略座位提供者 demo（v1/v2 同名多版本；HOT_REPLACE 必须授） */
    { "vendor.demo.sched", KMOD_CAP_SEAT_PROVIDER | KMOD_CAP_HOT_REPLACE |
                               KMOD_CAP_LOG | KMOD_CAP_MEM_ALLOC },
    /* B6：分配器策略座位提供者 demo（v1/v2 同名多版本） */
    { "vendor.demo.alloc", KMOD_CAP_SEAT_PROVIDER | KMOD_CAP_HOT_REPLACE |
                               KMOD_CAP_LOG | KMOD_CAP_MEM_ALLOC |
                               KMOD_CAP_ALLOC_POLICY },
    /* B7：Tier0 共享服务（klib/sync）消费者 demo */
    { "vendor.demo.klib", KMOD_CAP_LOG | KMOD_CAP_MEM_ALLOC },
};

/* 信任表存储：entry.name 指向 names[i]（稳定地址；TrustAdd 原地更新 caps）。 */
char g_trust_names[CAP_TRUST_MAX][KMOD_NAME_MAX];
kmod_trusted_entry_t g_trust_entries[CAP_TRUST_MAX];
uint32_t g_trust_count = 0u;
bool g_cap_inited = false;
uint32_t g_cap_mode = KMOD_POLICY_STRICT;
volatile int32_t g_cap_lock = 0;

uint64_t str_len(const char *s)
{
    uint64_t n = 0;
    if (!s)
        return 0;
    while (s[n])
        n++;
    return n;
}

bool str_eq(const char *a, const char *b)
{
    if (!a || !b)
        return a == b;
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

/* 调用方必须已持 g_cap_lock。把 name 拷贝进信任表；不覆盖已有同名项。 */
bool trust_push_locked(const char *name, uint64_t caps)
{
    if (g_trust_count >= CAP_TRUST_MAX)
        return false;
    if (!name || !name[0])
        return false;
    uint64_t len = str_len(name);
    if (len >= (uint64_t)KMOD_NAME_MAX)
        return false;
    for (uint32_t i = 0; i < g_trust_count; i++) {
        if (str_eq(g_trust_entries[i].name, name)) {
            g_trust_entries[i].caps = caps;
            return true;
        }
    }
    char *dst = g_trust_names[g_trust_count];
    for (uint64_t i = 0; i <= len; i++)
        dst[i] = name[i];
    g_trust_entries[g_trust_count].name = dst;
    g_trust_entries[g_trust_count].caps = caps;
    g_trust_count++;
    return true;
}

/* 调用方必须已持 g_cap_lock。 */
void cap_init_locked(void)
{
    if (g_cap_inited)
        return;
    g_trust_count = 0u;
    for (uint32_t i = 0; i < (uint32_t)(sizeof(kBuiltinTrust) / sizeof(kBuiltinTrust[0]));
         i++) {
        (void)trust_push_locked(kBuiltinTrust[i].name, kBuiltinTrust[i].caps);
    }
    g_cap_inited = true;
}

/* 调用方必须已持 g_cap_lock。 */
const uint64_t *trust_find_locked(const char *name)
{
    for (uint32_t i = 0; i < g_trust_count; i++) {
        if (str_eq(g_trust_entries[i].name, name))
            return &g_trust_entries[i].caps;
    }
    return nullptr;
}

void set_out(uint64_t *p, uint64_t v)
{
    if (p)
        *p = v;
}

} /* namespace */

extern "C" {

void KmodCapInit(void)
{
    uint64_t flags = kmod_lock_acquire(&g_cap_lock);
    cap_init_locked();
    g_cap_mode = KMOD_POLICY_STRICT;
    kmod_lock_release(&g_cap_lock, flags);
    kmod_log("[kmod] capability: %u trusted entries, STRICT policy\n",
             (unsigned)g_trust_count);
}

void KmodCapSetMode(uint32_t mode)
{
    if (mode != KMOD_POLICY_STRICT && mode != KMOD_POLICY_OPEN &&
        mode != KMOD_POLICY_DISABLED) {
        kmod_log("[kmod] capability: ignore invalid policy mode %u\n",
                 (unsigned)mode);
        return;
    }
    __atomic_store_n(&g_cap_mode, mode, __ATOMIC_RELEASE);
    kmod_log("[kmod] capability: policy -> %s\n",
             mode == KMOD_POLICY_STRICT ? "strict"
                                        : (mode == KMOD_POLICY_OPEN ? "open"
                                                                    : "disabled"));
}

uint32_t KmodCapMode(void)
{
    return __atomic_load_n(&g_cap_mode, __ATOMIC_ACQUIRE);
}

bool KmodCapIsReserved(uint64_t caps)
{
    return (caps & KMOD_CAP_RESERVED_CORE) != 0ull;
}

int32_t KmodCapGrant(const char *module_name, uint64_t required,
                     uint64_t optional, uint64_t *granted_out,
                     uint64_t *denied_out)
{
    set_out(granted_out, 0ull);
    set_out(denied_out, 0ull);
    if (!module_name || module_name[0] == '\0')
        return KMOD_ERR_NAME;

    uint64_t declared = required | optional;
    /* 保留位/未定义位出现在 required 或 optional —— 整单拒绝（不静默截断）。 */
    if (KmodCapIsReserved(declared) || (declared & ~CAP_DEFINED_MASK) != 0ull) {
        set_out(denied_out, declared);
        return KMOD_ERR_CAP;
    }

    uint32_t mode = KmodCapMode();
    if (mode == KMOD_POLICY_DISABLED)
        return KMOD_ERR_POLICY;

    uint64_t granted = 0ull;
    uint64_t denied = 0ull;
    int32_t rc = KMOD_OK;

    uint64_t flags = kmod_lock_acquire(&g_cap_lock);
    cap_init_locked();

    if (mode == KMOD_POLICY_STRICT) {
        const uint64_t *mask = trust_find_locked(module_name);
        if (!mask) {
            /* 可信清单之外：STRICT 下是策略拒绝，不是能力不足 */
            denied = declared;
            rc = KMOD_ERR_POLICY;
        } else {
            granted = declared & *mask;
            denied = declared & ~(*mask);
            if ((required & ~granted) != 0ull)
                rc = KMOD_ERR_CAP;
        }
    } else { /* KMOD_POLICY_OPEN：按声明全授予（保留/未定义位已在上面拒绝） */
        granted = declared;
        denied = 0ull;
    }

    kmod_lock_release(&g_cap_lock, flags);

    set_out(granted_out, granted);
    set_out(denied_out, denied);
    return rc;
}

const char *KmodCapName(uint64_t bit)
{
    for (uint32_t i = 0; i < (uint32_t)(sizeof(kCapNames) / sizeof(kCapNames[0])); i++) {
        if (kCapNames[i].bit == bit)
            return kCapNames[i].name;
    }
    return "unknown";
}

int32_t KmodCapTrustAdd(const char *name, uint64_t mask)
{
    if (!name || name[0] == '\0')
        return KMOD_ERR_NAME;
    int32_t rc = KmodNameValidate(name);
    if (rc != KMOD_OK)
        return rc;
    if ((mask & ~CAP_DEFINED_MASK) != 0ull || KmodCapIsReserved(mask))
        return KMOD_ERR_CAP; /* 可信清单本身也不得携带保留/未定义位 */

    uint64_t flags = kmod_lock_acquire(&g_cap_lock);
    cap_init_locked();
    bool ok = trust_push_locked(name, mask);
    uint32_t count = g_trust_count;
    kmod_lock_release(&g_cap_lock, flags);
    if (!ok)
        return KMOD_ERR_NOMEM; /* 表满 */
    kmod_log("[kmod] capability: trusted %s mask=0x%llx (n=%u)\n", name,
             (unsigned long long)mask, (unsigned)count);
    return KMOD_OK;
}

uint32_t KmodCapTrustedCount(void)
{
    uint64_t flags = kmod_lock_acquire(&g_cap_lock);
    cap_init_locked();
    uint32_t n = g_trust_count;
    kmod_lock_release(&g_cap_lock, flags);
    return n;
}

const kmod_trusted_entry_t *KmodCapTrustedAt(uint32_t index)
{
    uint64_t flags = kmod_lock_acquire(&g_cap_lock);
    cap_init_locked();
    const kmod_trusted_entry_t *e = nullptr;
    if (index < g_trust_count)
        e = &g_trust_entries[index];
    kmod_lock_release(&g_cap_lock, flags);
    return e;
}

} /* extern "C" */
