// SPDX-FileCopyrightText: 2026 Yo-yo-ooo
// SPDX-License-Identifier: GPL-2.0-only
// motified form https://github.com/tidwall/hashmap.c
#include <klib/klibc.h>
#include <klib/algorithm/hmap.h>
#include <klib/algorithm/rbtree.h>
#include <pdef.h>

/* ============================================================
 *  优化总览
 *  ------------------------------------------------------------
 *  [分支] likely/unlikely 只标结构性不对称分支:
 *         分配失败、阈值触发、哈希碰撞(48bit 空间)、参数守卫。
 *         ~50/50 数据分支(更新 vs 新增、桶空 vs 非空)一律不标。
 *  [预取] 单操作: 桶头行 → root.node / root.hint 两级目标,
 *         掩盖 rb_search 首次下降前的依赖缺失;
 *         批量: scan/iter/free/clear/resize 顺序流预取下一桶;
 *         resize 重插阶段预取 nodes[i+1];
 *         单向指针追逐(rb_next/Morris)无前瞻窗口, 不预取。
 *  [分配] resize: Morris O(n) 收集替代 O(n log n) 逐个 erase;
 *         先释放旧表再填充, 小表走栈缓冲零分配;
 *         扩容失败退避重试且操作继续(树桶优雅降级);
 *         缩容一步落在负载中带, 避免阶梯式连续缩容;
 *         节点空闲链: delete 回收 / insert 复用, 增删抖动零分配。
 * ============================================================ */




/* 节点空闲链开关: 置 0 恢复 "delete 即 kfree" 的严格释放语义 */
#ifndef HASHMAP_NODE_CACHE
#define HASHMAP_NODE_CACHE 1
#endif

/* resize 收集阶段: 节点数不超过该值时用栈缓冲, 一次 kmalloc 都省掉 */
#ifndef HASHMAP_STACK_COLLECT
#define HASHMAP_STACK_COLLECT 64
#endif

// Load factor configuration
#define GROW_AT   60
#define SHRINK_AT 10

#ifndef HASHMAP_LOAD_FACTOR
#define HASHMAP_LOAD_FACTOR GROW_AT
#endif

// Fixed-point definition for load factor calculation
#define LF_BITS    10
#define LF_ONE     (1U << LF_BITS)
#define LF_HALF    (LF_ONE >> 1)

#define PCT_TO_LF(pct)  ((uint16_t)(((pct) * LF_ONE + 50) / 100))
#define SHRINK_LF  PCT_TO_LF(SHRINK_AT)

static uint8_t clamp_load_factor_pct(uint8_t percent, uint8_t default_percent) {
    if (percent == 0) percent = default_percent;
    if (percent < 50) percent = 50;
    if (percent > 95) percent = 95;
    return percent;
}

static inline size_t lf_threshold(size_t nbuckets, uint16_t lf_fixed) {
    return (size_t)(((uint64_t)nbuckets * (uint64_t)lf_fixed) >> LF_BITS);
}

static inline uint64_t clip_hash(uint64_t hash) {
    return hash & 0xFFFFFFFFFFFFULL;
}

/* 上取整到 >=16 的 2 的幂 (统一 new/resize/shrink 三处 round-up 逻辑) */
static inline size_t hashmap_round_pow2(size_t v) {
    size_t p = 16;
    while (p < v) p <<= 1;
    return p;
}

// Hashmap node stored inside red-black tree
// Core design: store map pointer inside node to avoid modifying rb_tree.h comparator signature
struct hashmap_node {
    union {
        rb_node_t      rb;          // 树内链接 (MUST be first member)
        struct hashmap_node *cache_next; // [分配] 回收态复用头 8 字节穿空闲链, 不增内存
    };
    uint64_t       hash;        // 48-bit clipped hash value
    struct hashmap *map;        // Parent hashmap, replaces global context
    const void    *key_ptr;     // Valid only when is_proxy != 0
    uint8_t        is_proxy;    // 0 = real node, 1 = lookup proxy node
    uint8_t        _pad[7];     // Padding to align flexible array item[] to 8-byte boundary
    char           item[];      // Flexible array: user stored data for real node
};

static inline void *hashmap_node_item(struct hashmap_node *n) {
    return (void *)((char *)n + offsetof(struct hashmap_node, item));
}

// Bucket region structure (each region holds an independent red-black tree)
struct hashmap_region {
    rb_root_t root;
};

// Main hashmap instance structure
struct hashmap {
    /* [OPT] 热路径字段压缩进同一缓存行: 每次 set/get/delete 全部命中 */
    size_t   elsize;
    size_t   mask;
    size_t   count;
    size_t   growat;
    size_t   shrinkat;
    struct hashmap_region *regions;
    uint64_t (*hash)(const void *item, uint64_t seed0, uint64_t seed1);
    int32_t  (*compare)(const void *a, const void *b, void *udata);

