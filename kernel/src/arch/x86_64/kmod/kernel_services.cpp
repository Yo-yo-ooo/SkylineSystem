//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * kernel_services.cpp —— Tier0 共享服务导出层（klib / sync / algo）
 * ----------------------------------------------------------------------------
 * 把 klib 的公共工具面以两种方式共享给**每个模块**（内建组件与 .ko）：
 *   1) 直接符号：C 链接函数以真实符号名进入 .kout 导出表；.ko 直接调用时
 *      由加载器按 ELF 未定义符号名解析（零 .in 声明）。
 *   2) 契约 ops：kernel.klib.v1 / kernel.sync.v1 / kernel.algo.v1 三个结构体，
 *      模块用 KMOD_IMPORT_KLIB/SYNC/ALGO 导入；治理层校验 contract/abi/size。
 * 本文件是**导出表与 ops 结构的唯一来源**（宿主测试与内核共用同一份
 * initializer，通过 KMOD_SVC_SYM 宏在两类构建间切换符号实现），避免表漂移。
 * ==========================================================================*/
#include <kmod/kmod_internal.h>
#include <kmod/kmod_services.h>

#ifdef __KERNEL_TEST_HOST__
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#else
#include <stdarg.h>
#include <klib/klib.h>
#include <klib/str.h>
#include <klib/kprintf.h>
#include <klib/rnd.h>
#include <klib/algorithm/hmap.h>
#include <klib/algorithm/art.h>
#include <klib/algorithm/queue.h>
#include <klib/fifo.h>
#include <klib/list.h>
#include <mem/heap.h>
#endif

/* 宿主/内核实现切换：
 *   - 内核分支：KLIB/容器/格式化等 C 链接函数直接用真实内核符号（零额外跳转）；
 *   - 宿主分支：使用本文件的 ksvc_* 宿主实现（std libc / 最小桩），保证
 *     kmod 宿主测试能在不链接整个内核的情况下验证共享契约与符号解析；
 *   - 语义特殊或内核里名字不同的工具（kld16/RND/panic/irq/容器包装）在
 *     两个分支都提供 ksvc_* 薄包装（内核分支转发到内核实现）。 */
#ifdef __KERNEL_TEST_HOST__
#define KMOD_SVC_SYM(fn) ksvc_##fn
#else
#define KMOD_SVC_SYM(fn) fn
#endif

/* ================================================================== */
/* 分支实现：宿主（std libc + 最小桩）                                 */
/* ================================================================== */
#ifdef __KERNEL_TEST_HOST__

