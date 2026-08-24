//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
// motified from https://github.com/armon/libart/blob/master/src/art.c
#include <klib/algorithm/art.h>
#include <klib/klibc.h>
#include <stdbool.h>
#include <pdef.h>
extern void *__memcpy(void * d, const void * s, uint64_t n);
extern void Panic(const char* message);


/**
 * Macros to manipulate pointer tags
 */
#define IS_LEAF(x)    (((uintptr_t)x & 1))
#define SET_LEAF(x)   ((void*)((uintptr_t)x | 1))
#define LEAF_RAW(x)   ((art_leaf*)((void*)((uintptr_t)x & ~1)))

/* ============================================================
 *  [分配] NODE4/NODE16 节点缓存 (有界空闲链)
 *  单线程/外部加锁约定与本文件其余代码一致;
 *  常驻上限 ≈ 64*~56B + 32*~168B ≈ 9KB, art_tree_destroy 时清零。
 * ============================================================ */
#ifndef ART_NODE_CACHE
#define ART_NODE_CACHE 1
#endif

#if ART_NODE_CACHE
#define ART_CACHE_CAP_N4   64
#define ART_CACHE_CAP_N16  32

typedef struct {
    art_node *head;    /* 空闲链头; next 指针穿在节点头 8 字节,
                          复用前会被头部清零覆盖 (与 hashmap 同技巧) */
    uint16_t  count;
} art_node_cache;

static art_node_cache art_cache_n4  = { NULL, 0 };
static art_node_cache art_cache_n16 = { NULL, 0 };

static void art_cache_drain(void) {
    while (art_cache_n4.head) {
        art_node *next;
        __memcpy(&next, art_cache_n4.head, sizeof(next));
        kfree(art_cache_n4.head);
        art_cache_n4.head = next;
    }
    art_cache_n4.count = 0;
    while (art_cache_n16.head) {
        art_node *next;
        __memcpy(&next, art_cache_n16.head, sizeof(next));
        kfree(art_cache_n16.head);
        art_cache_n16.head = next;
    }
    art_cache_n16.count = 0;
}
#else
static void art_cache_drain(void) { }
#endif

/**
 * Allocates a node of the given type,
 * initializes to zero and sets the type.
 */
static art_node* alloc_node(uint8_t type) {
    art_node* n;
    switch (type) {
        case NODE4:
#if ART_NODE_CACHE
            if (art_cache_n4.count) {
                art_node *next;
                n = art_cache_n4.head;
                __memcpy(&next, n, sizeof(next));
                art_cache_n4.head = next;
                art_cache_n4.count--;
                /* [分配] 仅清头部 ~16B: type/num_children/partial_len/partial。
                   [num_children, cap) 的残留数组从不被解引用 (见文件头 A),
                   NODE48/256 的零值哨兵语义不受影响 —— 它们不走缓存 */
                _memset(n, 0, sizeof(art_node));
                n->type = type;
                return n;
            }
#endif
            n = (art_node*)kcalloc(1, sizeof(art_node4));
            break;
        case NODE16:
#if ART_NODE_CACHE
            if (art_cache_n16.count) {
                art_node *next;
                n = art_cache_n16.head;
                __memcpy(&next, n, sizeof(next));
                art_cache_n16.head = next;
                art_cache_n16.count--;
                _memset(n, 0, sizeof(art_node));
                n->type = type;
                return n;
            }
#endif
            n = (art_node*)kcalloc(1, sizeof(art_node16));
            break;
        case NODE48:
            /* [分配] 不走缓存: keys==0 哨兵必须整体清零 (见文件头 B) */
            n = (art_node*)kcalloc(1, sizeof(art_node48));
            break;
        case NODE256:
            n = (art_node*)kcalloc(1, sizeof(art_node256));
            break;
        default:
            Panic("Abort!");
    }
    n->type = type;
    return n;
}

/* [分配] 内层节点统一释放口: NODE4/16 入有界缓存, 48/256 直走 kfree。
   注意: 入缓存后节点头 8 字节被穿链指针覆盖, type 不再可读 ——
   调用方必须在调用前完成对节点的一切读取。 */
static void free_node(art_node *n) {
#if ART_NODE_CACHE
    switch (n->type) {
        case NODE4:
            if (art_cache_n4.count < ART_CACHE_CAP_N4) {
                __memcpy(n, &art_cache_n4.head, sizeof(art_node*));
                art_cache_n4.head = n;
                art_cache_n4.count++;
                return;
            }
            break;
        case NODE16:
            if (art_cache_n16.count < ART_CACHE_CAP_N16) {
                __memcpy(n, &art_cache_n16.head, sizeof(art_node*));
                art_cache_n16.head = n;
                art_cache_n16.count++;
                return;
            }
            break;
        default:
            break;
    }
#endif
    kfree(n);
}

/**
 * Initializes an ART tree
 * @return 0 on success.
 */
int32_t art_tree_init(art_tree *t) {
    t->root = NULL;
    t->size = 0;
    return 0;
}

/* ============================================================
 *  [栈安全] 显式游标栈基础设施 (destroy/iter 共用)
 *  栈深 = 树高 (≤ 最长键长), 与树规模无关, 堆分配按需倍增。
 * ============================================================ */
typedef struct {
    art_node *n;
    int32_t   idx;    /* 该节点下一个待访问的子槽 */
} art_frame;

static bool art_frame_reserve(art_frame **st, size_t *cap,
                               size_t used, size_t need) {
    if (likely(*cap >= need)) return true;
    size_t ncap = *cap ? *cap : 64;
    while (ncap < need) ncap <<= 1;
    art_frame *nst = (art_frame*)kmalloc(ncap * sizeof(art_frame));
    if (unlikely(!nst)) return false;
    if (*st) {
        __memcpy(nst, *st, used * sizeof(art_frame));
        kfree(*st);
    }
    *st = nst;
    *cap = ncap;
    return true;
}