    /* ---- 冷路径 ---- */
    size_t   nbuckets;
    size_t   cap;
    struct hashmap_node *freelist;   // [分配] 回收节点空闲链
    size_t   freecount;
    void     *spare;
    void     (*elfree)(void *item);
    void     *udata;
    uint64_t seed0;
    uint64_t seed1;
    uint16_t loadfactor;
    uint8_t  growpower;
    bool     oom;
};

// Red-black tree comparator
// No global variable, no extra context, fully compatible with original rb_tree.h interface
static int hashmap_rb_cmp(const rb_node_t *a, const rb_node_t *b) {
    struct hashmap_node *na = RB_CONTAINER_OF(a, struct hashmap_node, rb);
    struct hashmap_node *nb = RB_CONTAINER_OF(b, struct hashmap_node, rb);

    /* [分支] 48bit 哈希空间, 相等 ≈ n²/2⁴⁸ 属罕见事件;
       绝大多数比较在哈希层就解决, 不进用户比较器 */
    if (likely(na->hash != nb->hash)) {
        return na->hash < nb->hash ? -1 : 1;
    }

    // 哈希碰撞: 至少一端携带有效 map 指针 (proxy 必有)
    struct hashmap *map = na->map ? na->map : nb->map;
    if (unlikely(!map || !map->compare)) {
        return 1;
    }
    const void *a_data = na->is_proxy ? na->key_ptr : hashmap_node_item(na);
    const void *b_data = nb->is_proxy ? nb->key_ptr : hashmap_node_item(nb);
    return (int)map->compare(a_data, b_data, map->udata);
}

// Allocate new hashmap node
// [分配] 优先从空闲链取, 高频增删场景完全绕过 kmalloc/kfree
static struct hashmap_node *hashmap_node_alloc(struct hashmap *map,
                                                const void *item,
                                                uint64_t hash) {
    struct hashmap_node *n;
#if HASHMAP_NODE_CACHE
    if (likely(map->freecount != 0)) {
        n = map->freelist;
        map->freelist = n->cache_next;
        map->freecount--;
    } else
#endif
    {
        n = (struct hashmap_node *)kmalloc(sizeof(struct hashmap_node) + map->elsize);
        if (unlikely(!n)) return NULL;
    }
    n->hash     = hash;
    n->map      = map; // Bind node to parent hashmap
    n->key_ptr  = NULL;
    n->is_proxy = 0;
    /* [OPT] _pad 是纯对齐填充、从不被读, 去掉 memset;
       链接字段由 rb_insert 内部的 rb_init_node 重建 */
    if (item && map->elsize) {
        __memcpy(hashmap_node_item(n), item, map->elsize);
    }
    return n;
}

// [分配] delete 侧: 回收进空闲链 (item 已由 spare 返回给调用方,
// elfree 语义与原实现一致 —— delete 从不调用 elfree)
static inline void hashmap_node_release(struct hashmap *map, struct hashmap_node *n) {
#if HASHMAP_NODE_CACHE
    n->cache_next = map->freelist;
    map->freelist = n;
    map->freecount++;
#else
    (void)map;
    kfree(n);
#endif
}

// [分配] 清空/销毁时全量释放回收节点 —— clear 语义 = 完整释放内存
static void hashmap_cache_drain(struct hashmap *map) {
#if HASHMAP_NODE_CACHE
    struct hashmap_node *n = map->freelist;
    while (n) {
        struct hashmap_node *next = n->cache_next;
        kfree(n);
        n = next;
    }
    map->freelist = NULL;
    map->freecount = 0;
#else
    (void)map;
#endif
}

// Initialize bucket region
static void hashmap_region_init(struct hashmap_region *r) {
    rb_root_init(&r->root, NULL, NULL, NULL, NULL, NULL);
}

// Callback to free node content during region clear
static void hashmap_free_node_cb(rb_node_t *node, void *arg) {
    struct hashmap *map = (struct hashmap *)arg;
    struct hashmap_node *n = RB_CONTAINER_OF(node, struct hashmap_node, rb);
    if (map->elfree) {
        map->elfree(hashmap_node_item(n));
    }
    kfree(n);
}

// Clear all nodes inside one bucket region
static void hashmap_clear_region(struct hashmap *map, struct hashmap_region *r) {
    rb_clear(&r->root, hashmap_free_node_cb, map);
}

/* [分配] resize 收集上下文: 只记录节点指针。
   注意: rb_clear 走 Morris 后序, 回调期间节点的 right 指针处于
   反转穿链状态, 绝不能在回调里触碰节点本身 (否则破坏遍历链) */
struct hashmap_collect_ctx {
    rb_node_t **nodes;
    size_t      pos;
};

static void hashmap_collect_cb(rb_node_t *node, void *arg) {
    struct hashmap_collect_ctx *ctx = (struct hashmap_collect_ctx *)arg;
    ctx->nodes[ctx->pos++] = node;
}

