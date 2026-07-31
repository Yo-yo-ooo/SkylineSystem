#include <klib/klibc.h>
#include <klib/algorithm/hmap.h>
#include <klib/algorithm/rbtree.h>

// ============================================================
//  负载因子配置
// ============================================================
#define GROW_AT   60
#define SHRINK_AT 10

#ifndef HASHMAP_LOAD_FACTOR
#define HASHMAP_LOAD_FACTOR GROW_AT
#endif

// ============================================================
//  定点数定义
// ============================================================
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

// ============================================================
//  节点结构：存储在红黑树中的元素
//
//  核心设计：将 map 指针存放在节点中，避免修改 rb_tree.h 的
//  比较函数签名，同时彻底消除全局变量。
// ============================================================
struct hashmap_node {
    rb_node_t      rb;          // 红黑树节点（必须为首成员）
    uint64_t       hash;        // 48-bit clipped hash
    struct hashmap *map;        // 所属的 hashmap，替代全局变量
    const void    *key_ptr;     // 仅当 is_proxy != 0 时有效
    uint8_t        is_proxy;    // 0 = 真实节点, 1 = 查找代理
    uint8_t        _pad[7];     // 对齐 item[] 到 8 字节边界
    char           item[];      // 真实节点的用户数据（柔性数组）
};

static inline void *hashmap_node_item(struct hashmap_node *n) {
    return (void *)((char *)n + offsetof(struct hashmap_node, item));
}

// ============================================================
//  区间结构
// ============================================================
struct hashmap_region {
    rb_root_t root;
};

// ============================================================
//  哈希映射主结构
// ============================================================
struct hashmap {
    size_t   elsize;
    size_t   cap;
    size_t   nbuckets;
    size_t   count;
    size_t   mask;
    size_t   growat;
    size_t   shrinkat;
    uint16_t loadfactor;
    uint8_t  growpower;
    bool     oom;
    uint64_t seed0;
    uint64_t seed1;
    uint64_t (*hash)(const void *item, uint64_t seed0, uint64_t seed1);
    int32_t  (*compare)(const void *a, const void *b, void *udata);
    void     (*elfree)(void *item);
    void     *udata;
    struct hashmap_region *regions;
    void     *spare;
};

// ============================================================
//  红黑树比较函数 (无全局变量，无 ctx，完全兼容原版 rb_tree.h)
//
//  通过 container_of 还原 hashmap_node，直接从节点中取 map 指针。
// ============================================================
static int hashmap_rb_cmp(const rb_node_t *a, const rb_node_t *b) {
    struct hashmap_node *na = RB_CONTAINER_OF(a, struct hashmap_node, rb);
    struct hashmap_node *nb = RB_CONTAINER_OF(b, struct hashmap_node, rb);

    if (na->hash < nb->hash) return -1;
    if (na->hash > nb->hash) return 1;

    // 两个节点中至少有一个带有 map 指针（通常都有）
    struct hashmap *map = na->map ? na->map : nb->map;
    if (!map || !map->compare) {
        return 1;
    }
    if (map && map->compare) {
        const void *a_data = na->is_proxy ? na->key_ptr : hashmap_node_item(na);
        const void *b_data = nb->is_proxy ? nb->key_ptr : hashmap_node_item(nb);
        return (int)map->compare(a_data, b_data, map->udata);
    }
    return 0;
}

// ============================================================
//  节点分配
// ============================================================
static struct hashmap_node *hashmap_node_alloc(struct hashmap *map,
                                                const void *item,
                                                uint64_t hash) {
    size_t total = sizeof(struct hashmap_node) + map->elsize;
    struct hashmap_node *n = (struct hashmap_node *)kmalloc(total);
    if (!n) return NULL;
    n->hash     = hash;
    n->map      = map; // 绑定 map 指针
    n->key_ptr  = NULL;
    n->is_proxy = 0;
    _memset(n->_pad, 0, sizeof(n->_pad));
    if (item && map->elsize) {
        __memcpy(hashmap_node_item(n), item, map->elsize);
    }
    return n;
}