extern "C" {

/* ---- 普通 C 符号的宿主等价实现（导出表/ops 共用） ---- */
size_t ksvc_strlen(const char *s) { return s ? std::strlen(s) : 0u; }
int32_t ksvc_strcmp(const char *a, const char *b) { return a && b ? std::strcmp(a, b) : (a ? 1 : (b ? -1 : 0)); }
int32_t ksvc_strncmp(const char *a, const char *b, size_t n) { return a && b ? std::strncmp(a, b, n) : 0; }
char *ksvc_strcpy(char *d, const char *s) { return d && s ? std::strcpy(d, s) : d; }
char *ksvc_strncpy(char *d, const char *s, size_t n) { return d && s ? std::strncpy(d, s, n) : d; }
char *ksvc_strcat(char *d, const char *s) { return d && s ? std::strcat(d, s) : d; }
char *ksvc_strchr(const char *s, int32_t c) { return s ? const_cast<char *>(std::strchr(s, c)) : nullptr; }
char *ksvc_strtok(char *s, const char *delim) { return std::strtok(s, delim); }
int32_t ksvc_atoi(char *s) { return s ? std::atoi(s) : 0; }

int32_t ksvc_printf_(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int32_t r = std::vfprintf(stdout, fmt, ap);
    va_end(ap); return r;
}
int32_t ksvc_sprintf_(char *buf, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int32_t r = std::vsprintf(buf, fmt, ap);
    va_end(ap); return r;
}
int32_t ksvc_snprintf_(char *buf, size_t n, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int32_t r = std::vsnprintf(buf, n, fmt, ap);
    va_end(ap); return r;
}
int32_t ksvc_vsnprintf_(char *buf, size_t n, const char *fmt, va_list ap)
{
    return std::vsnprintf(buf, n, fmt, ap);
}
int32_t ksvc_vprintf_(const char *fmt, va_list ap)
{
    return std::vfprintf(stdout, fmt, ap);
}
int32_t ksvc_fctprintf(void (*out)(char, void *), void *arg, const char *fmt, ...)
{
    (void)out; (void)arg; (void)fmt;
    return 0;   /* 宿主仅保证链接；格式化直写由 ksvc_snprintf_ 覆盖 */
}

/* ---- 内核 klib 工具面的宿主实现 ---- */
void ksvc_qsort(void *base, size_t num, size_t width,
                int32_t (*cmp)(const void *, const void *))
{
    /* 简单插入排序（宿主测试数据量小；不引入 qsort_r 依赖） */
    if (!base || !cmp || width == 0u)
        return;
    uint8_t *b = (uint8_t *)base;
    for (size_t i = 1; i < num; i++) {
        for (size_t j = i; j > 0; j--) {
            if (cmp(b + (j - 1) * width, b + j * width) <= 0)
                break;
            for (size_t k = 0; k < width; k++) {
                uint8_t t = b[(j - 1) * width + k];
                b[(j - 1) * width + k] = b[j * width + k];
                b[j * width + k] = t;
            }
        }
    }
}
void ksvc_bitmap_set(uint8_t *bm, uint64_t bit) { if (bm) bm[bit >> 3] |= (uint8_t)(1u << (bit & 7u)); }
void ksvc_bitmap_clear(uint8_t *bm, uint64_t bit) { if (bm) bm[bit >> 3] &= (uint8_t)~(1u << (bit & 7u)); }
bool ksvc_bitmap_get(uint8_t *bm, uint64_t bit) { return bm && (bm[bit >> 3] & (uint8_t)(1u << (bit & 7u))) != 0; }
uint16_t ksvc_kld16(const uint8_t *p) { return p ? (uint16_t)(p[0] | ((uint16_t)p[1] << 8)) : 0; }
uint32_t ksvc_kld32(const uint8_t *p) { return p ? ((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24)) : 0; }
uint64_t ksvc_kld64(const uint8_t *p) { return p ? ((uint64_t)ksvc_kld32(p) | ((uint64_t)ksvc_kld32(p + 4) << 32)) : 0; }

static uint64_t g_host_rng = 0x9E3779B97F4A7C15ull;
static uint64_t ksvc_random_u64(void)
{
    g_host_rng ^= g_host_rng >> 12; g_host_rng ^= g_host_rng << 25; g_host_rng ^= g_host_rng >> 27;
    return g_host_rng * 0x2545F4914F6CDD1Dull;
}
static uint64_t ksvc_random_range(uint64_t lo, uint64_t hi)
{
    if (hi <= lo) return lo;
    return lo + (ksvc_random_u64() % (hi - lo + 1u));
}
void ksvc_panic(const char *message) { std::fprintf(stderr, "[kmod-host][panic] %s\n", message ? message : ""); }

void ksvc_spinlock_lock(int32_t *l) { if (l) *l = 1; }
void ksvc_spinlock_unlock(int32_t *l) { if (l) *l = 0; }
uint64_t ksvc_irq_save(void) { return 0u; }
void ksvc_irq_restore(uint64_t flags) { (void)flags; }
int32_t ksvc_irq_state(void) { return 1; }

void *ksvc_kcalloc(size_t numitems, size_t size) { return std::calloc(numitems, size); }
void *ksvc_krealloc(void *ptr, uint64_t size) { return std::realloc(ptr, (size_t)size); }
void *ksvc_kmalloc_aligned(uint64_t size, uint64_t align)
{
    if (align < sizeof(void *)) align = sizeof(void *);
    return std::aligned_alloc((size_t)align, (size + align - 1u) & ~(align - 1u));
}

/* 容器：宿主测试不依赖这些实现（仅保证链接与签名）；返回 nullptr/0 表示
 * “宿主无容器后端”。真正的容器语义由内核分支的 ksvc_* 转发到 klib 实现。 */
void *ksvc_hashmap_new(size_t, size_t, uint64_t, uint64_t, kmod_hm_hash_fn,
                       kmod_hm_cmp_fn, kmod_hm_free_fn, void *) { return nullptr; }
void ksvc_hashmap_free(void *) {}
void ksvc_hashmap_clear(void *, bool) {}
size_t ksvc_hashmap_count(const void *) { return 0; }
bool ksvc_hashmap_oom(const void *) { return true; }
const void *ksvc_hashmap_get(const void *, const void *) { return nullptr; }
const void *ksvc_hashmap_set(void *, const void *) { return nullptr; }
const void *ksvc_hashmap_delete(void *, const void *) { return nullptr; }
const void *ksvc_hashmap_probe(void *, uint64_t) { return nullptr; }
bool ksvc_hashmap_scan(void *, kmod_hm_scan_fn, void *) { return false; }
bool ksvc_hashmap_iter(void *, size_t *, void **) { return false; }
uint64_t ksvc_hashmap_sip(const void *, size_t, uint64_t, uint64_t) { return 0; }
int32_t ksvc_art_tree_init(void *) { return 0; }
int32_t ksvc_art_tree_destroy(void *) { return 0; }
void *ksvc_art_insert(void *, const uint8_t *, int32_t, void *) { return nullptr; }
void *ksvc_art_insert_no_replace(void *, const uint8_t *, int32_t, void *) { return nullptr; }
void *ksvc_art_delete(void *, const uint8_t *, int32_t) { return nullptr; }
void *ksvc_art_search(const void *, const uint8_t *, int32_t) { return nullptr; }
int32_t ksvc_art_iter(void *, kmod_art_cb_fn, void *) { return 0; }
int32_t ksvc_art_iter_prefix(void *, const uint8_t *, int32_t, kmod_art_cb_fn, void *) { return 0; }
void *ksvc_art_minimum(void *) { return nullptr; }
void *ksvc_art_maximum(void *) { return nullptr; }
void *ksvc_queue_create(void) { return nullptr; }
void *ksvc_queue_append(void *, void *) { return nullptr; }
void *ksvc_queue_dequeue(void *) { return nullptr; }
void ksvc_queue_destroy(void *) {}
void *ksvc_fifo_create(uint64_t, uint64_t) { return nullptr; }
void ksvc_fifo_push(void *, void *) {}
void ksvc_fifo_pop(void *, void *) {}
void ksvc_fifo_get(void *, void *) {}
void *ksvc_list_create(void) { return nullptr; }
void ksvc_list_add(void *, void *) {}
void ksvc_list_remove(void *, void *) {}
void *ksvc_list_iterate(void *, bool) { return nullptr; }
void *ksvc_list_find(void *, void *) { return nullptr; }

} /* extern "C" */