// Resize bucket table (expand or shrink)
// [分配] 相比原实现:
//   1) 原来每节点 rb_first+rb_erase+rb_insert, erase 自带 O(log n) fixup;
//      现在 Morris 后序 O(n) 摘链, 只剩一次插入下降;
//   2) 先释放旧桶表再填充新表, 缩短 "旧表+新表" 同时存活的峰值窗口;
//   3) 节点数 <= HASHMAP_STACK_COLLECT 时用栈缓冲, 零额外分配;
//   4) 任一分配失败时旧表完好无损, 调用方可安全降级。
static bool hashmap_resize(struct hashmap *map, size_t new_cap) {
    new_cap = hashmap_round_pow2(new_cap);

    struct hashmap_region *new_regions = (struct hashmap_region *)
        kmalloc(sizeof(struct hashmap_region) * new_cap);
    if (unlikely(!new_regions)) return false;
    for (size_t i = 0; i < new_cap; i++) {
        hashmap_region_init(&new_regions[i]);
    }

    size_t n = map->count;
    if (n == 0) {
        // 空表换壳: 典型场景是 clear(false) 复位 / 缩容到空
        kfree(map->regions);
        goto commit;
    }

    rb_node_t *stack_nodes[HASHMAP_STACK_COLLECT];
    rb_node_t **nodes = stack_nodes;
    if (unlikely(n > HASHMAP_STACK_COLLECT)) {
        nodes = (rb_node_t **)kmalloc(n * sizeof(rb_node_t *));
        if (unlikely(!nodes)) {
            kfree(new_regions);
            return false;
        }
    }

    // Phase 1: O(n) 摘下全部节点 (无 erase、无 fixup、无 rebalance)
    struct hashmap_collect_ctx ctx = { nodes, 0 };
    for (size_t i = 0; i < map->nbuckets; i++) {
        /* [预取] 顺序遍历桶数组; Morris 遍历耗时较长,
           足够下一桶头行落地 */
        if (likely(i + 1 < map->nbuckets)) {
            PREFETCH_RH(&map->regions[i + 1].root);
        }
        rb_clear(&map->regions[i].root, hashmap_collect_cb, &ctx);
    }

    // 节点已全部脱链, 旧桶表成为死重, 尽早释放
    kfree(map->regions);

    // Phase 2: 重插到新表
    size_t moved = ctx.pos;
    if (unlikely(moved != n)) {
        map->count = moved;  // 防御: 修复可能的计数漂移(不应发生)
    }
    size_t new_mask = new_cap - 1;
    for (size_t i = 0; i < moved; i++) {
        /* [预取] nodes 数组本身顺序流由硬件预取器覆盖,
           但数组元素指向的节点行是随机跳转 —— 提前一拍预取 */
        if (likely(i + 1 < moved)) {
            PREFETCH_RH(nodes[i + 1]);
        }
        struct hashmap_node *hn = RB_CONTAINER_OF(nodes[i], struct hashmap_node, rb);
        rb_insert(&new_regions[hn->hash & new_mask].root, &hn->rb, hashmap_rb_cmp);
    }

    if (nodes != stack_nodes) kfree(nodes);

commit:
    map->regions  = new_regions;
    map->nbuckets = new_cap;
    map->mask     = new_cap - 1;
    map->growat   = lf_threshold(new_cap, PCT_TO_LF(map->loadfactor));
    map->shrinkat = lf_threshold(new_cap, SHRINK_LF);
    return true;
}

// Public API: set growth power factor
void hashmap_set_grow_by_power(struct hashmap *map, size_t power) {
    map->growpower = power < 1 ? 1 : (power > 16 ? 16 : (uint8_t)power);
}

// Public API: update load factor threshold
void hashmap_set_load_factor(struct hashmap *map, uint8_t percent) {
    uint8_t cur_pct = (map->loadfactor != 0) ? (uint8_t)map->loadfactor : GROW_AT;
    uint8_t pct = clamp_load_factor_pct(percent, cur_pct);
    map->loadfactor = pct;
    map->growat = lf_threshold(map->nbuckets, PCT_TO_LF(pct));
}

// Create hashmap with custom allocator wrapper
struct hashmap *hashmap_new_with_allocator(
    size_t elsize, size_t cap, uint64_t seed0, uint64_t seed1,
    uint64_t (*hash)(const void *item, uint64_t seed0, uint64_t seed1),
    int32_t (*compare)(const void *a, const void *b, void *udata),
    void (*elfree)(void *item),
    void *udata)
{
    if (unlikely(elsize == 0)) {
        return NULL;
    }
    cap = hashmap_round_pow2(cap);

    struct hashmap *map = (struct hashmap *)kmalloc(sizeof(struct hashmap));
    if (unlikely(!map)) return NULL;
    _memset(map, 0, sizeof(*map));

    map->elsize   = elsize;
    map->seed0    = seed0;
    map->seed1    = seed1;
    map->hash     = hash;
    map->compare  = compare;
    map->elfree   = elfree;
    map->udata    = udata;
    map->cap      = cap;
    map->nbuckets = cap;
    map->mask     = cap - 1;
    map->growpower = 1;
    map->oom      = false;
    map->count    = 0;

    map->regions = (struct hashmap_region *)
        kmalloc(sizeof(struct hashmap_region) * cap);
    if (unlikely(!map->regions)) {
        kfree(map);
        return NULL;
    }
    for (size_t i = 0; i < cap; i++) {
        hashmap_region_init(&map->regions[i]);
    }

    map->spare = kmalloc(elsize);   // elsize 已保证非零
    if (unlikely(!map->spare)) {
        kfree(map->regions);
        kfree(map);
        return NULL;
    }

    map->loadfactor = clamp_load_factor_pct(HASHMAP_LOAD_FACTOR, GROW_AT);
    map->growat   = lf_threshold(map->nbuckets, PCT_TO_LF(map->loadfactor));
    map->shrinkat = lf_threshold(map->nbuckets, SHRINK_LF);

    return map;
}

