//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
#include <mem/heap.h>
#include <mem/pmm.h>

#ifdef __x86_64__
#include <arch/x86_64/smp/smp.h>
#endif
#include <pdef.h>

#include "heap_internal.h"
#include <klib/kprintf.h>

/* B6：分配器策略座位（Tier0 机制固定，Tier1 策略可替换）。
 * 内核构建才引入 kmod 座位 ABI/注册入口；宿主 slub 测试（tests/slub）
 * 不链接 kmod，策略恒缺席 → 默认路径与历史逐位一致（也不引入 kmod 头开销）。 */
#ifndef __KERNEL_TEST_HOST__
#include <kmod/kmod_seats.h>

extern "C" const void *KmodSeatCurrent(const char *name);

/* 读取当前分配策略 ops；NULL/ABI 不符/identity 缺失一律视为"无策略"。
 * 仅在冷路径调用（>=4 KiB large path 或分配失败后的 OOM 路径）。 */
static const kmod_seat_alloc_policy_ops_t *heap_alloc_policy(void)
{
    const void *raw = KmodSeatCurrent(KMOD_SEAT_ALLOC_POLICY);
    if (!raw)
        return nullptr;
    const kmod_seat_alloc_policy_ops_t *ops =
        (const kmod_seat_alloc_policy_ops_t *)raw;
    if (ops->abi_version != KMOD_SEAT_ALLOC_POLICY_ABI ||
        ops->ops_size != (uint32_t)sizeof(kmod_seat_alloc_policy_ops_t) ||
        !ops->identity)
        return nullptr;
    return ops;
}

/* 策略 large 阈值解析：返回 0 = 沿用内核默认（不改变路径）。
 * 策略不得把 large 门限压到小对象区（< 4 KiB），越界值被 clamp。 */
static uint64_t heap_policy_large_threshold(uint64_t request)
{
    const kmod_seat_alloc_policy_ops_t *ops = heap_alloc_policy();
    if (!ops || !ops->large_alloc_threshold)
        return 0ull;
    uint64_t t = ops->large_alloc_threshold(request,
                                            KMOD_ALLOC_LARGE_DEFAULT_THRESHOLD);
    if (t != 0ull && t < KMOD_ALLOC_LARGE_MIN)
        t = KMOD_ALLOC_LARGE_MIN;
    return t;
}
#endif /* !__KERNEL_TEST_HOST__ */

/* 临时诊断: 按调用者统计内核堆分配字节数 (64 槽哈希)。
   目的: 找出"DOOM 启动后消失的 ~110 MiB" 是哪些内核调用点要的。
   只累加, 不在这里打印 —— 打印放在 lwip 的定时统计里, 避免重入。 */
#define KM_TRACK_SLOTS 64
volatile uint64_t g_km_bytes[KM_TRACK_SLOTS];
volatile uint64_t g_km_calls[KM_TRACK_SLOTS];
volatile uint64_t g_km_ra[KM_TRACK_SLOTS];
volatile uint64_t g_km_total;

static inline void km_track(uint64_t size, void *ra) {
    uint64_t h = ((uint64_t)ra >> 4) & (KM_TRACK_SLOTS - 1);
    __atomic_add_fetch(&g_km_bytes[h], size, __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_km_calls[h], 1, __ATOMIC_RELAXED);
    __atomic_store_n(&g_km_ra[h], (uint64_t)ra, __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_km_total, size, __ATOMIC_RELAXED);
}

#ifdef __x86_64__
extern "C" kmem_cache *kmem_cache_create(const char *n, uint64_t sz, uint64_t al) { return SLUB::Create(n, sz, al ? al : 8); }
extern "C" void        kmem_cache_destroy(kmem_cache *c) { SLUB::Destroy(c); }
extern "C" void       *kmem_cache_alloc(kmem_cache *c) { return SLUB::Alloc(c); }
extern "C" void        kmem_cache_free(kmem_cache *c, void *o) { SLUB::Free(c, o); }
#endif // __x86_64__

#ifdef __KERNEL_TEST_HOST__
/* 宿主测试(ASAN): fc_entry_free 毒化已释放对象; 分配时解毒,
   使 ASAN 只报告"释放后未经复用即被访问"的真实 UAF */
extern "C" void __asan_unpoison_memory_region(const volatile void *addr, size_t size);

/* 全量分配环形日志: 诊断"活地址被再次分配/覆盖"。
   显式开关: 单线程套件开启; 多线程/TSAN 套件关闭(避免日志写入竞态)。 */