/* 递归版 destroy, 保留作游标栈初始分配失败时的回退 (不差于原实现) */
static void destroy_node_recur(art_node *n) {
    if (unlikely(!n)) return;

    if (unlikely(IS_LEAF(n))) {
        kfree(LEAF_RAW(n));
        return;
    }

    int32_t i, idx;
    union {
        art_node4 *p1;
        art_node16 *p2;
        art_node48 *p3;
        art_node256 *p4;
    } p;

    switch (n->type) {
        case NODE4:
            p.p1 = (art_node4*)n;
            for (i = 0; i < n->num_children; i++) {
                destroy_node_recur(p.p1->children[i]);
            }
            break;
        case NODE16:
            p.p2 = (art_node16*)n;
            for (i = 0; i < n->num_children; i++) {
                destroy_node_recur(p.p2->children[i]);
            }
            break;
        case NODE48:
            p.p3 = (art_node48*)n;
            for (i = 0; i < 256; i++) {
                idx = p.p3->keys[i];
                if (unlikely(!idx)) continue;
                destroy_node_recur(p.p3->children[idx-1]);
            }
            break;
        case NODE256:
            p.p4 = (art_node256*)n;
            for (i = 0; i < 256; i++) {
                if (likely(p.p4->children[i]))
                    destroy_node_recur(p.p4->children[i]);
            }
            break;
        default:
            Panic("Abort!");
    }

    free_node(n);
}

/* [栈安全] destroy: 释放顺序无关紧要 → (节点, 子槽游标) 栈,
   深度 = 树高; 递归版深度 = 键长, 内核栈下长键必溢出 */
static void destroy_node(art_node *root) {
    if (unlikely(!root)) return;

    size_t cap = 0, top = 0;
    art_frame *st = NULL;
    if (unlikely(!art_frame_reserve(&st, &cap, 0, 1))) {
        destroy_node_recur(root);   /* OOM 回退: 树尚未被触碰, 安全 */
        return;
    }
    st[top].n = root; st[top].idx = 0; top = 1;

    while (likely(top)) {
        art_frame *e = &st[top - 1];
        art_node *n = e->n;

        if (unlikely(IS_LEAF(n))) {
            kfree(LEAF_RAW(n));
            top--;
            continue;
        }

        art_node *child = NULL;
        switch (n->type) {
            case NODE4: {
                if (likely(e->idx < n->num_children)) {
                    child = ((art_node4*)n)->children[e->idx++];
                }
                break;
            }
            case NODE16: {
                if (likely(e->idx < n->num_children)) {
                    child = ((art_node16*)n)->children[e->idx++];
                }
                break;
            }
            case NODE48: {
                art_node48 *p = (art_node48*)n;
                while (e->idx < 256 && !p->keys[e->idx]) e->idx++;
                if (likely(e->idx < 256)) {
                    child = p->children[p->keys[e->idx++] - 1];
                }
                break;
            }
            case NODE256: {
                art_node256 *p = (art_node256*)n;
                while (e->idx < 256 && !p->children[e->idx]) e->idx++;
                if (likely(e->idx < 256)) {
                    child = p->children[e->idx++];
                }
                break;
            }
            default:
                Panic("Abort!");
        }

        if (likely(child)) {
            if (unlikely(!art_frame_reserve(&st, &cap, top, top + 1))) {
                /* 部分遍历状态 (部分子树已释放) 无法安全回退到递归,
                   只能显式失败 — 2KB 初始栈 + 倍增下实际不可达 */
                Panic("ART: destroy stack OOM");
            }
            st[top].n = child; st[top].idx = 0; top++;
        } else {
            /* 子节点耗尽, 释放自身。
               直接 kfree 而非 free_node: destroy 尾部统一 drain,
               先入缓存再倒出只是无用功 */
            kfree(n);
            top--;
        }
    }
    kfree(st);
}

/**
 * Destroys an ART tree
 * @return 0 on success.
 */
int32_t art_tree_destroy(art_tree *t) {
    destroy_node(t->root);
    art_cache_drain();   /* [分配] 销毁时归零缓存, 不留驻留内存 */
    return 0;
}

/**
 * Returns the size of the ART tree.
 */
#ifndef BROKEN_GCC_C99_INLINE
extern inline uint64_t art_size(art_tree *t);
#endif