// Standard hashmap constructor
struct hashmap *hashmap_new(size_t elsize, size_t cap, uint64_t seed0,
    uint64_t seed1,
    uint64_t (*hash)(const void *item, uint64_t seed0, uint64_t seed1),
    int32_t (*compare)(const void *a, const void *b, void *udata),
    void (*elfree)(void *item),
    void *udata)
{
    return hashmap_new_with_allocator(elsize, cap, seed0,
        seed1, hash, compare, elfree, udata);
}

// Destroy entire hashmap and free all resources
void hashmap_free(struct hashmap *map) {
    if (unlikely(!map)) return;
    for (size_t i = 0; i < map->nbuckets; i++) {
        /* [预取] 顺序流: 下一桶头行与当前桶的 Morris 释放重叠 */
        if (likely(i + 1 < map->nbuckets)) {
            PREFETCH_RH(&map->regions[i + 1].root);
        }
        hashmap_clear_region(map, &map->regions[i]);
    }
    hashmap_cache_drain(map);   // [分配] 回收链也一并释放
    kfree(map->regions);
    if (map->spare) kfree(map->spare);
    kfree(map);
}

// Clear all entries, optionally reset bucket capacity
void hashmap_clear(struct hashmap *map, bool update_cap) {
    for (size_t i = 0; i < map->nbuckets; i++) {
        if (likely(i + 1 < map->nbuckets)) {
            PREFETCH_RH(&map->regions[i + 1].root);
        }
        hashmap_clear_region(map, &map->regions[i]);
    }
    map->count = 0;
    /* [分配] clear 语义 = 完整释放: 回收链同样清空;
       delete→insert 的抖动复用才是空闲链的目标场景 */
    hashmap_cache_drain(map);

    if (update_cap) {
        map->cap = map->nbuckets;
    } else if (map->nbuckets != map->cap) {
        hashmap_resize(map, map->cap);   // count==0: 纯换壳, 零节点搬移
    }
}

// Internal helper: compute and clip hash value
static uint64_t get_hash(const struct hashmap *map, const void *key) {
    return clip_hash(map->hash(key, map->seed0, map->seed1));
}

// Initialize proxy node used for search matching
/* [OPT] 比较器只读 hash/map/is_proxy/key_ptr, rb 链接字段与 _pad
   从不被读 —— 省掉 rb_init_node + memset 共 ~10 次存储/次操作 */
static inline void hashmap_build_proxy(struct hashmap_node *proxy,
                                        struct hashmap *map,
                                        uint64_t hash, const void *key) {
    proxy->hash     = hash;
    proxy->map      = map;
    proxy->key_ptr  = key;
    proxy->is_proxy = 1;
}

// Insert or update entry with precomputed hash
const void *hashmap_set_with_hash(struct hashmap *map, const void *item,
    uint64_t hash)
{
    hash = clip_hash(hash);
    map->oom = false;

    /* [分支][分配] 扩容点: 摊还触发, 属罕见路径。
       失败时不再让本次插入整体失败 —— 树桶可优雅降级(桶内树变深),
       仅退避 25% 负载后重试, 避免OOM期间每次 set 都撞一次 kmalloc */
    if (unlikely(map->count >= map->growat)) {
        if (unlikely(!hashmap_resize(map, map->nbuckets << map->growpower))) {
            map->oom = true;
            map->growat = map->count + (map->nbuckets >> 2) + 1;
        }
    }

    size_t idx = hash & map->mask;
    struct hashmap_region *r = &map->regions[idx];

    /* [预取] 先发桶头行, 再构造 proxy(独立存储), 与该行到达重叠;
       随后预取 root.node / root.hint 两个目标 —— 这是 rb_search
       即将开始的三级指针追逐的头两级 */
    PREFETCH_RH(&r->root);

    struct hashmap_node proxy;
    hashmap_build_proxy(&proxy, map, hash, item);

    PREFETCH_RH(r->root.node);
    PREFETCH_RH(r->root.hint);

    rb_node_t *existing = rb_search(&r->root, &proxy.rb, hashmap_rb_cmp);

    if (existing) {
        /* [分支] 更新 vs 新增是数据相关 (~50/50), 不标 */
        struct hashmap_node *n = RB_CONTAINER_OF(existing, struct hashmap_node, rb);
        void *old_item = hashmap_node_item(n);
        __memcpy(map->spare, old_item, map->elsize);
        __memcpy(old_item, item, map->elsize);
        return map->spare;
    }

    struct hashmap_node *n = hashmap_node_alloc(map, item, hash);
    if (unlikely(!n)) {
        map->oom = true;
        return NULL;
    }
    rb_insert(&r->root, &n->rb, hashmap_rb_cmp);
    map->count++;

    return NULL;
}

