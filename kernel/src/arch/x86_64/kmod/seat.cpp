//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * seat.cpp —— Tier1 座位(seat) 注册表 + 原子切换 + 失败回退
 * ============================================================================
 * 原则 5：开放策略，不开放核心机制。
 *   - 座位名/ops 结构尺寸/abi 版本由 Tier0 定义（kmod_seats.h），模块必须
 *     逐字段匹配；内核只做注册、校验、原子切换、回退。
 *   - 当前座位：KMOD_SEAT_RANDOM（随机数提供者），Tier0 默认实现始终存在；
 *     模块可用 KMOD_EXPORT_SEAT 取代，消费入口 KmodSeatRandomU64/Fill
 *     原子读取"模块优先、内核兜底"的当前实现。
 *   - 模块提供者调用期间持有 epoch（进入失败则回退内核默认 RNG），保证
 *     热替换/卸载先等 epoch 排空再切换/释放。
 * 宿主测试（-D__KERNEL_TEST_HOST__）：随机源用 xorshift64*，不依赖 HPET。
 * ==========================================================================*/
#include <kmod/kmod_internal.h>

#ifndef __KERNEL_TEST_HOST__
#include <klib/rnd.h>
#include <arch/x86_64/drivers/hpet/hpet.h>
#endif

namespace {

/* ------------------------------------------------------------------ */
/* 小工具                                                              */
/* ------------------------------------------------------------------ */
bool str_eq(const char *a, const char *b)
{
    if (!a || !b)
        return false;
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

void copy_str(char *dst, uint32_t cap, const char *src)
{
    uint32_t i = 0u;
    if (src) {
        while (i + 1u < cap && src[i]) {
            dst[i] = src[i];
            i++;
        }
    }
    dst[i] = '\0';
}

/* ------------------------------------------------------------------ */
/* Tier0 默认随机源                                                    */
/*   内核：沿用 sys_getrandom 的既有风格（RND::rand LCG × HPET 纳秒），      */
/*         再用 splitmix64 finalizer 打散成 64 位。                      */
/*   宿主：xorshift64*（确定性、可重放）。                                */
/* ------------------------------------------------------------------ */
uint64_t fallback_u64(void)
{
#ifdef __KERNEL_TEST_HOST__
    static uint64_t s = 0x9e3779b97f4a7c15ull;
    s ^= s >> 12;
    s ^= s << 25;
    s ^= s >> 27;
    return s * 2685821657736338717ull;
#else
    const uint64_t ns = HPET::GetTimeNS();
    const uint64_t ms = kmod_uptime_ms();
    const uint64_t lcg = (uint64_t)(RND::rand(1337, 0, 0x8FFFFFFF) % 65535);
    uint64_t v = (lcg * (ns % 65535ull)) ^ (ns << 21) ^ (ms << 7);
    v ^= v >> 33;
    v *= 0xff51afd7ed558ccdull;
    v ^= v >> 33;
    return v;
#endif
}

/* 用 u64 生成器按小端字节填 [buf, buf+len)；不要求对齐 */
void fill_from_u64(uint64_t (*next)(void), uint8_t *buf, uint64_t len)
{
    uint64_t i = 0u;
    while (len - i >= 8u) {
        const uint64_t v = next();
        for (uint32_t k = 0u; k < 8u; k++)
            buf[i + k] = (uint8_t)(v >> (k * 8u));
        i += 8u;
    }
    if (i < len) {
        const uint64_t v = next();
        const uint64_t rem = len - i;
        for (uint64_t k = 0u; k < rem; k++)
            buf[i + k] = (uint8_t)(v >> (k * 8u));
    }
}

void fallback_fill(uint8_t *buf, uint64_t len)
{
    fill_from_u64(fallback_u64, buf, len);
}

/* Tier0 内核默认座位实现（v1）。abi/ops_size 必须与 kmod_seats.h 一致。 */
const kmod_seat_random_ops_t g_kernel_random_ops = {
    KMOD_SEAT_RANDOM_ABI,
    (uint32_t)sizeof(kmod_seat_random_ops_t),
    fallback_u64,
    fallback_fill,
};

/* ------------------------------------------------------------------ */
/* 座位注册表                                                          */
/* ------------------------------------------------------------------ */
kmod_seat_t g_seats[KMOD_SEAT_MAX];
uint32_t g_seat_count = 0u;
volatile int32_t g_seat_lock = 0;

/* 调用方必须持有 g_seat_lock（或接受引导期单线程假设） */
kmod_seat_t *seat_find_locked(const char *name)
{
    for (uint32_t i = 0; i < g_seat_count; i++) {
        if (str_eq(g_seats[i].name, name))
            return &g_seats[i];
    }
    return nullptr;
}

/* 模块指针 -> g_kmod_modules[] 下标（未注册返回 -1） */
int32_t module_index(const kmod_module_t *m)
{
    if (!m)
        return -1;
    for (uint32_t i = 0; i < KMOD_MAX_MODULES; i++) {
        if (&g_kmod_modules[i] == m)
            return (int32_t)i;
    }
    return -1;
}

/* 座位实现深度校验：随机数座位是 Tier0 机制契约，逐字段硬校验。 */
int32_t validate_seat_impl(const kmod_seat_t *seat, const void *addr)
{
    if (!addr)
        return KMOD_ERR_INVAL;
    if (str_eq(seat->name, KMOD_SEAT_RANDOM)) {
        const kmod_seat_random_ops_t *ops =
            reinterpret_cast<const kmod_seat_random_ops_t *>(addr);
        if (ops->abi_version != KMOD_SEAT_RANDOM_ABI)
            return KMOD_ERR_VERSION;
        if (ops->ops_size != (uint32_t)sizeof(kmod_seat_random_ops_t))
            return KMOD_ERR_CONTRACT;
        if (!ops->u64)
            return KMOD_ERR_INVAL;
    }
    return KMOD_OK;
}

/* ------------------------------------------------------------------ */
/* 当前随机提供者解析                                                  */
/* ------------------------------------------------------------------ */
struct RandomRef {
    const kmod_seat_random_ops_t *ops;
    const char *module_name; /* 非空 = 模块提供者（调用需 epoch 保护） */
};

bool random_ref(RandomRef *out)
{
    kmod_seat_t *seat = KmodSeatFind(KMOD_SEAT_RANDOM);
    if (!seat)
        return false;
    const void *ops_addr =
        __atomic_load_n(&seat->module_ops, __ATOMIC_ACQUIRE);
    if (ops_addr) {
        const int32_t prov =
            __atomic_load_n(&seat->provider, __ATOMIC_ACQUIRE);
        if (prov < 0 || (uint32_t)prov >= g_kmod_module_count)
            return false; /* provider 不可见：安全回退内核默认源 */
        out->ops = reinterpret_cast<const kmod_seat_random_ops_t *>(ops_addr);
        out->module_name = g_kmod_modules[prov].name;
        return true;
    }
    if (seat->kernel_ops) {
        out->ops = reinterpret_cast<const kmod_seat_random_ops_t *>(
            seat->kernel_ops);
        out->module_name = nullptr;
        return true;
    }
    return false;
}

} /* namespace */

/* ================================================================== */
/* 初始化：注册 KMOD_SEAT_RANDOM 的 Tier0 默认实现                     */
/* ================================================================== */
extern "C" int32_t KmodSeatInit(void)
{
    uint64_t flags = kmod_lock_acquire(&g_seat_lock);
    for (uint32_t i = 0; i < KMOD_SEAT_MAX; i++) {
        kmod_seat_t *s = &g_seats[i];
        s->name[0] = '\0';
        s->abi_version = 0u;
        s->ops_size = 0u;
        s->allowed_caps = 0ull;
        s->kernel_ops = nullptr;
        s->provider = -1;
        s->module_ops = nullptr;
        s->generation = 0u;
        s->lock = 0;
    }
    g_seat_count = 0u;
    kmod_lock_release(&g_seat_lock, flags);

    int32_t rc = KmodSeatRegisterKernel(
        KMOD_SEAT_RANDOM, KMOD_SEAT_RANDOM_ABI, &g_kernel_random_ops,
        (uint32_t)sizeof(kmod_seat_random_ops_t), KMOD_CAP_SEAT_PROVIDER);
    if (rc != KMOD_OK) {
        kmod_log("[kmod] seat init FAILED: random seat rc=%d\n", (int)rc);
        return rc;
    }
    kmod_log("[kmod] seats ready: %u seat(s), random abi=%u ops_size=%u\n",
             (unsigned)KmodSeatCount(), (unsigned)KMOD_SEAT_RANDOM_ABI,
             (unsigned)sizeof(kmod_seat_random_ops_t));
    return KMOD_OK;
}

/* ================================================================== */
/* 内核默认实现注册（Tier0 固定；同名重复注册=幂等更新）               */
/* ================================================================== */
extern "C" int32_t KmodSeatRegisterKernel(const char *name,
                                          uint32_t abi_version,
                                          const void *ops, uint32_t ops_size,
                                          uint64_t allowed_caps)
{
    if (!name || !name[0] || !ops || ops_size == 0u)
        return KMOD_ERR_INVAL;
    uint32_t len = 0u;
    while (name[len])
        len++;
    if (len >= KMOD_CONTRACT_MAX)
        return KMOD_ERR_NAME;

    uint64_t flags = kmod_lock_acquire(&g_seat_lock);
    kmod_seat_t *seat = seat_find_locked(name);
    if (!seat) {
        if (g_seat_count >= KMOD_SEAT_MAX) {
            kmod_lock_release(&g_seat_lock, flags);
            return KMOD_ERR_NOMEM;
        }
        seat = &g_seats[g_seat_count];
        g_seat_count++;
        copy_str(seat->name, KMOD_CONTRACT_MAX, name);
    }
    seat->abi_version = abi_version;
    seat->ops_size = ops_size;
    seat->allowed_caps = allowed_caps;
    seat->kernel_ops = ops;
    if (__atomic_load_n(&seat->provider, __ATOMIC_RELAXED) < 0)
        __atomic_store_n(&seat->module_ops, nullptr, __ATOMIC_RELEASE);
    seat->generation++;
    kmod_lock_release(&g_seat_lock, flags);
    return KMOD_OK;
}

extern "C" kmod_seat_t *KmodSeatFind(const char *name)
{
    if (!name)
        return nullptr;
    uint64_t flags = kmod_lock_acquire(&g_seat_lock);
    kmod_seat_t *seat = seat_find_locked(name);
    kmod_lock_release(&g_seat_lock, flags);
    return seat;
}

extern "C" const kmod_seat_t *KmodSeatAt(uint32_t index)
{
    if (index >= g_seat_count)
        return nullptr;
    return &g_seats[index];
}

extern "C" uint32_t KmodSeatCount(void)
{
    return g_seat_count;
}

extern "C" const void *KmodSeatCurrent(const char *name)
{
    kmod_seat_t *seat = KmodSeatFind(name);
    if (!seat)
        return nullptr;
    const void *ops = __atomic_load_n(&seat->module_ops, __ATOMIC_ACQUIRE);
    if (ops)
        return ops;
    return seat->kernel_ops;
}

/* ================================================================== */
/* 提交阶段：扫描模块的 KMOD_SYM_SEAT 导出并安装                        */
/*   失败时本次已安装的部分逆序回滚（governance 事务还会统一兜底）。     */
/* ================================================================== */
extern "C" int32_t KmodSeatAttachModule(kmod_module_t *m)
{
    if (!m)
        return KMOD_ERR_INVAL;
    const int32_t idx = module_index(m);
    if (idx < 0) {
        kmod_log("[kmod] seat attach: module %s not in registry\n", m->name);
        return KMOD_ERR_INVAL;
    }
    if (m->n_exports > KMOD_MAX_EXPORTS)
        return KMOD_ERR_FORMAT;

    struct SeatUndo {
        uint32_t seat_index;
        int32_t provider;
        const void *module_ops;
        uint32_t generation;
    };
    SeatUndo undo[KMOD_MAX_EXPORTS];
    uint32_t n_undo = 0u;
    uint32_t n_installed = 0u;
    int32_t rc = KMOD_OK;

    uint64_t flags = kmod_lock_acquire(&g_seat_lock);
    for (uint32_t i = 0; i < m->n_exports; i++) {
        const kmod_export_t *e = &m->exports[i];
        if (e->kind != KMOD_SYM_SEAT)
            continue;
        if (!e->symbol || !e->symbol[0]) {
            rc = KMOD_ERR_INVAL;
            break;
        }
        kmod_seat_t *seat = seat_find_locked(e->symbol);
        if (!seat) {
            rc = KMOD_ERR_NOTFOUND;
            break;
        }
        /* 契约名（若有）必须与座位名一致；不解释语义，只做机械校验 */
        if (e->contract && e->contract[0] && !str_eq(e->contract, seat->name)) {
            rc = KMOD_ERR_CONTRACT;
            break;
        }
        if (e->abi_version != seat->abi_version) {
            rc = KMOD_ERR_VERSION;
            break;
        }
        if (e->size != seat->ops_size) {
            rc = KMOD_ERR_CONTRACT;
            break;
        }
        if (seat->allowed_caps != 0ull &&
            (m->caps_granted & seat->allowed_caps) == 0ull) {
            rc = KMOD_ERR_CAP;
            break;
        }
        /* 先做实现结构校验（abi/ops_size/回调非空），再判占用冲突：
         * 这样诊断日志能给出根因，而不是一律 BUSY。 */
        rc = validate_seat_impl(seat, e->addr);
        if (rc != KMOD_OK)
            break;
        if (__atomic_load_n(&seat->provider, __ATOMIC_ACQUIRE) >= 0 &&
            __atomic_load_n(&seat->provider, __ATOMIC_ACQUIRE) != idx) {
            rc = KMOD_ERR_BUSY; /* 已有其它模块提供者 */
            break;
        }
        if (n_undo >= KMOD_MAX_EXPORTS) {
            rc = KMOD_ERR_NOMEM;
            break;
        }
        undo[n_undo].seat_index =
            (uint32_t)(seat - &g_seats[0]); /* seat 必然落在数组内 */
        undo[n_undo].provider =
            __atomic_load_n(&seat->provider, __ATOMIC_ACQUIRE);
        undo[n_undo].module_ops =
            __atomic_load_n(&seat->module_ops, __ATOMIC_ACQUIRE);
        undo[n_undo].generation = seat->generation;
        n_undo++;

        __atomic_store_n(&seat->module_ops, e->addr, __ATOMIC_RELEASE);
        __atomic_store_n(&seat->provider, idx, __ATOMIC_RELEASE);
        seat->generation++;
        n_installed++;
    }

    if (rc != KMOD_OK) {
        while (n_undo > 0u) {
            n_undo--;
            kmod_seat_t *seat = &g_seats[undo[n_undo].seat_index];
            __atomic_store_n(&seat->module_ops, undo[n_undo].module_ops,
                             __ATOMIC_RELEASE);
            __atomic_store_n(&seat->provider, undo[n_undo].provider,
                             __ATOMIC_RELEASE);
            seat->generation = undo[n_undo].generation;
        }
        kmod_lock_release(&g_seat_lock, flags);
        kmod_log("[kmod] seat attach %s FAILED rc=%d\n", m->name, (int)rc);
        return rc;
    }
    kmod_lock_release(&g_seat_lock, flags);

    if (n_installed) {
        __atomic_fetch_add(&g_kmod_stats.seat_swaps, (uint64_t)n_installed,
                           __ATOMIC_RELAXED);
        kmod_log("[kmod] seat attach: %s installed %u implementation(s)\n",
                 m->name, (unsigned)n_installed);
    }
    return KMOD_OK;
}

extern "C" void KmodSeatDetachModule(kmod_module_t *m)
{
    const int32_t idx = module_index(m);
    if (idx < 0)
        return; /* 未注册/重复调用安全 no-op */
    uint64_t flags = kmod_lock_acquire(&g_seat_lock);
    for (uint32_t i = 0; i < g_seat_count; i++) {
        kmod_seat_t *seat = &g_seats[i];
        if (__atomic_load_n(&seat->provider, __ATOMIC_ACQUIRE) != idx)
            continue;
        __atomic_store_n(&seat->module_ops, nullptr, __ATOMIC_RELEASE);
        __atomic_store_n(&seat->provider, -1, __ATOMIC_RELEASE);
        seat->generation++;
    }
    kmod_lock_release(&g_seat_lock, flags);
}

extern "C" int32_t KmodSeatRollbackToKernel(const char *name)
{
    if (!name)
        return KMOD_ERR_INVAL;
    uint64_t flags = kmod_lock_acquire(&g_seat_lock);
    kmod_seat_t *seat = seat_find_locked(name);
    if (!seat) {
        kmod_lock_release(&g_seat_lock, flags);
        return KMOD_ERR_NOTFOUND;
    }
    if (__atomic_load_n(&seat->provider, __ATOMIC_ACQUIRE) >= 0) {
        __atomic_store_n(&seat->module_ops, nullptr, __ATOMIC_RELEASE);
        __atomic_store_n(&seat->provider, -1, __ATOMIC_RELEASE);
        seat->generation++;
    }
    kmod_lock_release(&g_seat_lock, flags);
    return KMOD_OK;
}

/* ================================================================== */
/* 消费入口：sys_getrandom 等 Tier0 路径调用                            */
/* ================================================================== */
extern "C" uint64_t KmodSeatRandomU64(void)
{
    RandomRef ref = {nullptr, nullptr};
    if (!random_ref(&ref) || !ref.ops || !ref.ops->u64)
        return fallback_u64();
    if (!ref.module_name)
        return ref.ops->u64(); /* Tier0 内核默认实现，无需 epoch */

    if (KmodEpochEnterInternal(ref.module_name) != KMOD_OK)
        return fallback_u64(); /* 模块正在静默/替换：回退内核默认源 */
    const uint64_t v = ref.ops->u64();
    KmodEpochExitInternal(ref.module_name);
    return v;
}

extern "C" void KmodSeatRandomFill(uint8_t *buf, uint64_t len)
{
    if (!buf || len == 0u)
        return; /* NULL/空长度：按 memcpy 语义直接返回 */

    RandomRef ref = {nullptr, nullptr};
    if (!random_ref(&ref) || !ref.ops) {
        fallback_fill(buf, len);
        return;
    }
    const bool epoch = (ref.module_name != nullptr);
    if (epoch && KmodEpochEnterInternal(ref.module_name) != KMOD_OK) {
        fallback_fill(buf, len);
        return;
    }
    if (ref.ops->fill)
        ref.ops->fill(buf, len);
    else if (ref.ops->u64)
        fill_from_u64(ref.ops->u64, buf, len); /* fill=NULL 回退循环 u64 */
    else
        fallback_fill(buf, len);
    if (epoch)
        KmodEpochExitInternal(ref.module_name);
}