#else  /* ======================= 内核实现分支 ======================= */

extern "C" {

/* 名字与内核符号不同的工具：统一 ksvc_* 薄包装（内联转发） */
uint16_t ksvc_kld16(const uint8_t *p) { return kld_16(p); }
uint32_t ksvc_kld32(const uint8_t *p) { return kld_32(p); }
uint64_t ksvc_kld64(const uint8_t *p) { return kld_64(p); }

void ksvc_bitmap_set(uint8_t *bm, uint64_t bit) { bitmap_set(bm, bit); }
void ksvc_bitmap_clear(uint8_t *bm, uint64_t bit) { bitmap_clear(bm, bit); }
bool ksvc_bitmap_get(uint8_t *bm, uint64_t bit) { return bitmap_get(bm, bit); }

uint64_t ksvc_random_u64(void) { return RND::lehmer64(); }
uint64_t ksvc_random_range(uint64_t lo, uint64_t hi)
{
    if (hi <= lo) return lo;
    return lo + (ksvc_random_u64() % (hi - lo + 1u));
}
void ksvc_panic(const char *message) { Panic(message != nullptr ? message : "module panic"); }

uint64_t ksvc_irq_save(void) { return Interrupt::SaveMask() ? 1ull : 0ull; }
void ksvc_irq_restore(uint64_t flags) { Interrupt::Restore(flags != 0ull); }
int32_t ksvc_irq_state(void) { return Interrupt::State(); }

/* 容器：全部转发到 klib（句柄保持不透明 void*，布局由内核私有） */
void *ksvc_hashmap_new(size_t elsize, size_t cap, uint64_t s0, uint64_t s1,
                       kmod_hm_hash_fn hash, kmod_hm_cmp_fn cmp,
                       kmod_hm_free_fn elf, void *ud)
{
    return (void *)hashmap_new(elsize, cap, s0, s1,
                               (uint64_t (*)(const void *, uint64_t, uint64_t))hash,
                               (int32_t (*)(const void *, const void *, void *))cmp,
                               (void (*)(void *))elf, ud);
}
void ksvc_hashmap_free(void *m) { hashmap_free((struct hashmap *)m); }
void ksvc_hashmap_clear(void *m, bool update_cap) { hashmap_clear((struct hashmap *)m, update_cap); }
size_t ksvc_hashmap_count(const void *m) { return hashmap_count((const struct hashmap *)m); }
bool ksvc_hashmap_oom(const void *m) { return hashmap_oom((struct hashmap *)m); }
const void *ksvc_hashmap_get(const void *m, const void *item) { return hashmap_get((const struct hashmap *)m, item); }
const void *ksvc_hashmap_set(void *m, const void *item) { return hashmap_set((struct hashmap *)m, item); }
const void *ksvc_hashmap_delete(void *m, const void *item) { return hashmap_delete((struct hashmap *)m, item); }
const void *ksvc_hashmap_probe(void *m, uint64_t pos) { return hashmap_probe((struct hashmap *)m, pos); }
bool ksvc_hashmap_scan(void *m, kmod_hm_scan_fn iter, void *ud)
{
    return hashmap_scan((struct hashmap *)m,
                        (bool (*)(const void *, void *))iter, ud);
}
bool ksvc_hashmap_iter(void *m, size_t *i, void **item) { return hashmap_iter((struct hashmap *)m, i, item); }
uint64_t ksvc_hashmap_sip(const void *data, size_t len, uint64_t s0, uint64_t s1)
{
    return hashmap_sip(data, len, s0, s1);
}

int32_t ksvc_art_tree_init(void *t) { return art_tree_init((art_tree *)t); }
int32_t ksvc_art_tree_destroy(void *t) { return art_tree_destroy((art_tree *)t); }
void *ksvc_art_insert(void *t, const uint8_t *key, int32_t len, void *v) { return art_insert((art_tree *)t, key, len, v); }
void *ksvc_art_insert_no_replace(void *t, const uint8_t *key, int32_t len, void *v) { return art_insert_no_replace((art_tree *)t, key, len, v); }
void *ksvc_art_delete(void *t, const uint8_t *key, int32_t len) { return art_delete((art_tree *)t, key, len); }
void *ksvc_art_search(const void *t, const uint8_t *key, int32_t len) { return art_search((const art_tree *)t, key, len); }
int32_t ksvc_art_iter(void *t, kmod_art_cb_fn cb, void *data)
{
    return art_iter((art_tree *)t, (art_callback)cb, data);
}
int32_t ksvc_art_iter_prefix(void *t, const uint8_t *prefix, int32_t plen,
                             kmod_art_cb_fn cb, void *data)
{
    return art_iter_prefix((art_tree *)t, prefix, plen, (art_callback)cb, data);
}
void *ksvc_art_minimum(void *t) { return art_minimum((art_tree *)t); }
void *ksvc_art_maximum(void *t) { return art_maximum((art_tree *)t); }

void *ksvc_queue_create(void) { return (void *)Queue::Create(); }
void *ksvc_queue_append(void *q, void *data) { return (void *)Queue::Append((queue_t *)q, data); }
void *ksvc_queue_dequeue(void *q) { return Queue::Dequeue((queue_t *)q); }
void ksvc_queue_destroy(void *q) { Queue::Destroy((queue_t *)q); }

void *ksvc_fifo_create(uint64_t cap, uint64_t item_size) { return (void *)FIFO::Create(cap, item_size); }
void ksvc_fifo_push(void *f, void *val) { FIFO::Push((fifo *)f, val); }
void ksvc_fifo_pop(void *f, void *buffer) { FIFO::Pop((fifo *)f, buffer); }
void ksvc_fifo_get(void *f, void *buffer) { FIFO::Get((fifo *)f, buffer); }

void *ksvc_list_create(void) { return (void *)List::Create(); }
void ksvc_list_add(void *l, void *v) { List::Add((list *)l, v); }
void ksvc_list_remove(void *l, void *item) { List::Remove((list *)l, (list_item *)item); }
void *ksvc_list_iterate(void *l, bool wrap) { return List::Iterate((list *)l, wrap); }
void *ksvc_list_find(void *l, void *v) { return (void *)List::Find((list *)l, v); }

} /* extern "C" */