// ============================================================
//  区间操作
// ============================================================
static void hashmap_region_init(struct hashmap_region *r) {
    rb_root_init(&r->root, NULL, NULL, NULL, NULL, NULL);
}

static void hashmap_free_node_cb(rb_node_t *node, void *arg) {
    struct hashmap *map = (struct hashmap *)arg;
    struct hashmap_node *n = RB_CONTAINER_OF(node, struct hashmap_node, rb);
    if (map->elfree) {
        map->elfree(hashmap_node_item(n));
    }
    kfree(n);
}

static void hashmap_clear_region(struct hashmap *map, struct hashmap_region *r) {
    rb_clear(&r->root, hashmap_free_node_cb, map);
}

// ============================================================
//  扩缩容
// ============================================================
static bool hashmap_resize(struct hashmap *map, size_t new_cap) {
    size_t ncap = 16;
    if (new_cap < 16) new_cap = 16;
    while (ncap < new_cap) ncap *= 2;
    new_cap = ncap;

    struct hashmap_region *new_regions = (struct hashmap_region *)
        kmalloc(sizeof(struct hashmap_region) * new_cap);
    if (!new_regions) return false;

    for (size_t i = 0; i < new_cap; i++) {
        hashmap_region_init(&new_regions[i]);
    }

    size_t new_mask = new_cap - 1;

    for (size_t i = 0; i < map->nbuckets; i++) {
        struct hashmap_region *old_r = &map->regions[i];
        while (old_r->root.node) {
            rb_node_t *node = rb_first(old_r->root.node);
            rb_erase(&old_r->root, node);

            struct hashmap_node *hn = RB_CONTAINER_OF(node, struct hashmap_node, rb);
            size_t new_idx = hn->hash & new_mask;
            struct hashmap_region *new_r = &new_regions[new_idx];
            // 不需要传 map 参数，因为 hn 内部已经包含了 map 指针
            rb_insert(&new_r->root, &hn->rb, hashmap_rb_cmp);
        }
    }

    kfree(map->regions);
    map->regions  = new_regions;
    map->nbuckets = new_cap;
    map->mask     = new_mask;
    map->growat   = lf_threshold(map->nbuckets, PCT_TO_LF(map->loadfactor));
    map->shrinkat = lf_threshold(map->nbuckets, SHRINK_LF);
    return true;
}

// ============================================================
//  公共 API: 配置
// ============================================================
void hashmap_set_grow_by_power(struct hashmap *map, size_t power) {
    map->growpower = power < 1 ? 1 : (power > 16 ? 16 : (uint8_t)power);
}

void hashmap_set_load_factor(struct hashmap *map, uint8_t percent) {
    uint8_t cur_pct = (map->loadfactor != 0) ? (uint8_t)map->loadfactor : GROW_AT;
    uint8_t pct = clamp_load_factor_pct(percent, cur_pct);
    map->loadfactor = pct;
    map->growat = lf_threshold(map->nbuckets, PCT_TO_LF(pct));
}

// ============================================================
//  构造
// ============================================================
struct hashmap *hashmap_new_with_allocator(
    size_t elsize, size_t cap, uint64_t seed0, uint64_t seed1,
    uint64_t (*hash)(const void *item, uint64_t seed0, uint64_t seed1),
    int32_t (*compare)(const void *a, const void *b, void *udata),
    void (*elfree)(void *item),
    void *udata)
{
    if (elsize == 0) {
        return NULL;
    }
    size_t ncap = 16;
    if (cap < ncap) {
        cap = ncap;
    } else {
        while (ncap < cap) ncap *= 2;
        cap = ncap;
    }

    struct hashmap *map = (struct hashmap *)kmalloc(sizeof(struct hashmap));
    if (!map) return NULL;
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
    if (!map->regions) {
        kfree(map);
        return NULL;
    }
    for (size_t i = 0; i < cap; i++) {
        hashmap_region_init(&map->regions[i]);
    }

    map->spare = kmalloc(elsize ? elsize : 1);
    if (!map->spare) {
        kfree(map->regions);
        kfree(map);
        return NULL;
    }

    map->loadfactor = clamp_load_factor_pct(HASHMAP_LOAD_FACTOR, GROW_AT);
    map->growat   = lf_threshold(map->nbuckets, PCT_TO_LF(map->loadfactor));
    map->shrinkat = lf_threshold(map->nbuckets, SHRINK_LF);

    return map;
}

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

