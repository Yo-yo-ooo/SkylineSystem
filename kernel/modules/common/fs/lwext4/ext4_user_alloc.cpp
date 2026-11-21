// SPDX-FileCopyrightText: 2026 Yo-yo-ooo
// SPDX-License-Identifier: GPL-2.0-only
/* lwext4 的分配钩子。
 *
 * 背景: ext4_malloc 直接映射到 kmalloc。内存紧张时 kmalloc 会失败,
 * ext4_buf_alloc() 拿不到 4 KiB 的块缓冲就返回 NULL, 于是**文件读到一半
 * 就返回 0** —— 实测 80 MiB 下 DOOM 报
 *     W_ReadLump: only read 8192 of 16297 on lump 438
 * (在 11 MB 文件的 3.4 MB 偏移处, 而文件还有 7.7 MB 没读)。
 * 这不是瞬时压力: 重试 32 次仍然失败, 因为它依赖的堆已经彻底拿不到页了。
 *
 * 对策: 给 lwext4 一个**独立的静态应急池**。正常情况仍然用 kmalloc (行为
 * 不变, 不占额外内存); 只有当 kmalloc 失败时才从池里切, 保证文件系统在
 * 极端内存压力下仍能继续工作。池是 BSS (256 KiB), 不强占物理内存。 */

#include <fs/lwext4/ext4_types.h>
#include <klib/kio.h>

extern "C" void *kmalloc(uint64_t size);
extern "C" void  kfree(void *ptr);
extern "C" void *krealloc(void *ptr, uint64_t size);

namespace {

constexpr uint32_t POOL_BYTES = 256u * 1024u;
constexpr uint32_t HDR_ALIGN  = 16u;

struct PoolHdr {
    uint32_t size;      /* 数据区字节数 */
    uint32_t used;      /* 1 = 已分配 */
};

__attribute__((aligned(16))) uint8_t g_pool[POOL_BYTES];
uint32_t g_used;                 /* 已切出去的字节数 (含头部) */
volatile int g_lock;

inline void pool_lock() {
    while (__atomic_test_and_set(&g_lock, __ATOMIC_ACQUIRE)) {
        __asm__ __volatile__("pause");
    }
}
inline void pool_unlock() { __atomic_clear(&g_lock, __ATOMIC_RELEASE); }

inline bool in_pool(const void *p) {
    const uint8_t *b = (const uint8_t *)p;
    return b >= g_pool && b < g_pool + POOL_BYTES;
}

inline uint32_t round_up(uint32_t n) {
    return (n + HDR_ALIGN - 1) & ~(HDR_ALIGN - 1u);
}

} // namespace

/* 从池里 first-fit 分配; 返回 nullptr 表示池也满了 */
static void *pool_alloc(uint32_t need) {
    uint32_t want = round_up(need);
    pool_lock();
    uint32_t off = 0;
    while (off + sizeof(PoolHdr) <= g_used) {
        PoolHdr *h = (PoolHdr *)(g_pool + off);
        if (!h->used && h->size >= want) {
            h->used = 1;
            void *ret = g_pool + off + sizeof(PoolHdr);
            pool_unlock();
            return ret;
        }
        off += sizeof(PoolHdr) + h->size;
    }
    uint32_t total = round_up((uint32_t)sizeof(PoolHdr)) + want;
    if (g_used + total > POOL_BYTES) { pool_unlock(); return nullptr; }
    PoolHdr *h = (PoolHdr *)(g_pool + g_used);
    h->size = want;
    h->used = 1;
    void *ret = g_pool + g_used + sizeof(PoolHdr);
    g_used += total;
    pool_unlock();
    return ret;
}

static void pool_free(void *p) {
    if (!in_pool(p)) return;
    PoolHdr *h = (PoolHdr *)((uint8_t *)p - sizeof(PoolHdr));
    pool_lock();
    h->used = 0;
    pool_unlock();
}

extern "C" void *ext4_user_malloc(size_t size) {
    void *p = kmalloc((uint64_t)size);
    if (p) return p;
    return pool_alloc((uint32_t)size);
}

extern "C" void *ext4_user_calloc(size_t nmemb, size_t size) {
    size_t total = nmemb * size;
    void *p = ext4_user_malloc(total);
    if (p) {
        uint8_t *b = (uint8_t *)p;
        for (size_t i = 0; i < total; i++) b[i] = 0;
    }
    return p;
}

extern "C" void ext4_user_free(void *p) {
    if (!p) return;
    if (in_pool(p)) { pool_free(p); return; }
    kfree(p);
}

extern "C" void *ext4_user_realloc(void *p, size_t size) {
    if (!p) return ext4_user_malloc(size);
    if (size == 0) { ext4_user_free(p); return nullptr; }
    if (in_pool(p)) {
        PoolHdr *h = (PoolHdr *)((uint8_t *)p - sizeof(PoolHdr));
        if (h->size >= size) return p;
        void *np = ext4_user_malloc(size);
        if (!np) return nullptr;
        uint8_t *d = (uint8_t *)np, *s = (uint8_t *)p;
        for (size_t i = 0; i < h->size; i++) d[i] = s[i];
        pool_free(p);
        return np;
    }
    return krealloc(p, (uint64_t)size);
}