#endif /* __KERNEL_TEST_HOST__ */

/* ================================================================== */
/* 共享 ops 实例（唯一来源；内核 .kout 与宿主测试表共用）             */
/* ================================================================== */
static kmod_klib_ops_t g_kmod_klib_ops_v1 = {
    KMOD_KLIB_ABI_V1,
    (uint32_t)sizeof(kmod_klib_ops_t),
    KMOD_SVC_SYM(strlen), KMOD_SVC_SYM(strcmp), KMOD_SVC_SYM(strncmp),
    KMOD_SVC_SYM(strcpy), KMOD_SVC_SYM(strncpy), KMOD_SVC_SYM(strcat),
    KMOD_SVC_SYM(strchr), KMOD_SVC_SYM(strtok), KMOD_SVC_SYM(atoi),
    KMOD_SVC_SYM(sprintf_), KMOD_SVC_SYM(snprintf_),
    KMOD_SVC_SYM(qsort),
    ksvc_bitmap_set, ksvc_bitmap_clear, ksvc_bitmap_get,
    ksvc_kld16, ksvc_kld32, ksvc_kld64,
    ksvc_random_u64, ksvc_random_range, ksvc_panic,
};

static kmod_sync_ops_t g_kmod_sync_ops_v1 = {
    KMOD_SYNC_ABI_V1,
    (uint32_t)sizeof(kmod_sync_ops_t),
    KMOD_SVC_SYM(spinlock_lock), KMOD_SVC_SYM(spinlock_unlock),
    ksvc_irq_save, ksvc_irq_restore, ksvc_irq_state,
};