// ============================================================
//  析构 / 清空
// ============================================================
void hashmap_free(struct hashmap *map) {
    if (!map) return;
    for (size_t i = 0; i < map->nbuckets; i++) {
        hashmap_clear_region(map, &map->regions[i]);
    }
    kfree(map->regions);
    if (map->spare) kfree(map->spare);
    kfree(map);
}

void hashmap_clear(struct hashmap *map, bool update_cap) {
    for (size_t i = 0; i < map->nbuckets; i++) {
        hashmap_clear_region(map, &map->regions[i]);
    }
    map->count = 0;

    if (update_cap) {
        map->cap = map->nbuckets;
    } else if (map->nbuckets != map->cap) {
        hashmap_resize(map, map->cap);
    }
}

// ============================================================
//  内部辅助
// ============================================================
static uint64_t get_hash(const struct hashmap *map, const void *key) {
    return clip_hash(map->hash(key, map->seed0, map->seed1));
}

static inline void hashmap_build_proxy(struct hashmap_node *proxy,
                                        struct hashmap *map,
                                        uint64_t hash, const void *key) {
    rb_init_node(&proxy->rb);
    proxy->hash     = hash;
    proxy->map      = map; // 代理节点也绑定 map 指针
    proxy->key_ptr  = key;
    proxy->is_proxy = 1;
    _memset(proxy->_pad, 0, sizeof(proxy->_pad));
}

// ============================================================
//  插入
// ============================================================
const void *hashmap_set_with_hash(struct hashmap *map, const void *item,
    uint64_t hash)
{
    hash = clip_hash(hash);
    map->oom = false;

    if (map->count >= map->growat) {
        if (!hashmap_resize(map, map->nbuckets * (1U << map->growpower))) {
            map->oom = true;
            return NULL;
        }
    }

    size_t idx = hash & map->mask;
    struct hashmap_region *r = &map->regions[idx];

    // 构造代理节点时传入 map
    struct hashmap_node proxy;
    hashmap_build_proxy(&proxy, map, hash, item);

    // 调用红黑树查找，使用原版无 ctx 接口
    rb_node_t *existing = rb_search(&r->root, &proxy.rb, hashmap_rb_cmp);

    if (existing) {
        struct hashmap_node *n = RB_CONTAINER_OF(existing, struct hashmap_node, rb);
        void *old_item = hashmap_node_item(n);
        __memcpy(map->spare, old_item, map->elsize);
        __memcpy(old_item, item, map->elsize);
        return map->spare;
    }

    struct hashmap_node *n = hashmap_node_alloc(map, item, hash);
    if (!n) {
        map->oom = true;
        return NULL;
    }
    // 调用红黑树插入，使用原版无 ctx 接口
    rb_insert(&r->root, &n->rb, hashmap_rb_cmp);
    map->count++;

    return NULL;
}

const void *hashmap_set(struct hashmap *map, const void *item) {
    return hashmap_set_with_hash(map, item, get_hash(map, item));
}

// ============================================================
//  查找
// ============================================================
const void *hashmap_get_with_hash(const struct hashmap *map, const void *key,
    uint64_t hash)
{
    hash = clip_hash(hash);
    size_t idx = hash & map->mask;
    struct hashmap_region *r = &map->regions[idx];

    struct hashmap_node proxy;
    hashmap_build_proxy(&proxy, (struct hashmap *)map, hash, key);

    // 调用红黑树查找，使用原版无 ctx 接口
    rb_node_t *found = rb_search(&r->root, &proxy.rb, hashmap_rb_cmp);

    if (!found) return NULL;
    struct hashmap_node *n = RB_CONTAINER_OF(found, struct hashmap_node, rb);
    return hashmap_node_item(n);
}

const void *hashmap_get(const struct hashmap *map, const void *key) {
    return hashmap_get_with_hash(map, key, get_hash(map, key));
}