// Insert or update entry (compute hash internally)
const void *hashmap_set(struct hashmap *map, const void *item) {
    return hashmap_set_with_hash(map, item, get_hash(map, item));
}

// Lookup entry with precomputed hash
const void *hashmap_get_with_hash(const struct hashmap *map, const void *key,
    uint64_t hash)
{
    hash = clip_hash(hash);
    size_t idx = hash & map->mask;
    struct hashmap_region *r = &map->regions[idx];

    PREFETCH_RH(&r->root);

    struct hashmap_node proxy;
    hashmap_build_proxy(&proxy, (struct hashmap *)map, hash, key);

    PREFETCH_RH(r->root.node);
    PREFETCH_RH(r->root.hint);

    rb_node_t *found = rb_search(&r->root, &proxy.rb, hashmap_rb_cmp);

    /* [分支] 存在性查询以命中为主流(同 rb_tree.h 的标注口径);
       若你的负载以 miss 为主, 把提示反过来即可 */
    if (likely(found)) {
        return hashmap_node_item(RB_CONTAINER_OF(found, struct hashmap_node, rb));
    }
    return NULL;
}

// Lookup entry (compute hash internally)
const void *hashmap_get(const struct hashmap *map, const void *key) {
    return hashmap_get_with_hash(map, key, get_hash(map, key));
}

// Delete entry with precomputed hash
const void *hashmap_delete_with_hash(struct hashmap *map, const void *key,
    uint64_t hash)
{
    hash = clip_hash(hash);
    map->oom = false;
    size_t idx = hash & map->mask;
    struct hashmap_region *r = &map->regions[idx];

    PREFETCH_RH(&r->root);

    struct hashmap_node proxy;
    hashmap_build_proxy(&proxy, map, hash, key);

    PREFETCH_RH(r->root.node);
    PREFETCH_RH(r->root.hint);

    rb_node_t *found = rb_search(&r->root, &proxy.rb, hashmap_rb_cmp);
    if (unlikely(!found)) {
        return NULL;   // [分支] 删除通常针对存在的键
    }

    struct hashmap_node *n = RB_CONTAINER_OF(found, struct hashmap_node, rb);
    __memcpy(map->spare, hashmap_node_item(n), map->elsize);
    rb_erase(&r->root, found);
    hashmap_node_release(map, n);   // [分配] 回收进空闲链而非 kfree
    map->count--;

    /* [分支][分配] 缩容点: 低于 10% 才触发, 罕见。
       一步落在负载中带(~50%), 而非原实现的只减半 ——
       批量删除时从 "每跌过阈值减半一次" 的 O(log n) 次 resize
       收敛为少数几次; 中带落点同时远离两侧阈值, 无伸缩抖动 */
    if (unlikely(map->nbuckets > map->cap && map->count <= map->shrinkat)) {
        size_t half = map->nbuckets >> 1;
        size_t target = hashmap_round_pow2(map->count << 1);  // ~50% 负载目标
        if (target > half) target = half;
        if (target < map->cap) target = map->cap;
        hashmap_resize(map, target);   // 尽力而为; 失败仅保持大表, 无害
    }

    return map->spare;
}

// Delete entry (compute hash internally)
const void *hashmap_delete(struct hashmap *map, const void *key) {
    return hashmap_delete_with_hash(map, key, get_hash(map, key));
}

// Return stored entry count
size_t hashmap_count(const struct hashmap *map) {
    return map->count;
}

// Check whether last operation encountered OOM
bool hashmap_oom(struct hashmap *map) {
    return map->oom;
}

// Return bucket table size
size_t hashmap_nbuckets(const struct hashmap *map) {
    return map->nbuckets;
}

// Fetch first item in specified bucket
const void *hashmap_bucket_item(const struct hashmap *map, size_t i) {
    if (unlikely(i >= map->nbuckets)) return NULL;
    struct hashmap_region *r = &map->regions[i];
    rb_node_t *first = rb_first(r->root.node);
    if (!first) return NULL;
    return hashmap_node_item(RB_CONTAINER_OF(first, struct hashmap_node, rb));
}

// Simple probe function by hash position
const void *hashmap_probe(struct hashmap *map, uint64_t position) {
    size_t idx = (size_t)(position & map->mask);
    if (unlikely(idx >= map->nbuckets)) return NULL;
    struct hashmap_region *r = &map->regions[idx];
    rb_node_t *first = rb_first(r->root.node);
    if (!first) return NULL;
    return hashmap_node_item(RB_CONTAINER_OF(first, struct hashmap_node, rb));
}