static art_node** find_child(art_node *n, uint8_t c) {
    union {
        art_node4 *p1;
        art_node16 *p2;
        art_node48 *p3;
        art_node256 *p4;
    } p;

    /* ========== BUG修复：修正NODE16块作用域位置，原代码块包裹了case标签 ========== */
    switch (n->type) {
        case NODE4: {
            p.p1 = (art_node4*)n;
            /* [预取] keys/children 与节点头同处 1-2 个缓存行(刚解引用过),
               预取自身数组零收益, 已删; 命中时预取目标子节点行 */
            for (int32_t i = 0; i < n->num_children; i++) {
                /* this cast works around a bug in gcc 5.1 when unrolling loops
                 * https://gcc.gnu.org/bugzilla/show_bug.cgi?id=59124
                 */
                if (unlikely(((uint8_t*)p.p1->keys)[i] == c)) {
                    PREFETCH_R(p.p1->children[i]);   /* 目标子节点行 */
                    return &p.p1->children[i];
                }
            }
            break;
        }

        case NODE16: {
            p.p2 = (art_node16*)n;
            uint32_t bitfield = 0;
            uint32_t mask;

            /* 固定 16 次比较可被向量化; 结果被 nc 掩码截断,
               缓存复用节点的残留 key 亦安全 */
            for (int32_t i = 0; i < 16; ++i) {
                if (p.p2->keys[i] == c)
                    bitfield |= (1U << i);
            }

            mask = (1U << n->num_children) - 1;
            bitfield &= mask;

            if (likely(bitfield)) {
                uint32_t idx = __builtin_ctz(bitfield);
                /* [预取] children[] 在节点第 2+ 缓存行 — 真实跨行收益 */
                PREFETCH_R(p.p2->children[idx]);
                return &p.p2->children[idx];
            }
            break;
        }

        case NODE48: {
            p.p3 = (art_node48*)n;
            /* [预取] keys[c] 紧随其后立即加载, 预取无重叠窗口(已删);
               children 槽位依赖 keys[c] 的值, 本质不可预取,
               只能预取目标行 */
            uint8_t idx = p.p3->keys[c];
            if (likely(idx)) {
                PREFETCH_R(p.p3->children[idx-1]);   /* 目标子节点行 */
                return &p.p3->children[idx-1];
            }
            break;
        }

        case NODE256: {
            p.p4 = (art_node256*)n;
            /* [预取] 槽位可能位于 2KB 节点的任意一行 → 先取槽位行,
               非空再取目标行 */
            PREFETCH_R(&p.p4->children[c]);
            if (likely(p.p4->children[c])) {
                PREFETCH_R(p.p4->children[c]);
                return &p.p4->children[c];
            }
            break;
        }

        default:
            Panic("Abort!");
    }
    return NULL;
}

// Simple inlined if
static inline int32_t _min__art(int32_t a, int32_t b) {
    return (a < b) ? a : b;
}

/**
 * Returns the number of prefix characters shared between
 * the key and node.
 */
static int32_t check_prefix(const art_node *n, const uint8_t *key, int32_t key_len, int32_t depth) {
    int32_t max_cmp = _min__art(_min__art(n->partial_len, MAX_PREFIX_LEN), key_len - depth);
    /* [预取] partial 内嵌于节点头行(读 partial_len 时已驻留), 原预取零收益, 已删 */

    int32_t idx;
    for (idx = 0; idx < max_cmp; idx++) {
        if (unlikely(n->partial[idx] != key[depth+idx]))
            return idx;
    }
    return idx;
}

/**
 * Checks if a leaf matches
 * @return 0 on success.
 */
static int32_t leaf_matches(const art_leaf *n, const uint8_t *key, int32_t key_len, int32_t depth) {
    (void)depth;
    // Fail if the key lengths are different
    if (unlikely(n->key_len != (uint32_t)key_len)) return 1;
    // Compare the keys starting at the depth
    return _memcmp(n->key, key, key_len);
}

/**
 * Searches for a value in the ART tree
 * @arg t The tree
 * @arg key The key
 * @arg key_len The length of the key
 * @return NULL if the item was not found, otherwise
 * the value pointer is returned.
 */
void* art_search(const art_tree *t, const uint8_t *key, int32_t key_len) {
    art_node **child;
    art_node *n = t->root;
    int32_t prefix_len, depth = 0;

    if (likely(n)) PREFETCH_R(n);   /* 根节点行 */

    while (likely(n)) {
        // Might be a leaf
        if (unlikely(IS_LEAF(n))) {
            art_leaf *leaf = LEAF_RAW(n);
            PREFETCH_R(leaf);       /* value/key_len/key 起始行 */
            // Check if the expanded path matches
            if (likely(!leaf_matches(leaf, key, key_len, depth))) {
                return leaf->value;
            }
            return NULL;
        }

        // Bail if the prefix does not match
        /* [分支] 路径压缩密度随负载漂移(随机键多为0/字符串键多为非0),
           数据相关 ~50/50, 不标 — 原实现的 unlikely 已移除 */
        if (n->partial_len) {
            prefix_len = check_prefix(n, key, key_len, depth);
            if (unlikely(prefix_len != _min__art(MAX_PREFIX_LEN, n->partial_len)))
                return NULL;
            depth = depth + n->partial_len;
        }

        // Recursively search
        /* find_child 内部已对目标子节点行发出预取,
           与本层返回/循环回边的周期重叠 */
        child = find_child(n, key[depth]);
        n = likely(child) ? *child : NULL;
        depth++;
    }
    return NULL;
}

// Find the minimum leaf under a node
/* [栈安全] 尾递归 → 循环; 迭代化后回边到解引用仅数条指令,
   预取窗口消失, 原递归版的子节点预取随之移除 */
static art_leaf* minimum(art_node *n) {
    while (likely(n)) {
        if (unlikely(IS_LEAF(n))) return LEAF_RAW(n);

        switch (n->type) {
            case NODE4:
                n = ((art_node4*)n)->children[0];
                break;
            case NODE16:
                n = ((art_node16*)n)->children[0];
                break;
            case NODE48: {
                art_node48 *p = (art_node48*)n;
                int32_t idx = 0;
                while (!p->keys[idx]) idx++;
                n = p->children[p->keys[idx] - 1];
                break;
            }
            case NODE256: {
                art_node256 *p = (art_node256*)n;
                int32_t idx = 0;
                while (!p->children[idx]) idx++;
                n = p->children[idx];
                break;
            }
            default:
                Panic("Abort!");
        }
    }
    return NULL;
}

// Find the maximum leaf under a node
static art_leaf* maximum(const art_node *n) {
    while (likely(n)) {
        if (unlikely(IS_LEAF(n))) return LEAF_RAW((art_node*)n);

        switch (n->type) {
            case NODE4:
                n = ((art_node4*)n)->children[n->num_children-1];
                break;
            case NODE16:
                n = ((art_node16*)n)->children[n->num_children-1];
                break;
            case NODE48: {
                art_node48 *p = (art_node48*)n;
                int32_t idx = 255;
                while (!p->keys[idx]) idx--;
                idx = p->keys[idx] - 1;
                n = p->children[idx];
                break;
            }
            case NODE256: {
                art_node256 *p = (art_node256*)n;
                int32_t idx = 255;
                while (!p->children[idx]) idx--;
                n = p->children[idx];
                break;
            }
            default:
                Panic("Abort!");
        }
    }
    return NULL;
}