struct _kalloc_rec { uintptr_t p; uint32_t size; uint32_t ra; };
static struct _kalloc_rec g_kalloc_log[1 << 20];
static uint32_t g_kalloc_idx = 0;
static int g_kalloc_log_on = 0;
extern "C" void kalloc_log_enable(int on) { g_kalloc_log_on = on; }
extern "C" void kalloc_query(void *p) {
    printf_("[KMQ] 查询 %p 的全部分配记录 (最近 2^20 次):\n", p);
    uint32_t found = 0;
    for (uint32_t i = 0; i < (1u << 20); i++) {
        if (g_kalloc_log[i].p == (uintptr_t)p) {
            printf_("[KMQ]   p=%p size=%u ra=0x%x\n", p, g_kalloc_log[i].size, g_kalloc_log[i].ra);
            if (++found >= 8) break;
        }
    }
    printf_("[KMQ] 共 %u 条\n", found);
}
#endif

extern "C" void *kmalloc(uint64_t size) {
#ifdef __x86_64__
    /* B6：分配器策略座位（仅冷路径）。
       - 只有 request >= KMOD_ALLOC_LARGE_MIN(4 KiB) 才查询 large 阈值；
         小对象（<4 KiB）热路径**零策略查询**，与原路径指令级等价。
       - 默认（无提供者 / 回调返回 0）large_direct=false：仍先尝试 SLUB
         小对象分配器，然后 SLAB large 路径 —— 与历史行为逐位一致。
       - 策略显式返回阈值 t 且 request >= t 时，跳过 SLUB 尝试直接走
         large 路径：SLUB kmalloc 尺寸类仅 16..1024 B，>=4 KiB 必然 miss，
         因此该"跳过"不改变结果，只改变失败尝试次数。 */
    bool large_direct = false;
#ifndef __KERNEL_TEST_HOST__
    if (unlikely(size >= KMOD_ALLOC_LARGE_MIN)) {
        const uint64_t t = heap_policy_large_threshold(size);
        if (t != 0ull && size >= t)
            large_direct = true;
    }
#endif
    if (!large_direct) {
        void *p = SLUB::Kmalloc((size_t)size);    // small object: lock-free SLUB path
        if (likely(p)) {
#ifdef __KERNEL_TEST_HOST__
            __asan_unpoison_memory_region(p, (size_t)size);
            if (g_kalloc_log_on)
                g_kalloc_log[g_kalloc_idx++ & ((1u << 20) - 1)] = {(uintptr_t)p, (uint32_t)size, (uint32_t)(uintptr_t)__builtin_return_address(0)};
#endif
            km_track(size, __builtin_return_address(0));
            return p;
        }
    }
#endif
    void *q = SLAB::Alloc(size);               // >1024 B large object, or SLUB not online
#ifndef __KERNEL_TEST_HOST__
    if (unlikely(!q)) {
        /* B6：OOM/回退策略（仅分配失败路径查询；默认 0/NULL 保持原语义：
         * 不重试，返回 NULL）。策略返回 KMOD_ALLOC_OOM_RETRY 才允许重试一次。 */
        const kmod_seat_alloc_policy_ops_t *ops = heap_alloc_policy();
        if (ops && ops->oom_policy &&
            ops->oom_policy(size, KMOD_ALLOC_OOM_KIND_KMALLOC) ==
                KMOD_ALLOC_OOM_RETRY) {
            q = SLAB::Alloc(size);
        }
    }
#endif
#ifdef __KERNEL_TEST_HOST__
    if (q) {
        __asan_unpoison_memory_region(q, (size_t)size);
        if (g_kalloc_log_on)
            g_kalloc_log[g_kalloc_idx++ & ((1u << 20) - 1)] = {(uintptr_t)q, (uint32_t)size, (uint32_t)(uintptr_t)__builtin_return_address(0)};
    }
#endif
    if (q) km_track(size, __builtin_return_address(0));
    return q;
}

/* ---- kfree 指针合法性闸门 (防御层, QEMU 实测得来) ---------------------
 * 背景: SLUB::TryFree / SLAB::Free 都会先用 "ptr & ~0xFFF" 取页头再读 magic,
 * 对**非内核堆指针**完全没有校验:
 *   · 用户态地址 (0x7000_...) -> SMAP 生效, 内核读即 #PF
 *   · 低位地址 (1..4095)      -> 页 0 未映射, #PF
 *   实测现场: DOOM 读 IWAD 时 file_cache_promote 把一个非堆指针丢进 kfree,
 *   内核态 #PF -> idt_exception_handler -> Panic -> hcf; 而 panic 发生时
 *   正持着文件缓存锁, 其余 CPU 在 spinlock_lock 里永久自旋 => 整机假死
 *   (stall watchdog 报 CPU0 file_cache_get / CPU3 file_cache_fsync 同锁)。
 *
 * 原则: 宁可漏掉一次释放 (并大声报出来), 也不能把整机打死。
 * 内核半区下界取 HHDM/直映基址 0xFFFF800000000000 —— 堆对象、大页、
 * 静态区都在其上; 用户地址、NULL 附近的野指针全部拒绝。 */