// Full scan iteration over all entries
bool hashmap_scan(struct hashmap *map,
    bool (*iter)(const void *item, void *udata), void *udata)
{
    for (size_t i = 0; i < map->nbuckets; i++) {
        /* [预取] 桶数组顺序流: 空桶/稀疏桶切换极快时收益最大 */
        if (likely(i + 1 < map->nbuckets)) {
            PREFETCH_RH(&map->regions[i + 1].root);
        }
        struct hashmap_region *r = &map->regions[i];
        rb_node_t *cur = rb_first(r->root.node);
        while (cur) {
            struct hashmap_node *n = RB_CONTAINER_OF(cur, struct hashmap_node, rb);
            if (unlikely(!iter(hashmap_node_item(n), udata))) return false;
            cur = rb_next(cur);   // 指针追逐, 无前瞻窗口, 不预取
        }
    }
    return true;
}

// Resumable iterator
bool hashmap_iter(struct hashmap *map, size_t *i, void **item) {
    uint64_t state  = *i;
    uint32_t region = (uint32_t)(state >> 32);
    uint32_t offset = (uint32_t)(state & 0xFFFFFFFFu);

    while (region < map->nbuckets) {
        if (likely(region + 1 < map->nbuckets)) {
            PREFETCH_RH(&map->regions[region + 1].root);
        }
        struct hashmap_region *r = &map->regions[region];
        rb_node_t *cur = rb_first(r->root.node);
        for (uint32_t k = 0; k < offset && cur; k++) {
            cur = rb_next(cur);
        }
        if (likely(cur)) {
            *item = hashmap_node_item(RB_CONTAINER_OF(cur, struct hashmap_node, rb));
            *i = ((uint64_t)region << 32) | (uint64_t)(offset + 1);
            return true;
        }
        region++;
        offset = 0;
    }
    return false;
}

/* ==================== 哈希函数: 与原实现一致, 未改动 ==================== */

// SipHash reference C implementation
static uint64_t SIP64(const uint8_t *in, const size_t inlen, uint64_t seed0,
    uint64_t seed1)
{
#define U8TO64_LE(p) \
    {  (((uint64_t)((p)[0])) | ((uint64_t)((p)[1]) << 8) | \
        ((uint64_t)((p)[2]) << 16) | ((uint64_t)((p)[3]) << 24) | \
        ((uint64_t)((p)[4]) << 32) | ((uint64_t)((p)[5]) << 40) | \
        ((uint64_t)((p)[6]) << 48) | ((uint64_t)((p)[7]) << 56)) }
#define U64TO8_LE(p, v) \
    { U32TO8_LE((p), (uint32_t)((v))); \
      U32TO8_LE((p) + 4, (uint32_t)((v) >> 32)); }
#define U32TO8_LE(p, v) \
    { (p)[0] = (uint8_t)((v)); \
      (p)[1] = (uint8_t)((v) >> 8); \
      (p)[2] = (uint8_t)((v) >> 16); \
      (p)[3] = (uint8_t)((v) >> 24); }
#define ROTL(x, b) (uint64_t)(((x) << (b)) | ((x) >> (64 - (b))))
#define SIPROUND \
    { v0 += v1; v1 = ROTL(v1, 13); \
      v1 ^= v0; v0 = ROTL(v0, 32); \
      v2 += v3; v3 = ROTL(v3, 16); \
      v3 ^= v2; \
      v0 += v3; v3 = ROTL(v3, 21); \
      v3 ^= v0; \
      v2 += v1; v1 = ROTL(v1, 17); \
      v1 ^= v2; v2 = ROTL(v2, 32); }
    uint64_t k0 = U8TO64_LE((uint8_t*)&seed0);
    uint64_t k1 = U8TO64_LE((uint8_t*)&seed1);
    uint64_t v3 = UINT64_C(0x7465646279746573) ^ k1;
    uint64_t v2 = UINT64_C(0x6c7967656e657261) ^ k0;
    uint64_t v1 = UINT64_C(0x646f72616e646f6d) ^ k1;
    uint64_t v0 = UINT64_C(0x736f6d6570736575) ^ k0;
    const uint8_t *end = in + inlen - (inlen % sizeof(uint64_t));
    for (; in != end; in += 8) {
        uint64_t m = U8TO64_LE(in);
        v3 ^= m;
        SIPROUND; SIPROUND;
        v0 ^= m;
    }
    const int32_t left = inlen & 7;
    uint64_t b = ((uint64_t)inlen) << 56;
    switch (left) {
    case 7: b |= ((uint64_t)in[6]) << 48; /* fall through */
    case 6: b |= ((uint64_t)in[5]) << 40; /* fall through */
    case 5: b |= ((uint64_t)in[4]) << 32; /* fall through */
    case 4: b |= ((uint64_t)in[3]) << 24; /* fall through */
    case 3: b |= ((uint64_t)in[2]) << 16; /* fall through */
    case 2: b |= ((uint64_t)in[1]) << 8; /* fall through */
    case 1: b |= ((uint64_t)in[0]); break;
    case 0: break;
    }
    v3 ^= b;
    SIPROUND; SIPROUND;
    v0 ^= b;
    v2 ^= 0xff;
    SIPROUND; SIPROUND; SIPROUND; SIPROUND;
    b = v0 ^ v1 ^ v2 ^ v3;
    uint64_t out = 0;
    U64TO8_LE((uint8_t*)&out, b);
    return out;
}