/**
 * Returns the minimum valued leaf
 */
art_leaf* art_minimum(art_tree *t) {
    return minimum((art_node*)t->root);
}

/**
 * Returns the maximum valued leaf
 */
art_leaf* art_maximum(art_tree *t) {
    return maximum((art_node*)t->root);
}

static art_leaf* make_leaf(const uint8_t *key, int32_t key_len, void *value) {
    /* [分配] kmalloc + 显式初始化: 原实现 kcalloc 先清零整个 key
       区域再被 __memcpy 全量覆盖, key 越长浪费越大 (插入热路径)。
       注: art_leaf 若新增字段, 必须在此处同步初始化 */
    art_leaf *l = (art_leaf*)kmalloc(sizeof(art_leaf) + key_len);
    l->value = value;
    l->key_len = key_len;
    __memcpy(l->key, key, key_len);
    return l;
}

static int32_t longest_common_prefix(art_leaf *l1, art_leaf *l2, int32_t depth) {
    int32_t max_cmp = _min__art(l1->key_len, l2->key_len) - depth;
    int32_t idx;
    for (idx = 0; idx < max_cmp; idx++) {
        if (unlikely(l1->key[depth+idx] != l2->key[depth+idx]))
            return idx;
    }
    return idx;
}

static void copy_header(art_node *dest, art_node *src) {
    dest->num_children = src->num_children;
    dest->partial_len = src->partial_len;
    __memcpy(dest->partial, src->partial, _min__art(MAX_PREFIX_LEN, src->partial_len));
}

static void add_child256(art_node256 *n, art_node **ref, uint8_t c, void *child) {
    (void)ref;
    n->n.num_children++;
    n->children[c] = (art_node*)child;
}

static void add_child48(art_node48 *n, art_node **ref, uint8_t c, void *child);
static void add_child16(art_node16 *n, art_node **ref, uint8_t c, void *child);

static void add_child48(art_node48 *n, art_node **ref, uint8_t c, void *child) {
    if (likely(n->n.num_children < 48)) {
        /* ========== BUG修复(R1): 原 "pos = num_children" 在删除产生
           空洞后会覆盖仍存活的子节点 (children[] 与 num_children 脱钩),
           树结构静默损坏。恢复扫描首个空槽 —— 空洞稀疏且 growth 路径
           连续填充, 扫描通常 0-2 次即命中 ========== */
        int32_t pos = 0;
        while (n->children[pos]) pos++;
        n->children[pos] = (art_node*)child;
        n->keys[c] = pos + 1;
        n->n.num_children++;
    } else {
        // 扩容为冷路径，标记unlikely已由调用结构保证; NODE48 → NODE256
        art_node256 *new_node = (art_node256*)alloc_node(NODE256);
        for (int32_t i = 0; i < 256; i++) {
            if (n->keys[i]) {
                new_node->children[i] = n->children[n->keys[i] - 1];
            }
        }
        copy_header((art_node*)new_node, (art_node*)n);
        *ref = (art_node*)new_node;
        free_node((art_node*)n);
        /* [分配] 扩容路径直接落位: c 在旧节点必不存在,
           children[c] 必为 NULL, 免去经 add_child256 的分发 */
        new_node->children[c] = (art_node*)child;
        new_node->n.num_children++;
    }
}

static void add_child16(art_node16 *n, art_node **ref, uint8_t c, void *child) {
    if (likely(n->n.num_children < 16)) {
        uint32_t mask = (1U << n->n.num_children) - 1;

        // Compare the key to all stored keys
        /* [预取] keys 内嵌于节点行(读 num_children 时已驻留), 原预取零收益, 已删 */
        uint32_t bitfield = 0;
        for (int32_t i = 0; i < n->n.num_children; ++i) {
            if (c < n->keys[i])
                bitfield |= (1U << i);
        }

        bitfield &= mask;
        uint32_t idx;

        if (likely(bitfield)) {
            idx = __builtin_ctz(bitfield);
            _memmove(n->keys+idx+1, n->keys+idx, n->n.num_children - idx);
            _memmove(n->children+idx+1, n->children+idx,
                    (n->n.num_children - idx) * sizeof(void*));
        } else {
            idx = n->n.num_children;
        }

        // Set the child
        n->keys[idx] = c;
        n->children[idx] = (art_node*)child;
        n->n.num_children++;
    } else {
        // 扩容为冷路径; NODE16 → NODE48
        art_node48 *new_node = (art_node48*)alloc_node(NODE48);
        int32_t nc = n->n.num_children;
        // Copy the child pointers and populate the key map
        __memcpy(new_node->children, n->children, sizeof(void*) * nc);
        for (int32_t i = 0; i < nc; i++) {
            new_node->keys[n->keys[i]] = (uint8_t)(i + 1);
        }
        copy_header((art_node*)new_node, (art_node*)n);
        *ref = (art_node*)new_node;
        free_node((art_node*)n);
        /* [分配] 新节点 children[0..nc) 已连续填充, 首个空位确定是 nc,
           直接落位 — 免去经 add_child48 的空槽扫描与类型再分发 */
        new_node->children[nc] = (art_node*)child;
        new_node->keys[c] = (uint8_t)(nc + 1);
        new_node->n.num_children++;
    }
}