static kmod_algo_ops_t g_kmod_algo_ops_v1 = {
    KMOD_ALGO_ABI_V1,
    (uint32_t)sizeof(kmod_algo_ops_t),
    ksvc_hashmap_new, ksvc_hashmap_free, ksvc_hashmap_count,
    ksvc_hashmap_get, ksvc_hashmap_set, ksvc_hashmap_delete,
    ksvc_hashmap_scan, ksvc_hashmap_iter, ksvc_hashmap_sip,
    ksvc_art_tree_init, ksvc_art_tree_destroy,
    ksvc_art_insert, ksvc_art_delete, ksvc_art_search,
    ksvc_art_iter, ksvc_art_iter_prefix,
    ksvc_art_minimum, ksvc_art_maximum,
    ksvc_queue_create, ksvc_queue_append, ksvc_queue_dequeue, ksvc_queue_destroy,
    ksvc_fifo_create, ksvc_fifo_push, ksvc_fifo_pop, ksvc_fifo_get,
    ksvc_list_create, ksvc_list_add, ksvc_list_remove,
    ksvc_list_iterate, ksvc_list_find,
};

/* ABI 尺寸冻结：任何字段增删/顺序变化必须升 ABI 版本（v2）并保留 v1。 */
static_assert(sizeof(kmod_klib_ops_t) > 0, "klib ops layout");
static_assert(sizeof(kmod_sync_ops_t) > 0, "sync ops layout");
static_assert(sizeof(kmod_algo_ops_t) > 0, "algo ops layout");
static_assert(sizeof(kmod_klib_ops_t) == 8u + 21u * sizeof(void *),
              "klib ops v1 field count freeze (21 function pointers)");