// MurmurHash3 128-bit implementation
static uint64_t MM86128(const void *key, const int32_t len, uint32_t seed) {
#define	ROTL32(x, r) ((x << r) | (x >> (32 - r)))
#define FMIX32(h) h^=h>>16; h*=0x85ebca6b; h^=h>>13; h*=0xc2b2ae35; h^=h>>16;
    const uint8_t * data = (const uint8_t*)key;
    const int32_t nblocks = len / 16;
    uint32_t h1 = seed;
    uint32_t h2 = seed;
    uint32_t h3 = seed;
    uint32_t h4 = seed;
    uint32_t c1 = 0x239b961b;
    uint32_t c2 = 0xab0e9789;
    uint32_t c3 = 0x38b34ae5;
    uint32_t c4 = 0xa1e38b93;
    const uint32_t * blocks = (const uint32_t *)(data + nblocks*16);
    for (int32_t i = -nblocks; i; i++) {
        uint32_t k1 = blocks[i*4+0];
        uint32_t k2 = blocks[i*4+1];
        uint32_t k3 = blocks[i*4+2];
        uint32_t k4 = blocks[i*4+3];
        k1 *= c1; k1  = ROTL32(k1,15); k1 *= c2; h1 ^= k1;
        h1 = ROTL32(h1,19); h1 += h2; h1 = h1*5+0x561ccd1b;
        k2 *= c2; k2  = ROTL32(k2,16); k2 *= c3; h2 ^= k2;
        h2 = ROTL32(h2,17); h2 += h3; h2 = h2*5+0x0bcaa747;
        k3 *= c3; k3  = ROTL32(k3,17); k3 *= c4; h3 ^= k3;
        h3 = ROTL32(h3,15); h3 += h4; h3 = h3*5+0x96cd1c35;
        k4 *= c4; k4  = ROTL32(k4,18); k4 *= c1; h4 ^= k4;
        h4 = ROTL32(h4,13); h4 += h1; h4 = h4*5+0x32ac3b17;
    }
    const uint8_t * tail = (const uint8_t*)(data + nblocks*16);
    uint32_t k1 = 0;
    uint32_t k2 = 0;
    uint32_t k3 = 0;
    uint32_t k4 = 0;
    switch(len & 15) {
    case 15: k4 ^= tail[14] << 16; /* fall through */
    case 14: k4 ^= tail[13] << 8; /* fall through */
    case 13: k4 ^= tail[12] << 0;
             k4 *= c4; k4  = ROTL32(k4,18); k4 *= c1; h4 ^= k4;
             /* fall through */
    case 12: k3 ^= tail[11] << 24; /* fall through */
    case 11: k3 ^= tail[10] << 16; /* fall through */
    case 10: k3 ^= tail[ 9] << 8; /* fall through */
    case  9: k3 ^= tail[ 8] << 0;
             k3 *= c3; k3  = ROTL32(k3,17); k3 *= c4; h3 ^= k3;
             /* fall through */
    case  8: k2 ^= tail[ 7] << 24; /* fall through */
    case  7: k2 ^= tail[ 6] << 16; /* fall through */
    case  6: k2 ^= tail[ 5] << 8; /* fall through */
    case  5: k2 ^= tail[ 4] << 0;
             k2 *= c2; k2  = ROTL32(k2,16); k2 *= c3; h2 ^= k2;
             /* fall through */
    case  4: k1 ^= tail[ 3] << 24; /* fall through */
    case  3: k1 ^= tail[ 2] << 16; /* fall through */
    case  2: k1 ^= tail[ 1] << 8; /* fall through */
    case  1: k1 ^= tail[ 0] << 0;
             k1 *= c1; k1  = ROTL32(k1,15); k1 *= c2; h1 ^= k1;
             /* fall through */
    };
    h1 ^= len; h2 ^= len; h3 ^= len; h4 ^= len;
    h1 += h2; h1 += h3; h1 += h4;
    h2 += h1; h3 += h1; h4 += h1;
    FMIX32(h1); FMIX32(h2); FMIX32(h3); FMIX32(h4);
    h1 += h2; h1 += h3; h1 += h4;
    h2 += h1; h3 += h1; h4 += h1;
    return (((uint64_t)h2)<<32)|h1;
}