static void add_child4(art_node4 *n, art_node **ref, uint8_t c, void *child) {
    if (likely(n->n.num_children < 4)) {
        int32_t idx;
        for (idx = 0; idx < n->n.num_children; idx++) {
            if (c < n->keys[idx]) break;
        }
        // Shift to make room
        _memmove(n->keys+idx+1, n->keys+idx, n->n.num_children - idx);
        _memmove(n->children+idx+1, n->children+idx,
                (n->n.num_children - idx) * sizeof(void*));
        // Insert element
        n->keys[idx] = c;
        n->children[idx] = (art_node*)child;
        n->n.num_children++;
    } else {
        // 扩容为冷路径; NODE4 → NODE16
        /* [分配] 缓存命中时免 kcalloc+kfree 往返 — 4↔16 边界抖动
           (16 在 3 个子时缩回 4, 4 在第 5 子时扩回 16) 全被吸收 */
        art_node16 *new_node = (art_node16*)alloc_node(NODE16);
        int32_t nc = n->n.num_children;
        // Copy the child pointers and the key map
        __memcpy(new_node->children, n->children, sizeof(void*) * nc);
        __memcpy(new_node->keys, n->keys, nc);
        copy_header((art_node*)new_node, (art_node*)n);
        *ref = (art_node*)new_node;
        free_node((art_node*)n);
        /* 插入需保序 (bitfield/ctz), 复用 add_child16 落位逻辑;
           nc=4 < 16, 必然直接插入不再扩容 */
        add_child16(new_node, ref, c, child);
    }
}

static void add_child(art_node *n, art_node **ref, uint8_t c, void *child) {
    switch (n->type) {
        case NODE4:
            add_child4((art_node4*)n, ref, c, child);
            return;
        case NODE16:
            add_child16((art_node16*)n, ref, c, child);
            return;
        case NODE48:
            add_child48((art_node48*)n, ref, c, child);
            return;
        case NODE256:
            add_child256((art_node256*)n, ref, c, child);
            return;
        default:
            Panic("Abort!");
    }
}

/**
 * Calculates the index at which the prefixes mismatch
 */
static int32_t prefix_mismatch(const art_node *n, const uint8_t *key, int32_t key_len, int32_t depth) {
    int32_t max_cmp = _min__art(_min__art(MAX_PREFIX_LEN, n->partial_len), key_len - depth);
    int32_t idx;

    /* [预取] partial 内嵌于节点头行, 原预取零收益, 已删 */
    for (idx = 0; idx < max_cmp; idx++) {
        if (unlikely(n->partial[idx] != key[depth+idx]))
            return idx;
    }

    /* ========== 注释修复：原"int16_t"为拼写错误，应为"shorter than or equal to" ==========
     * If the prefix is shorter than or equal to MAX_PREFIX_LEN we can avoid finding a leaf
     */
    if (unlikely(n->partial_len > MAX_PREFIX_LEN)) {
        // Prefix is longer than what we've checked, find a leaf for full comparison
        art_leaf *l = minimum(n);   /* [栈安全] 已迭代化 */
        /* ========== 性能修复：限制比较上限为节点前缀总长度，避免多余比较 ========== */
        max_cmp = _min__art(_min__art((int32_t)l->key_len, key_len) - depth, n->partial_len);
        for (; idx < max_cmp; idx++) {
            if (unlikely(l->key[idx+depth] != key[depth+idx]))
                return idx;
        }
    }
    return idx;
}

/* [栈安全] 原 recursive_insert 唯一递归点是尾调用 → 显式迭代化。
   递归深度 = 键长, 4KB 路径键在 8-16KB 内核栈上必溢出;
   gcc -O2 通常能转换此尾调用但不保证, 显式循环无此风险。 */
static void* art_insert_internal(art_node *n, art_node **ref, const uint8_t *key,
                                  int32_t key_len, void *value, int32_t depth,
                                  int32_t *old, int32_t replace) {
    for (;;) {
        // If we are at a NULL node, inject a leaf
        if (unlikely(!n)) {
            *ref = (art_node*)SET_LEAF(make_leaf(key, key_len, value));
            return NULL;
        }

        // If we are at a leaf, we need to replace it with a node
        if (unlikely(IS_LEAF(n))) {
            art_leaf *l = LEAF_RAW(n);
            // Check if we are updating an existing value
            if (likely(!leaf_matches(l, key, key_len, depth))) {
                *old = 1;
                void *old_val = l->value;
                if (replace) l->value = value;
                return old_val;
            }

            // New value, we must split the leaf into a node4
            /* [分配] NODE4 走缓存: 叶分裂/塌缩 churn
               (交替 insert/delete 同前缀键对) 不再经过分配器 */
            art_node4 *new_node = (art_node4*)alloc_node(NODE4);
            // Create a new leaf
            art_leaf *l2 = make_leaf(key, key_len, value);
            // Determine longest prefix
            int32_t longest_prefix = longest_common_prefix(l, l2, depth);
            new_node->n.partial_len = longest_prefix;
            __memcpy(new_node->n.partial, key+depth, _min__art(MAX_PREFIX_LEN, longest_prefix));
            // Add the leafs to the new node4
            *ref = (art_node*)new_node;
            add_child4(new_node, ref, l->key[depth+longest_prefix], SET_LEAF(l));
            add_child4(new_node, ref, l2->key[depth+longest_prefix], SET_LEAF(l2));
            return NULL;
        }

        // Check if given node has a prefix
        /* [分支] 数据相关(路径压缩密度), 不标 */
        if (n->partial_len) {
            // Determine if the prefixes differ, since we need to split
            int32_t prefix_diff = prefix_mismatch(n, key, key_len, depth);
            if (likely((uint32_t)prefix_diff >= n->partial_len)) {
                depth += n->partial_len;
                goto recurse_search;
            }

            // Prefix split: cold path
            art_node4 *new_node = (art_node4*)alloc_node(NODE4);
            *ref = (art_node*)new_node;
            new_node->n.partial_len = prefix_diff;
            __memcpy(new_node->n.partial, n->partial, _min__art(MAX_PREFIX_LEN, prefix_diff));

            // Adjust the prefix of the old node
            if (likely(n->partial_len <= MAX_PREFIX_LEN)) {
                add_child4(new_node, ref, n->partial[prefix_diff], n);
                n->partial_len -= (prefix_diff+1);
                _memmove(n->partial, n->partial+prefix_diff+1,
                        _min__art(MAX_PREFIX_LEN, n->partial_len));
            } else {
                n->partial_len -= (prefix_diff+1);
                art_leaf *l = minimum(n);
                add_child4(new_node, ref, l->key[depth+prefix_diff], n);
                __memcpy(n->partial, l->key+depth+prefix_diff+1,
                        _min__art(MAX_PREFIX_LEN, n->partial_len));
            }

            // Insert the new leaf
            art_leaf *l = make_leaf(key, key_len, value);
            add_child4(new_node, ref, key[depth+prefix_diff], SET_LEAF(l));
            return NULL;
        }

    recurse_search:;
        // Find a child to recurse to
        art_node **child = find_child(n, key[depth]);
        if (likely(child)) {
            n = *child;     /* [栈安全] 尾递归 → 回边 */
            ref = child;
            depth++;
            continue;
        }

        // No child, insert directly
        art_leaf *l = make_leaf(key, key_len, value);
        add_child(n, ref, key[depth], SET_LEAF(l));
        return NULL;
    }
}