/* ================================================================== */
/* 导出表：内核 .kout 段 / 宿主静态表（同一 initializer）              */
/* ================================================================== */
#define KMOD_SERVICE_CONTRACT_ENTRY(contract_, ops_var_, abi_) \
    { contract_, contract_, (void *)&ops_var_, (uint32_t)sizeof(ops_var_), \
      (uint32_t)(abi_), KMOD_SYM_CONTRACT, 0u, 0u, 0u }

#define KMOD_SERVICE_FUNC_ENTRY(fn_, contract_) \
    { #fn_, contract_, (void *)(KMOD_SVC_SYM(fn_)), 0u, 1u, KMOD_SYM_FUNC, 0u, 0u, 0u }

#ifdef __KERNEL_TEST_HOST__
#define KMOD_SERVICE_EXPORT_ATTR
#else
#define KMOD_SERVICE_EXPORT_ATTR \
    __attribute__((used, section(KMOD_SEC_KERNEL_OUT), aligned(8)))
#endif

KMOD_SERVICE_EXPORT_ATTR
static kmod_export_t g_kmod_service_exports[] = {
    /* ---- 三个服务契约（size 供模块 expect_size 强校验） ---- */
    KMOD_SERVICE_CONTRACT_ENTRY(KMOD_CONTRACT_KERNEL_KLIB, g_kmod_klib_ops_v1, KMOD_KLIB_ABI_V1),
    KMOD_SERVICE_CONTRACT_ENTRY(KMOD_CONTRACT_KERNEL_SYNC, g_kmod_sync_ops_v1, KMOD_SYNC_ABI_V1),
    KMOD_SERVICE_CONTRACT_ENTRY(KMOD_CONTRACT_KERNEL_ALGO, g_kmod_algo_ops_v1, KMOD_ALGO_ABI_V1),

    /* ---- klib 字符串 ---- */
    KMOD_SERVICE_FUNC_ENTRY(strlen, KMOD_CONTRACT_KERNEL_KLIB),
    KMOD_SERVICE_FUNC_ENTRY(strcmp, KMOD_CONTRACT_KERNEL_KLIB),
    KMOD_SERVICE_FUNC_ENTRY(strncmp, KMOD_CONTRACT_KERNEL_KLIB),
    KMOD_SERVICE_FUNC_ENTRY(strcpy, KMOD_CONTRACT_KERNEL_KLIB),
    KMOD_SERVICE_FUNC_ENTRY(strncpy, KMOD_CONTRACT_KERNEL_KLIB),
    KMOD_SERVICE_FUNC_ENTRY(strcat, KMOD_CONTRACT_KERNEL_KLIB),
    KMOD_SERVICE_FUNC_ENTRY(strchr, KMOD_CONTRACT_KERNEL_KLIB),
    KMOD_SERVICE_FUNC_ENTRY(strtok, KMOD_CONTRACT_KERNEL_KLIB),
    KMOD_SERVICE_FUNC_ENTRY(atoi, KMOD_CONTRACT_KERNEL_KLIB),

    /* ---- kprintf 格式化族（与 kernel.log.v1 的 kprintf_ 互补） ---- */
    KMOD_SERVICE_FUNC_ENTRY(printf_, KMOD_CONTRACT_KERNEL_KLIB),
    KMOD_SERVICE_FUNC_ENTRY(sprintf_, KMOD_CONTRACT_KERNEL_KLIB),
    KMOD_SERVICE_FUNC_ENTRY(snprintf_, KMOD_CONTRACT_KERNEL_KLIB),
    KMOD_SERVICE_FUNC_ENTRY(vsnprintf_, KMOD_CONTRACT_KERNEL_KLIB),
    KMOD_SERVICE_FUNC_ENTRY(vprintf_, KMOD_CONTRACT_KERNEL_KLIB),
    KMOD_SERVICE_FUNC_ENTRY(fctprintf, KMOD_CONTRACT_KERNEL_KLIB),

    /* ---- 容器：hashmap ---- */
    KMOD_SERVICE_FUNC_ENTRY(hashmap_new, KMOD_CONTRACT_KERNEL_KLIB),
    KMOD_SERVICE_FUNC_ENTRY(hashmap_free, KMOD_CONTRACT_KERNEL_KLIB),
    KMOD_SERVICE_FUNC_ENTRY(hashmap_clear, KMOD_CONTRACT_KERNEL_KLIB),
    KMOD_SERVICE_FUNC_ENTRY(hashmap_count, KMOD_CONTRACT_KERNEL_KLIB),
    KMOD_SERVICE_FUNC_ENTRY(hashmap_oom, KMOD_CONTRACT_KERNEL_KLIB),
    KMOD_SERVICE_FUNC_ENTRY(hashmap_get, KMOD_CONTRACT_KERNEL_KLIB),
    KMOD_SERVICE_FUNC_ENTRY(hashmap_set, KMOD_CONTRACT_KERNEL_KLIB),
    KMOD_SERVICE_FUNC_ENTRY(hashmap_delete, KMOD_CONTRACT_KERNEL_KLIB),
    KMOD_SERVICE_FUNC_ENTRY(hashmap_probe, KMOD_CONTRACT_KERNEL_KLIB),
    KMOD_SERVICE_FUNC_ENTRY(hashmap_scan, KMOD_CONTRACT_KERNEL_KLIB),
    KMOD_SERVICE_FUNC_ENTRY(hashmap_iter, KMOD_CONTRACT_KERNEL_KLIB),
    KMOD_SERVICE_FUNC_ENTRY(hashmap_sip, KMOD_CONTRACT_KERNEL_KLIB),

    /* ---- 容器：ART ---- */
    KMOD_SERVICE_FUNC_ENTRY(art_tree_init, KMOD_CONTRACT_KERNEL_KLIB),
    KMOD_SERVICE_FUNC_ENTRY(art_tree_destroy, KMOD_CONTRACT_KERNEL_KLIB),
    KMOD_SERVICE_FUNC_ENTRY(art_insert, KMOD_CONTRACT_KERNEL_KLIB),
    KMOD_SERVICE_FUNC_ENTRY(art_insert_no_replace, KMOD_CONTRACT_KERNEL_KLIB),
    KMOD_SERVICE_FUNC_ENTRY(art_delete, KMOD_CONTRACT_KERNEL_KLIB),
    KMOD_SERVICE_FUNC_ENTRY(art_search, KMOD_CONTRACT_KERNEL_KLIB),
    KMOD_SERVICE_FUNC_ENTRY(art_iter, KMOD_CONTRACT_KERNEL_KLIB),
    KMOD_SERVICE_FUNC_ENTRY(art_iter_prefix, KMOD_CONTRACT_KERNEL_KLIB),
    KMOD_SERVICE_FUNC_ENTRY(art_minimum, KMOD_CONTRACT_KERNEL_KLIB),
    KMOD_SERVICE_FUNC_ENTRY(art_maximum, KMOD_CONTRACT_KERNEL_KLIB),

    /* ---- 同步原语（kernel.sync.v1） ---- */
    KMOD_SERVICE_FUNC_ENTRY(spinlock_lock, KMOD_CONTRACT_KERNEL_SYNC),
    KMOD_SERVICE_FUNC_ENTRY(spinlock_unlock, KMOD_CONTRACT_KERNEL_SYNC),

    /* ---- 内存扩展（kernel.mem.v1 追加符号，不改变既有 2 个符号） ---- */
    KMOD_SERVICE_FUNC_ENTRY(kcalloc, KMOD_CONTRACT_KERNEL_MEM),
    KMOD_SERVICE_FUNC_ENTRY(krealloc, KMOD_CONTRACT_KERNEL_MEM),
    KMOD_SERVICE_FUNC_ENTRY(kmalloc_aligned, KMOD_CONTRACT_KERNEL_MEM),
};

#ifdef __KERNEL_TEST_HOST__
extern "C" void KmodKernelServiceExports(const kmod_export_t **out,
                                         uint32_t *count)
{
    if (out)
        *out = g_kmod_service_exports;
    if (count)
        *count = (uint32_t)(sizeof(g_kmod_service_exports) / sizeof(g_kmod_service_exports[0]));
}
#endif