// xxHash3 implementation
#define XXH_PRIME_1 11400714785074694791ULL
#define XXH_PRIME_2 14029467366897019727ULL
#define XXH_PRIME_3 1609587929392839161ULL
#define XXH_PRIME_4 9650029242287828579ULL
#define XXH_PRIME_5 2870177450012600261ULL

static uint64_t XXH_read64(const void* memptr) {
    uint64_t val;
    __memcpy(&val, memptr, sizeof(val));
    return val;
}

static uint32_t XXH_read32(const void* memptr) {
    uint32_t val;
    __memcpy(&val, memptr, sizeof(val));
    return val;
}

static uint64_t XXH_rotl64(uint64_t x, int32_t r) {
    return (x << r) | (x >> (64 - r));
}

static uint64_t xxh3(const void *data, size_t len, uint64_t seed) {
    const uint8_t* p = (const uint8_t*)data;
    const uint8_t* const end = p + len;
    uint64_t h64;

    if (len >= 32) {
        const uint8_t* const limit = end - 32;
        uint64_t v1 = seed + XXH_PRIME_1 + XXH_PRIME_2;
        uint64_t v2 = seed + XXH_PRIME_2;
        uint64_t v3 = seed + 0;
        uint64_t v4 = seed - XXH_PRIME_1;

        do {
            v1 += XXH_read64(p) * XXH_PRIME_2;
            v1 = XXH_rotl64(v1, 31);
            v1 *= XXH_PRIME_1;

            v2 += XXH_read64(p + 8) * XXH_PRIME_2;
            v2 = XXH_rotl64(v2, 31);
            v2 *= XXH_PRIME_1;

            v3 += XXH_read64(p + 16) * XXH_PRIME_2;
            v3 = XXH_rotl64(v3, 31);
            v3 *= XXH_PRIME_1;

            v4 += XXH_read64(p + 24) * XXH_PRIME_2;
            v4 = XXH_rotl64(v4, 31);
            v4 *= XXH_PRIME_1;

            p += 32;
        } while (p <= limit);

        h64 = XXH_rotl64(v1, 1) + XXH_rotl64(v2, 7) + XXH_rotl64(v3, 12) +
            XXH_rotl64(v4, 18);

        v1 *= XXH_PRIME_2;
        v1 = XXH_rotl64(v1, 31);
        v1 *= XXH_PRIME_1;
        h64 ^= v1;
        h64 = h64 * XXH_PRIME_1 + XXH_PRIME_4;

        v2 *= XXH_PRIME_2;
        v2 = XXH_rotl64(v2, 31);
        v2 *= XXH_PRIME_1;
        h64 ^= v2;
        h64 = h64 * XXH_PRIME_1 + XXH_PRIME_4;

        v3 *= XXH_PRIME_2;
        v3 = XXH_rotl64(v3, 31);
        v3 *= XXH_PRIME_1;
        h64 ^= v3;
        h64 = h64 * XXH_PRIME_1 + XXH_PRIME_4;

        v4 *= XXH_PRIME_2;
        v4 = XXH_rotl64(v4, 31);
        v4 *= XXH_PRIME_1;
        h64 ^= v4;
        h64 = h64 * XXH_PRIME_1 + XXH_PRIME_4;
    }
    else {
        h64 = seed + XXH_PRIME_5;
    }

    h64 += (uint64_t)len;

    while (p + 8 <= end) {
        uint64_t k1 = XXH_read64(p);
        k1 *= XXH_PRIME_2;
        k1 = XXH_rotl64(k1, 31);
        k1 *= XXH_PRIME_1;
        h64 ^= k1;
        h64 = XXH_rotl64(h64, 27) * XXH_PRIME_1 + XXH_PRIME_4;
        p += 8;
    }

    if (p + 4 <= end) {
        h64 ^= (uint64_t)(XXH_read32(p)) * XXH_PRIME_1;
        h64 = XXH_rotl64(h64, 23) * XXH_PRIME_2 + XXH_PRIME_3;
        p += 4;
    }

    while (p < end) {
        h64 ^= (*p) * XXH_PRIME_5;
        h64 = XXH_rotl64(h64, 11) * XXH_PRIME_1;
        p++;
    }

    h64 ^= h64 >> 33;
    h64 *= XXH_PRIME_2;
    h64 ^= h64 >> 29;
    h64 *= XXH_PRIME_3;
    h64 ^= h64 >> 32;

    return h64;
}

// Public hash function wrappers
uint64_t hashmap_sip(const void *data, size_t len, uint64_t seed0,
    uint64_t seed1)
{
    return SIP64((uint8_t*)data, len, seed0, seed1);
}

uint64_t hashmap_murmur(const void *data, size_t len, uint64_t seed0,
    uint64_t seed1)
{
    (void)seed1;
    return MM86128(data, len, seed0);
}

uint64_t hashmap_xxhash3(const void *data, size_t len, uint64_t seed0,
    uint64_t seed1)
{
    (void)seed1;
    return xxh3(data, len, seed0);
}