/**
 * inserts a new value into the art tree
 * @arg t the tree
 * @arg key the key
 * @arg key_len the length of the key
 * @arg value opaque value.
 * @return null if the item was newly inserted, otherwise
 * the old value pointer is returned.
 */
void* art_insert(art_tree *t, const uint8_t *key, int32_t key_len, void *value) {
    int32_t old_val = 0;
    void *old = art_insert_internal(t->root, &t->root, key, key_len, value, 0, &old_val, 1);
    if (!old_val) t->size++;
    return old;
}

/**
 * inserts a new value into the art tree (no replace)
 * @arg t the tree
 * @arg key the key
 * @arg key_len the length of the key
 * @arg value opaque value.
 * @return null if the item was newly inserted, otherwise
 * the old value pointer is returned.
 */
void* art_insert_no_replace(art_tree *t, const uint8_t *key, int32_t key_len, void *value) {
    int32_t old_val = 0;
    void *old = art_insert_internal(t->root, &t->root, key, key_len, value, 0, &old_val, 0);
    if (!old_val) t->size++;
    return old;
}

static void remove_child256(art_node256 *n, art_node **ref, uint8_t c) {
    n->children[c] = NULL;
    n->n.num_children--;

    /* [分配] 缩容迟滞: 256→48 在 37 (非 48), 与扩容点 49 之间留 11 的
       抖动间隙 — 阈值本身就是防频繁缩容的手段, 保持不变 */
    if (unlikely(n->n.num_children == 37)) {
        art_node48 *new_node = (art_node48*)alloc_node(NODE48);
        *ref = (art_node*)new_node;
        copy_header((art_node*)new_node, (art_node*)n);

        int32_t pos = 0;
        for (int32_t i = 0; i < 256; i++) {
            if (n->children[i]) {
                new_node->children[pos] = n->children[i];
                new_node->keys[i] = (uint8_t)(pos + 1);
                pos++;
            }
        }
        free_node((art_node*)n);
    }
}

static void remove_child48(art_node48 *n, art_node **ref, uint8_t c) {
    int32_t pos = n->keys[c];
    n->keys[c] = 0;
    n->children[pos-1] = NULL;   /* 空洞: add_child48 靠扫描定位 (R1) */
    n->n.num_children--;

    /* [分配] 缩容迟滞: 48→16 在 12, 与扩容点 17 之间留 5 */
    if (unlikely(n->n.num_children == 12)) {
        art_node16 *new_node = (art_node16*)alloc_node(NODE16);   /* 缓存可命中 */
        *ref = (art_node*)new_node;
        copy_header((art_node*)new_node, (art_node*)n);

        int32_t child = 0;
        for (int32_t i = 0; i < 256; i++) {
            pos = n->keys[i];
            if (pos) {
                new_node->keys[child] = (uint8_t)i;
                new_node->children[child] = n->children[pos - 1];
                child++;
            }
        }
        free_node((art_node*)n);
    }
}

static void remove_child16(art_node16 *n, art_node **ref, art_node **l) {
    int32_t pos = l - n->children;
    _memmove(n->keys+pos, n->keys+pos+1, n->n.num_children - 1 - pos);
    _memmove(n->children+pos, n->children+pos+1, (n->n.num_children - 1 - pos)*sizeof(void*));
    n->n.num_children--;

    /* [分配] 缩容迟滞: 16→4 在 3, 与扩容点 5 之间留 2;
       释放的 NODE16 入缓存, 4→16 扩容可直接复用 */
    if (unlikely(n->n.num_children == 3)) {
        art_node4 *new_node = (art_node4*)alloc_node(NODE4);
        *ref = (art_node*)new_node;
        copy_header((art_node*)new_node, (art_node*)n);
        /* ========== 修复(R2): 原拷贝 4 槽, 此时 num_children 已减为 3,
           第 4 槽为已失效的陈旧指针 — 收紧为 num_children ========== */
        __memcpy(new_node->keys, n->keys, n->n.num_children);
        __memcpy(new_node->children, n->children, n->n.num_children * sizeof(void*));
        free_node((art_node*)n);
    }
}