/* ---- kfree 历史环 (见 heap_internal.h) ---- */
struct kfree_hist_ent { void *p; void *caller; };
static kfree_hist_ent g_kfree_hist[KFREE_HIST_N];
static uint32_t       g_kfree_hist_idx;

extern "C" void kfree_hist_record(void *p, void *caller) {
    uint32_t i = __atomic_add_fetch(&g_kfree_hist_idx, 1, __ATOMIC_RELAXED) % KFREE_HIST_N;
    g_kfree_hist[i].p = p;
    g_kfree_hist[i].caller = caller;
}

extern "C" void kfree_hist_dump(const char *why) {
    kerrorln("[kfree-hist] %s (最近 %d 次释放, 新 -> 旧)", why, (int)KFREE_HIST_N);
    uint32_t base = __atomic_load_n(&g_kfree_hist_idx, __ATOMIC_RELAXED);
    for (uint32_t k = 0; k < KFREE_HIST_N; k++) {
        uint32_t i = (base + KFREE_HIST_N - 1u - k) % KFREE_HIST_N;
        if (!g_kfree_hist[i].p) continue;
        kerrorln("  #%u ptr=%p caller=%p", k, g_kfree_hist[i].p, g_kfree_hist[i].caller);
    }
}

static bool kfree_ptr_in_kernel_half(void *ptr) {
    const uint64_t v = (uint64_t)ptr;
    if (v >= 0xFFFF800000000000ull) return true;

    /* **绝对不要在这里打印**。
       kfree 会在各种临界区里被调用 (文件缓存锁、目录锁、dbgout 路径...),
       而 kerrorln 会去抢 ptf_lock, e9/serial 镜像又会抢 dbgout_lock ——
       与"持有 dbgout_lock 的线程回头 printf"构成 AB-BA 交叉死锁。
       QEMU 实测: 4 个核全部停在 spinlock_lock, 3 核争 ptf_lock、1 核争
       dbgout_lock, 连 stall watchdog 所在核也卡死, 整机无声停摆。
       只做无锁记账, 需要时由 fatal 路径的 kfree_hist_dump 打印。 */
    static uint32_t rejected = 0;
    static void    *last_ptr = nullptr;
    static void    *last_caller = nullptr;
    (void)__atomic_add_fetch(&rejected, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&last_ptr, ptr, __ATOMIC_RELAXED);
    __atomic_store_n(&last_caller, __builtin_return_address(0), __ATOMIC_RELAXED);
    return false;
}

extern "C" void kfree(void *ptr) {
    if (ptr) kfree_hist_record(ptr, __builtin_return_address(0));
#ifndef __KERNEL_TEST_HOST__
    /* 内核专属: 宿主测试里 heap 地址是 0x55xx.../0x7fxx..., 高 16 位并非
       0xFFFF, 这道闸门会把**所有**正常释放都拒掉 (泄漏 + 测试自杀)。
       内核地址空间校验只在真正的内核构建里启用。 */
    if (ptr && !kfree_ptr_in_kernel_half(ptr)) return;   /* 拒收, 见上 */
#endif
#ifdef __x86_64__
    if (SLUB::TryFree(ptr)) return;           // page magic routes SLUB vs SLAB/large
#endif
    SLAB::Free(ptr);
}

extern "C" void *krealloc(void *ptr, uint64_t size) {
    if (!ptr) return kmalloc(size);
    if (size == 0) { kfree(ptr); return nullptr; }
#ifdef __x86_64__
    size_t cap = SLUB::TryGetSize(ptr);
    if (cap) {                                // SLUB-backed object
        if ((uint64_t)cap >= size) return ptr;
        void *np = kmalloc(size);
        if (np) { __memcpy(np, ptr, cap); kfree(ptr); }
        return np;
    }
#endif
    return SLAB::Realloc(ptr, size);          // SLAB/large object keeps its semantics
}

extern "C" void *kmalloc_aligned(uint64_t size, uint64_t align) {
#ifdef __x86_64__
    if (SLUB::KmallocOnline()) {
        if (align == 0 || (align & (align - 1)) != 0) return nullptr;
        uint64_t need = (size > align) ? size : align;
        void *p = SLUB::Kmalloc((size_t)need);
        if (p) return p;                      // power-of-two class >= need >= align
    }
#endif
    return SLAB::AllocAligned(size, align);
}

uint64_t GetPtrPointAreaSize(void *ptr) {
#ifdef __x86_64__
    size_t s = SLUB::TryGetSize(ptr);
    if (s) return (uint64_t)s;
#endif
    return SLAB::GetSize(ptr, false);
}

extern "C" void *kcalloc(size_t numitems, size_t size) {
    if (numitems == 0 || size == 0) return nullptr;

    size_t total;
    if (__builtin_mul_overflow(numitems, size, &total)) {
        return nullptr;
    }

    void *ptr = kmalloc(total);
    if (ptr) {
        _memset(ptr, 0, total);
    }
    return ptr;
}