// ============================================================
//  删除
// ============================================================
const void *hashmap_delete_with_hash(struct hashmap *map, const void *key,
    uint64_t hash)
{
    hash = clip_hash(hash);
    map->oom = false;
    size_t idx = hash & map->mask;
    struct hashmap_region *r = &map->regions[idx];

    struct hashmap_node proxy;
    hashmap_build_proxy(&proxy, map, hash, key);

    // 调用红黑树查找，使用原版无 ctx 接口
    rb_node_t *found = rb_search(&r->root, &proxy.rb, hashmap_rb_cmp);
    if (!found) {
        return NULL;
    }

    struct hashmap_node *n = RB_CONTAINER_OF(found, struct hashmap_node, rb);
    __memcpy(map->spare, hashmap_node_item(n), map->elsize);
    rb_erase(&r->root, found);
    kfree(n);
    map->count--;

    if (map->nbuckets > map->cap && map->count <= map->shrinkat) {
        size_t new_cap = map->nbuckets / 2;
        if (new_cap < map->cap) new_cap = map->cap;
        hashmap_resize(map, new_cap);
    }

    return map->spare;
}

const void *hashmap_delete(struct hashmap *map, const void *key) {
    return hashmap_delete_with_hash(map, key, get_hash(map, key));
}

// ============================================================
//  统计与状态
// ============================================================
size_t hashmap_count(const struct hashmap *map) {
    return map->count;
}

bool hashmap_oom(struct hashmap *map) {
    return map->oom;
}

size_t hashmap_nbuckets(const struct hashmap *map) {
    return map->nbuckets;
}

// ============================================================
//  区间接口
// ============================================================
const void *hashmap_bucket_item(const struct hashmap *map, size_t i) {
    if (i >= map->nbuckets) return NULL;
    struct hashmap_region *r = &map->regions[i];
    rb_node_t *first = rb_first(r->root.node);
    if (!first) return NULL;
    return hashmap_node_item(RB_CONTAINER_OF(first, struct hashmap_node, rb));
}

const void *hashmap_probe(struct hashmap *map, uint64_t position) {
    size_t idx = (size_t)(position & map->mask);
    if (idx >= map->nbuckets) return NULL;
    struct hashmap_region *r = &map->regions[idx];
    rb_node_t *first = rb_first(r->root.node);
    if (!first) return NULL;
    return hashmap_node_item(RB_CONTAINER_OF(first, struct hashmap_node, rb));
}

// ============================================================
//  全量遍历
// ============================================================
bool hashmap_scan(struct hashmap *map,
    bool (*iter)(const void *item, void *udata), void *udata)
{
    for (size_t i = 0; i < map->nbuckets; i++) {
        struct hashmap_region *r = &map->regions[i];
        rb_node_t *cur = rb_first(r->root.node);
        while (cur) {
            struct hashmap_node *n = RB_CONTAINER_OF(cur, struct hashmap_node, rb);
            if (!iter(hashmap_node_item(n), udata)) return false;
            cur = rb_next(cur);
        }
    }
    return true;
}

// ============================================================
//  迭代器（可恢复）
// ============================================================
bool hashmap_iter(struct hashmap *map, size_t *i, void **item) {
    uint64_t state  = *i;
    uint32_t region = (uint32_t)(state >> 32);
    uint32_t offset = (uint32_t)(state & 0xFFFFFFFFu);

    while (region < map->nbuckets) {
        struct hashmap_region *r = &map->regions[region];
        rb_node_t *cur = rb_first(r->root.node);
        for (uint32_t k = 0; k < offset && cur; k++) {
            cur = rb_next(cur);
        }
        if (cur) {
            *item = hashmap_node_item(RB_CONTAINER_OF(cur, struct hashmap_node, rb));
            *i = ((uint64_t)region << 32) | (uint64_t)(offset + 1);
            return true;
        }
        region++;
        offset = 0;
    }
    return false;
}

//=============================================================================
// SipHash reference C implementation
//=============================================================================
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

//=============================================================================
// MurmurHash3
//=============================================================================
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

//=============================================================================
// xxHash Library
//=============================================================================
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