static void remove_child4(art_node4 *n, art_node **ref, art_node **l) {
    int32_t pos = l - n->children;
    _memmove(n->keys+pos, n->keys+pos+1, n->n.num_children - 1 - pos);
    _memmove(n->children+pos, n->children+pos+1, (n->n.num_children - 1 - pos)*sizeof(void*));
    n->n.num_children--;

    // Remove nodes with only a single child
    /* [分配] 塌缩路径零分配 (前缀拼接, 节点本体入缓存,
       与叶分裂的 alloc_node 配对吸收 churn) */
    if (unlikely(n->n.num_children == 1)) {
        art_node *child = n->children[0];
        if (!IS_LEAF(child)) {
            // Concatenate the prefixes
            int32_t prefix = n->n.partial_len;
            if (prefix < MAX_PREFIX_LEN) {
                n->n.partial[prefix] = n->keys[0];
                prefix++;
            }
            if (prefix < MAX_PREFIX_LEN) {
                int32_t sub_prefix = _min__art(child->partial_len, MAX_PREFIX_LEN - prefix);
                __memcpy(n->n.partial+prefix, child->partial, sub_prefix);
                prefix += sub_prefix;
            }
            // Store the prefix in the child
            __memcpy(child->partial, n->n.partial, _min__art(prefix, MAX_PREFIX_LEN));
            child->partial_len += n->n.partial_len + 1;
        }
        *ref = child;
        free_node((art_node*)n);
    }
}

static void remove_child(art_node *n, art_node **ref, uint8_t c, art_node **l) {
    switch (n->type) {
        case NODE4:
            remove_child4((art_node4*)n, ref, l);
            return;
        case NODE16:
            remove_child16((art_node16*)n, ref, l);
            return;
        case NODE48:
            remove_child48((art_node48*)n, ref, c);
            return;
        case NODE256:
            remove_child256((art_node256*)n, ref, c);
            return;
        default:
            Panic("Abort!");
    }
}

/* [栈安全] 原 recursive_delete 尾递归 → 显式迭代化 (同 insert) */
static art_leaf* art_delete_internal(art_node *n, art_node **ref, const uint8_t *key,
                                      int32_t key_len, int32_t depth) {
    for (;;) {
        // Search terminated
        if (unlikely(!n)) return NULL;

        // Handle hitting a leaf node
        if (unlikely(IS_LEAF(n))) {
            art_leaf *l = LEAF_RAW(n);
            if (likely(!leaf_matches(l, key, key_len, depth))) {
                *ref = NULL;
                return l;
            }
            return NULL;
        }

        // Bail if the prefix does not match
        /* [分支] 数据相关, 不标 */
        if (n->partial_len) {
            int32_t prefix_len = check_prefix(n, key, key_len, depth);
            if (unlikely(prefix_len != _min__art(MAX_PREFIX_LEN, n->partial_len))) {
                return NULL;
            }
            depth = depth + n->partial_len;
        }

        // Find child node
        art_node **child = find_child(n, key[depth]);
        if (unlikely(!child)) return NULL;

        // If the child is leaf, delete from this node
        if (unlikely(IS_LEAF(*child))) {
            art_leaf *l = LEAF_RAW(*child);
            if (likely(!leaf_matches(l, key, key_len, depth))) {
                remove_child(n, ref, key[depth], child);
                return l;
            }
            return NULL;
        }

        // Recurse → 回边
        n = *child;
        ref = child;
        depth++;
    }
}

/**
 * Deletes a value from the ART tree
 * @arg t The tree
 * @arg key The key
 * @arg key_len The length of the key
 * @return NULL if the item was not found, otherwise
 * the value pointer is returned.
 */
void* art_delete(art_tree *t, const uint8_t *key, int32_t key_len) {
    art_leaf *l = art_delete_internal(t->root, &t->root, key, key_len, 0);
    if (likely(l)) {
        t->size--;
        void *old = l->value;
        kfree(l);
        return old;
    }
    return NULL;
}

// Recursively iterates over the tree
/* 递归版保留作 iter_nodes 栈分配失败时的回退 */
static int32_t recursive_iter(art_node *n, art_callback cb, void *data) {
    // Handle base cases
    if (unlikely(!n)) return 0;
    if (unlikely(IS_LEAF(n))) {
        art_leaf *l = LEAF_RAW(n);
        return cb(data, (const uint8_t*)l->key, l->key_len, l->value);
    }

    int32_t idx, res;
    switch (n->type) {
        case NODE4:
            for (int32_t i = 0; i < n->num_children; i++) {
                PREFETCH_R(((art_node4*)n)->children[i]);
                res = recursive_iter(((art_node4*)n)->children[i], cb, data);
                if (unlikely(res)) return res;
            }
            break;
        case NODE16:
            for (int32_t i = 0; i < n->num_children; i++) {
                PREFETCH_R(((art_node16*)n)->children[i]);
                res = recursive_iter(((art_node16*)n)->children[i], cb, data);
                if (unlikely(res)) return res;
            }
            break;
        case NODE48:
            for (int32_t i = 0; i < 256; i++) {
                idx = ((art_node48*)n)->keys[i];
                if (unlikely(!idx)) continue;
                PREFETCH_R(((art_node48*)n)->children[idx-1]);
                res = recursive_iter(((art_node48*)n)->children[idx-1], cb, data);
                if (unlikely(res)) return res;
            }
            break;
        case NODE256:
            for (int32_t i = 0; i < 256; i++) {
                if (unlikely(!((art_node256*)n)->children[i])) continue;
                PREFETCH_R(((art_node256*)n)->children[i]);
                res = recursive_iter(((art_node256*)n)->children[i], cb, data);
                if (unlikely(res)) return res;
            }
            break;
        default:
            Panic("Abort!");
    }
    return 0;
}

/* [栈安全] 序保持的迭代遍历: 游标推进次序与递归版完全一致
   (NODE4/16 按 child 下标 = key 序, NODE48/256 按字节序),
   输出顺序不变; 栈深 = 树高 */
static int32_t iter_nodes(art_node *root, art_callback cb, void *data) {
    if (unlikely(!root)) return 0;

    size_t cap = 0, top = 0;
    art_frame *st = NULL;
    if (unlikely(!art_frame_reserve(&st, &cap, 0, 1))) {
        return recursive_iter(root, cb, data);   /* OOM 回退: 树未被触碰, 安全 */
    }
    st[top].n = root; st[top].idx = 0; top = 1;

    while (likely(top)) {
        art_frame *e = &st[top - 1];
        art_node *n = e->n;

        if (unlikely(IS_LEAF(n))) {
            art_leaf *l = LEAF_RAW(n);
            top--;   /* 先弹栈再回调 — 与原版语义一致 (回调只读) */
            int32_t res = cb(data, (const uint8_t*)l->key, l->key_len, l->value);
            if (unlikely(res)) {
                kfree(st);
                return res;
            }
            continue;
        }

        art_node *child = NULL;
        switch (n->type) {
            case NODE4: {
                if (likely(e->idx < n->num_children)) {
                    child = ((art_node4*)n)->children[e->idx++];
                }
                break;
            }
            case NODE16: {
                if (likely(e->idx < n->num_children)) {
                    child = ((art_node16*)n)->children[e->idx++];
                }
                break;
            }
            case NODE48: {
                art_node48 *p = (art_node48*)n;
                while (e->idx < 256 && !p->keys[e->idx]) e->idx++;
                if (likely(e->idx < 256)) {
                    child = p->children[p->keys[e->idx++] - 1];
                }
                break;
            }
            case NODE256: {
                art_node256 *p = (art_node256*)n;
                while (e->idx < 256 && !p->children[e->idx]) e->idx++;
                if (likely(e->idx < 256)) {
                    child = p->children[e->idx++];
                }
                break;
            }
            default:
                Panic("Abort!");
        }

        if (likely(child)) {
            /* [预取] 子节点行与压栈/弹栈/类型判断的十几个周期重叠;
               叶子行与回调前的弹栈路径重叠 */
            PREFETCH_R(child);
            if (unlikely(!art_frame_reserve(&st, &cap, top, top + 1))) {
                /* 部分遍历状态下回退会造成回调重复触发, 只能显式失败 */
                Panic("ART: iter stack OOM");
            }
            st[top].n = child; st[top].idx = 0; top++;
        } else {
            top--;
        }
    }
    kfree(st);
    return 0;
}

/**
 * Iterates through the entries pairs in the map,
 * invoking a callback for each. The call back gets a
 * key, value for each and returns an integer stop value.
 * If the callback returns non-zero, then the iteration stops.
 * @arg t The tree to iterate over
 * @arg cb The callback function to invoke
 * @arg data Opaque handle passed to the callback
 * @return 0 on success, or the return of the callback.
 */
int32_t art_iter(art_tree *t, art_callback cb, void *data) {
    return iter_nodes(t->root, cb, data);
}

/**
 * Checks if a leaf prefix matches
 * @return 0 on success.
 */
static int32_t leaf_prefix_matches(const art_leaf *n, const uint8_t *prefix, int32_t prefix_len) {
    /* ========== 注释修复：原"too int16_t"为拼写错误 ========== */
    // Fail if the key length is too short
    if (unlikely(n->key_len < (uint32_t)prefix_len)) return 1;
    // Compare the keys
    return _memcmp(n->key, prefix, prefix_len);
}

/**
 * Iterates through the entries pairs in the map,
 * invoking a callback for each that matches a given prefix.
 * The call back gets a key, value for each and returns an integer stop value.
 * If the callback returns non-zero, then the iteration stops.
 * @arg t The tree to iterate over
 * @arg prefix The prefix of keys to read
 * @arg prefix_len The length of the prefix
 * @arg cb The callback function to invoke
 * @arg data Opaque handle passed to the callback
 * @return 0 on success, or the return of the callback.
 */
int32_t art_iter_prefix(art_tree *t, const uint8_t *key, int32_t key_len, art_callback cb, void *data) {
    art_node **child;
    art_node *n = t->root;
    int32_t prefix_len, depth = 0;

    if (likely(n)) PREFETCH_R(n);

    while (likely(n)) {
        // Might be a leaf
        if (unlikely(IS_LEAF(n))) {
            art_leaf *leaf = LEAF_RAW(n);
            PREFETCH_R(leaf);
            // Check if the expanded path matches
            if (likely(!leaf_prefix_matches(leaf, key, key_len))) {
                return cb(data, (const uint8_t*)leaf->key, leaf->key_len, leaf->value);
            }
            return 0;
        }

        // If the depth matches the prefix, we need to handle this node
        if (unlikely(depth == key_len)) {
            art_leaf *l = minimum(n);
            if (likely(!leaf_prefix_matches(l, key, key_len)))
               return iter_nodes(n, cb, data);
            return 0;
        }

        // Bail if the prefix does not match
        /* [分支] 数据相关, 不标 */
        if (n->partial_len) {
            prefix_len = prefix_mismatch(n, key, key_len, depth);
            // Guard if the mis-match is longer than the MAX_PREFIX_LEN
            if ((uint32_t)prefix_len > n->partial_len) {
                prefix_len = n->partial_len;
            }

            // If there is no match, search is terminated
            if (unlikely(!prefix_len)) {
                return 0;
            // If we've matched the prefix, iterate on this node
            } else if (unlikely(depth + prefix_len == key_len)) {
                return iter_nodes(n, cb, data);
            }

            // if there is a full match, go deeper
            depth = depth + n->partial_len;
        }

        // Recursively search
        child = find_child(n, key[depth]);
        n = likely(child) ? *child : NULL;
        depth++;
    }
    return 0;
}