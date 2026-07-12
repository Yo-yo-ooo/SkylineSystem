#pragma once
#ifndef _RB_TREE_H_
#define _RB_TREE_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 *  兼容性宏与编译器扩展
 * ============================================================ */
#ifndef NULL
#define NULL ((void*)0)
#endif

#if !defined(_UINTPTR_T_DEFINED) && !defined(_UINTPTR_T_DECLARED) && !defined(__uintptr_t_defined)
typedef unsigned long uintptr_t;
#endif

#if defined(__GNUC__) || defined(__clang__)
#define RB_CONTAINER_OF(ptr, type, member) ({ \
    const __typeof__(((type *)0)->member) *__mptr = (ptr); \
    (type *)((char *)__mptr - offsetof(type, member)); })
#else
#define RB_CONTAINER_OF(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))
#endif

/* 允许用户通过定义 RB_NO_CONTAINER_OF 禁用通用宏，防止命名冲突 */
#ifndef RB_NO_CONTAINER_OF
#define container_of RB_CONTAINER_OF
#endif

#if defined(__x86_64__) || defined(__i386__)
#define RB_SPIN_PAUSE() __builtin_ia32_pause()
#elif defined(__aarch64__) || defined(__arm__)
#define RB_SPIN_PAUSE() __asm__ volatile("yield" ::: "memory")
#else
#define RB_SPIN_PAUSE() ((void)0)
#endif

/* ============================================================
 *  GCC __atomic 原子操作兼容层
 * ============================================================ */
#if !defined(__GNUC__) && !defined(__clang__)
#error "This header requires GCC or Clang for __atomic_* builtins"
#endif

#define RB_ATOMIC_LOAD(x)       __atomic_load_n((x), __ATOMIC_ACQUIRE)
#define RB_ATOMIC_STORE(x, v)   __atomic_store_n((x), (v), __ATOMIC_RELEASE)
#define RB_ATOMIC_ADD(x, v)     __atomic_fetch_add((x), (v), __ATOMIC_ACQ_REL)
#define RB_ATOMIC_SUB(x, v)     __atomic_fetch_sub((x), (v), __ATOMIC_ACQ_REL)
#define RB_ATOMIC_CAS(x, e, d)  __atomic_compare_exchange_n((x), (e), (d), 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)

/* ============================================================
 *  常量定义
 * ============================================================ */
#define RB_RED    0
#define RB_BLACK  1

#define RB_FLAG_COLD   0x00
#define RB_FLAG_WARM   0x01
#define RB_FLAG_HOT    0x02
#define RB_FLAG_PINNED 0x80

#define RB_FLAG_TEMP_MASK 0x03
#define RB_HINT_MASK_DEFAULT 0x03
#define RB_TIME_UPDATE_THRESHOLD 64

/* ============================================================
 *  锁原语与快捷宏
 *  约定：若初始化时传入的锁函数指针为 NULL，则所有锁操作退化为空，
 *        即等价于单线程无锁模式。
 * ============================================================ */
typedef void (*rb_lock_fn)(void *lock_ctx);
typedef void (*rb_unlock_fn)(void *lock_ctx);
typedef void (*rb_rdlock_fn)(void *lock_ctx);
typedef void (*rb_rdunlock_fn)(void *lock_ctx);

#define RB_WLOCK(r)    do { if((r) && (r)->lock)     (r)->lock((r)->lock_ctx);     } while(0)
#define RB_WUNLOCK(r)  do { if((r) && (r)->unlock)   (r)->unlock((r)->lock_ctx);   } while(0)
#define RB_RDLOCK(r)   do { if((r) && (r)->rdlock)   (r)->rdlock((r)->lock_ctx);   } while(0)
#define RB_RDUNLOCK(r) do { if((r) && (r)->rdunlock) (r)->rdunlock((r)->lock_ctx); } while(0)

/* ============================================================
 *  类型定义
 * ============================================================ */

struct rb_node;

typedef void (*rb_visit_t)(struct rb_node *node, void *arg);
typedef void (*rb_free_cb_t)(struct rb_node *node, void *arg);

/* 缓存行伪共享优化：将高频更新的热数据与冷数据分隔至不同 32 字节边界 */
typedef struct rb_node {
    /* --- 冷数据：树形结构指针 (24 bytes) --- */
    struct rb_node *parent;
    struct rb_node *left;
    struct rb_node *right;
    uint8_t  color;
    uint8_t  _pad1[7];      /* 填充至 32 字节边界 */

    /* --- 热数据：并发访问统计 (24 bytes) --- */
    uint8_t  flags;         /* 原子操作目标 */
    uint8_t  _pad2[7];      /* 对齐 access_time */
    uint64_t access_time;   /* 原子操作目标 */
    uint64_t access_count;  /* 原子操作目标 */
} rb_node_t;

typedef struct rb_root {
    rb_node_t *node;
    rb_node_t *hint;         /* 原子操作目标 */

    size_t   cnt;            /* 原子操作目标 */
    uint64_t clock;          /* 原子操作目标 */

    uint64_t   warm_decay;
    uint64_t   hot_decay;
    uint32_t   hint_mask;

    size_t stat_cold;        /* 原子操作目标 */
    size_t stat_warm;        /* 原子操作目标 */
    size_t stat_hot;         /* 原子操作目标 */
    size_t stat_pinned;      /* 原子操作目标 */

    void           *lock_ctx;
    rb_lock_fn      lock;
    rb_unlock_fn    unlock;
    rb_rdlock_fn    rdlock;
    rb_rdunlock_fn  rdunlock;
} rb_root_t;

typedef int  (*rb_compare_t)(const rb_node_t *a, const rb_node_t *b);
typedef void* (*rb_alloc_lock_fn)(uint32_t shard_idx);
typedef void (*rb_free_lock_fn)(void *lock_ctx);

/* 内存分配回调函数类型 */
typedef void* (*rb_alloc_fn)(size_t size);
typedef void  (*rb_free_fn)(void *ptr);

/* ============================================================
 *  分片键操作绑定结构
 * ============================================================ */
typedef struct rb_shard_ops {
    uint32_t (*hash_fn)(const void *key);
    const void *(*key_of)(const rb_node_t *node);
    rb_compare_t cmp;
} rb_shard_ops_t;

typedef struct rb_sharded_root {
    uint32_t shard_mask;
    uint32_t shard_num;
    rb_root_t *shards;
    rb_shard_ops_t ops;
    
    rb_alloc_fn mem_alloc; /* 节点及分片内存分配回调 */
    rb_free_fn  mem_free;  /* 节点及分片内存释放回调 */
} rb_sharded_root_t;

/* ============================================================
 *  统计信息结构
 *  注意：pinned 是属性标记，并非独立的温度状态。
 *  一个 pinned 的 HOT 节点会同时计入 stat_hot 和 stat_pinned。
 *  因此 cold + warm + hot + pinned 的总和会大于 total。
 * ============================================================ */
typedef struct rb_stats {
    size_t total;
    size_t cold;
    size_t warm;
    size_t hot;
    size_t pinned;
} rb_stats_t;

/* ============================================================
 *  内部辅助：原子状态转移与统计更新
 *  注：stat_inc/dec 在读锁(rb_touch)和写锁(erase/insert)中均可能被调用，
 *  因此必须使用原子操作。
 * ============================================================ */

static inline void rb_stat_dec(rb_root_t *root, uint8_t flags) {
    uint8_t pure = flags & RB_FLAG_TEMP_MASK;
    if (flags & RB_FLAG_PINNED) RB_ATOMIC_SUB(&root->stat_pinned, 1);
    if (pure == RB_FLAG_COLD) RB_ATOMIC_SUB(&root->stat_cold, 1);
    else if (pure == RB_FLAG_WARM) RB_ATOMIC_SUB(&root->stat_warm, 1);
    else if (pure == RB_FLAG_HOT) RB_ATOMIC_SUB(&root->stat_hot, 1);
}

static inline void rb_stat_inc(rb_root_t *root, uint8_t flags) {
    uint8_t pure = flags & RB_FLAG_TEMP_MASK;
    if (flags & RB_FLAG_PINNED) RB_ATOMIC_ADD(&root->stat_pinned, 1);
    if (pure == RB_FLAG_COLD) RB_ATOMIC_ADD(&root->stat_cold, 1);
    else if (pure == RB_FLAG_WARM) RB_ATOMIC_ADD(&root->stat_warm, 1);
    else if (pure == RB_FLAG_HOT) RB_ATOMIC_ADD(&root->stat_hot, 1);
}

/* ============================================================
 *  基础内联辅助
 * ============================================================ */

static inline void rb_init_node(rb_node_t *node) {
    if (!node) return;
    /* 初始化阶段无并发，普通赋值 */
    node->parent       = NULL;
    node->left         = NULL;
    node->right        = NULL;
    node->color        = RB_RED;
    node->flags        = RB_FLAG_COLD;
    node->access_time  = 0;
    node->access_count = 0;
}

static inline void rb_root_init(rb_root_t *root,
                                rb_lock_fn wlock, rb_unlock_fn wunlock,
                                rb_rdlock_fn rlock, rb_rdunlock_fn runlock,
                                void *lock_ctx) {
    if (!root) return;
    /* [OPT] 初始化无并发，全部改为普通赋值 */
    root->node      = NULL;
    root->hint      = NULL;
    root->cnt       = 0;
    root->clock     = 0;

    root->warm_decay = 1 << 12;
    root->hot_decay  = 1 << 14;
    root->hint_mask  = RB_HINT_MASK_DEFAULT;

    root->stat_cold   = 0;
    root->stat_warm   = 0;
    root->stat_hot    = 0;
    root->stat_pinned = 0;

    root->lock_ctx  = lock_ctx;
    root->lock      = wlock;
    root->unlock    = wunlock;
    root->rdlock    = rlock;
    root->rdunlock  = runlock;
}

static inline void rb_set_decay(rb_root_t *root, uint64_t warm_decay, uint64_t hot_decay) {
    if (root) {
        root->warm_decay = warm_decay;
        root->hot_decay  = hot_decay;
    }
}

static inline void rb_set_hint_mask(rb_root_t *root, uint32_t mask) {
    if (root) root->hint_mask = mask ? mask : RB_HINT_MASK_DEFAULT;
}

/* 推进逻辑时钟。由调用方周期性调用，冷热衰减阈值的时间单位即为 tick 数 */
static inline void rb_tick(rb_root_t *root) {
    if (root) RB_ATOMIC_ADD(&root->clock, 1);
}

/* ============================================================
 *  rb_pin_node / rb_unpin_node
 *  约束：必须在持有读锁或写锁的情况下调用，防止节点被并发删除导致野指针
 * ============================================================ */

static inline void rb_pin_node(rb_root_t *root, rb_node_t *node) {
    if (!root || !node) return;

    uint8_t old = RB_ATOMIC_LOAD(&node->flags);
    for (;;) {
        if (old & RB_FLAG_PINNED) return;

        uint8_t new_flags = old | RB_FLAG_PINNED;
        uint8_t expected = old;

        if (RB_ATOMIC_CAS(&node->flags, &expected, new_flags)) {
            rb_stat_dec(root, old);
            rb_stat_inc(root, new_flags);
            return;
        }
        old = expected;
    }
}

static inline void rb_unpin_node(rb_root_t *root, rb_node_t *node) {
    if (!root || !node) return;

    uint8_t old = RB_ATOMIC_LOAD(&node->flags);
    for (;;) {
        if (!(old & RB_FLAG_PINNED)) return;

        uint8_t new_flags = old & ~RB_FLAG_PINNED;
        uint8_t expected = old;

        if (RB_ATOMIC_CAS(&node->flags, &expected, new_flags)) {
            rb_stat_dec(root, old);
            rb_stat_inc(root, new_flags);
            return;
        }
        old = expected;
    }
}

static inline bool rb_is_pinned(const rb_node_t *node) {
    if (!node) return false;
    return RB_ATOMIC_LOAD(&node->flags) & RB_FLAG_PINNED;
}

/* ============================================================
 *  缓存友好：无锁原子冷热衰减与升级
 * ============================================================ */

static inline void rb_touch(rb_root_t *root, rb_node_t *node) {
    if (!root || !node) return;

    uint64_t cur_time = RB_ATOMIC_LOAD(&root->clock);
    uint64_t last_time = RB_ATOMIC_LOAD(&node->access_time);
    /* [OPT] 利用无符号整数天然回绕特性，简化 elapsed 计算 */
    uint64_t elapsed = cur_time - last_time;

    uint64_t count = RB_ATOMIC_ADD(&node->access_count, 1) + 1;

    if (elapsed > RB_TIME_UPDATE_THRESHOLD) {
        RB_ATOMIC_STORE(&node->access_time, cur_time);
    }

    int retries = 0;
    uint8_t cur_flags = RB_ATOMIC_LOAD(&node->flags);
    for (;;) {
        if (cur_flags & RB_FLAG_PINNED) break;

        uint8_t cur_temp = cur_flags & RB_FLAG_TEMP_MASK;
        uint8_t new_temp = cur_temp;
        bool should_decay = false;

        if (elapsed > root->hot_decay) {
            new_temp = RB_FLAG_COLD;
            should_decay = true;
        } else if (elapsed > root->warm_decay) {
            new_temp = (cur_temp == RB_FLAG_HOT) ? RB_FLAG_WARM : RB_FLAG_COLD;
            should_decay = true;
        }

        uint64_t effective_count = should_decay ? 1 : count;
        if (effective_count >= 8 && new_temp < RB_FLAG_HOT) {
            new_temp = RB_FLAG_HOT;
        } else if (effective_count >= 2 && new_temp < RB_FLAG_WARM) {
            new_temp = RB_FLAG_WARM;
        }

        if (new_temp == cur_temp) break;

        uint8_t expected = cur_flags;
        uint8_t new_flags = (cur_flags & ~RB_FLAG_TEMP_MASK) | new_temp;
        
        if (RB_ATOMIC_CAS(&node->flags, &expected, new_flags)) {
            rb_stat_dec(root, cur_flags);
            rb_stat_inc(root, new_flags);
            if (should_decay) {
                RB_ATOMIC_STORE(&node->access_count, 1);
            }
            break;
        }

        cur_flags = expected;
        if (++retries > 4) {
            RB_SPIN_PAUSE();
            retries = 0;
        }
    }
}

/* ============================================================
 *  旋转
 * ============================================================ */

static inline void rb_rotate_left(rb_node_t *node, rb_node_t **root) {
    rb_node_t *right = node->right;
    node->right = right->left;
    if (right->left) right->left->parent = node;
    right->parent = node->parent;
    if (node->parent) {
        if (node == node->parent->left) node->parent->left = right;
        else node->parent->right = right;
    } else {
        *root = right;
    }
    right->left = node;
    node->parent = right;
}

static inline void rb_rotate_right(rb_node_t *node, rb_node_t **root) {
    rb_node_t *left = node->left;
    node->left = left->right;
    if (left->right) left->right->parent = node;
    left->parent = node->parent;
    if (node->parent) {
        if (node == node->parent->right) node->parent->right = left;
        else node->parent->left = left;
    } else {
        *root = left;
    }
    left->right = node;
    node->parent = left;
}

/* ============================================================
 *  插入
 * ============================================================ */

static inline void rb_insert_raw(rb_root_t *root, rb_node_t *node, rb_compare_t cmp) {
    if (!root || !node || !cmp) return;
    rb_init_node(node);

    rb_node_t *parent  = NULL;
    rb_node_t *current = root->node;
    int cmp_res = 0;

    while (current) {
        parent = current;
        cmp_res = cmp(node, current);
        if (cmp_res < 0) current = current->left;
        else current = current->right;
    }

    node->parent = parent;
    if (!parent) root->node = node;
    else if (cmp_res < 0) parent->left = node;
    else parent->right = node;

    /* [OPT] 节点尚未接入树，仅当前线程可见，普通赋值即可 */
    node->access_time = RB_ATOMIC_LOAD(&root->clock);
    /* 写锁下调用，rb_stat_inc 内部的原子操作用于防止与读锁并发冲突 */
    rb_stat_inc(root, RB_FLAG_COLD);

    rb_node_t **proot = &root->node;
    while (node != *proot && node->parent->color == RB_RED) {
        rb_node_t *grandparent = node->parent->parent;
        if (node->parent == grandparent->left) {
            rb_node_t *uncle = grandparent->right;
            if (uncle && uncle->color == RB_RED) {
                node->parent->color = RB_BLACK;
                uncle->color        = RB_BLACK;
                grandparent->color  = RB_RED;
                node = grandparent;
            } else {
                if (node == node->parent->right) {
                    node = node->parent;
                    rb_rotate_left(node, proot);
                }
                node->parent->color = RB_BLACK;
                grandparent->color  = RB_RED;
                rb_rotate_right(grandparent, proot);
            }
        } else {
            rb_node_t *uncle = grandparent->left;
            if (uncle && uncle->color == RB_RED) {
                node->parent->color = RB_BLACK;
                uncle->color        = RB_BLACK;
                grandparent->color  = RB_RED;
                node = grandparent;
            } else {
                if (node == node->parent->left) {
                    node = node->parent;
                    rb_rotate_right(node, proot);
                }
                node->parent->color = RB_BLACK;
                grandparent->color  = RB_RED;
                rb_rotate_left(grandparent, proot);
            }
        }
    }
    (*proot)->color = RB_BLACK;
}

static inline void rb_insert(rb_root_t *root, rb_node_t *node, rb_compare_t cmp) {
    if (!root || !node || !cmp) return;
    RB_WLOCK(root);
    rb_insert_raw(root, node, cmp);
    /* [OPT] 写锁内独占，普通自增即可 */
    root->cnt++;
    RB_WUNLOCK(root);
}

/* ============================================================
 *  查找
 * ============================================================ */

static inline rb_node_t *rb_search_locked_only(rb_root_t *root, const rb_node_t *key, rb_compare_t cmp) {
    if (!root || !key || !cmp) return NULL;

    rb_node_t *current = NULL;
    rb_node_t *hint_node = RB_ATOMIC_LOAD(&root->hint);

    if (hint_node && cmp(key, hint_node) == 0) {
        current = hint_node;
    } else {
        current = root->node;
        while (current) {
            int res = cmp(key, current);
            if (res < 0) current = current->left;
            else if (res > 0) current = current->right;
            else break;
        }
    }

    if (current) {
        rb_touch(root, current);
        if (current != hint_node) {
            uint64_t clock = RB_ATOMIC_LOAD(&root->clock);
            if ((clock & root->hint_mask) == 0) {
                RB_ATOMIC_STORE(&root->hint, current);
            }
        }
    }
    return current;
}

static inline rb_node_t *rb_search(rb_root_t *root, const rb_node_t *key, rb_compare_t cmp) {
    if (!root || !key || !cmp) return NULL;
    RB_RDLOCK(root);
    rb_node_t *res = rb_search_locked_only(root, key, cmp);
    RB_RDUNLOCK(root);
    return res;
}

/* ============================================================
 *  迭代器
 * ============================================================ */

static inline rb_node_t *rb_first(rb_node_t *root) {
    if (!root) return NULL;
    while (root->left) root = root->left;
    return root;
}

static inline rb_node_t *rb_next(rb_node_t *node) {
    if (!node) return NULL;
    if (node->right) return rb_first(node->right);
    rb_node_t *parent = node->parent;
    while (parent && node == parent->right) {
        node = parent;
        parent = parent->parent;
    }
    return parent;
}

/* ============================================================
 *  删除
 * ============================================================ */

static inline void rb_erase_fixup(rb_node_t **root, rb_node_t *node, rb_node_t *parent) {
    while (node != *root && (!node || node->color == RB_BLACK)) {
        if (node == parent->left) {
            rb_node_t *sibling = parent->right;
            if (sibling->color == RB_RED) {
                sibling->color = RB_BLACK;
                parent->color  = RB_RED;
                rb_rotate_left(parent, root);
                sibling = parent->right;
            }
            if ((!sibling->left || sibling->left->color == RB_BLACK) &&
                (!sibling->right || sibling->right->color == RB_BLACK)) {
                sibling->color = RB_RED;
                node = parent;
                parent = node->parent;
            } else {
                if (!sibling->right || sibling->right->color == RB_BLACK) {
                    if (sibling->left) sibling->left->color = RB_BLACK;
                    sibling->color = RB_RED;
                    rb_rotate_right(sibling, root);
                    sibling = parent->right;
                }
                sibling->color = parent->color;
                parent->color  = RB_BLACK;
                if (sibling->right) sibling->right->color = RB_BLACK;
                rb_rotate_left(parent, root);
                node = *root;
                break;
            }
        } else {
            rb_node_t *sibling = parent->left;
            if (sibling->color == RB_RED) {
                sibling->color = RB_BLACK;
                parent->color  = RB_RED;
                rb_rotate_right(parent, root);
                sibling = parent->left;
            }
            if ((!sibling->left || sibling->left->color == RB_BLACK) &&
                (!sibling->right || sibling->right->color == RB_BLACK)) {
                sibling->color = RB_RED;
                node = parent;
                parent = node->parent;
            } else {
                if (!sibling->left || sibling->left->color == RB_BLACK) {
                    if (sibling->right) sibling->right->color = RB_BLACK;
                    sibling->color = RB_RED;
                    rb_rotate_left(sibling, root);
                    sibling = parent->left;
                }
                sibling->color = parent->color;
                parent->color  = RB_BLACK;
                if (sibling->left) sibling->left->color = RB_BLACK;
                rb_rotate_right(parent, root);
                node = *root;
                break;
            }
        }
    }
    if (node) node->color = RB_BLACK;
}

static inline void rb_erase_update_stats(rb_root_t *root, rb_node_t *node) {
    if (!root || !node) return;
    /* 写锁内读取，普通访问即可 */
    uint8_t old_flags = node->flags; 
    rb_stat_dec(root, old_flags);
    
    /* 节点字段清零，无并发风险 */
    node->flags = RB_FLAG_COLD;
    node->access_time = 0;
    node->access_count = 0;
}

static inline void rb_erase_raw(rb_root_t *root, rb_node_t *node) {
    if (!root || !node || !root->node) return;

    rb_node_t *child, *parent;
    uint8_t color;

    if (!node->left) {
        child = node->right;
    } else if (!node->right) {
        child = node->left;
    } else {
        rb_node_t *succ = rb_first(node->right);
        color = succ->color;
        child = succ->right;
        parent = succ->parent;

        if (succ->parent == node) {
            parent = succ;
        } else {
            if (child) child->parent = parent;
            parent->left = child;
            succ->right = node->right;
            node->right->parent = succ;
        }

        succ->color = node->color;
        succ->left = node->left;
        node->left->parent = succ;

        if (node->parent) {
            if (node == node->parent->left) node->parent->left = succ;
            else node->parent->right = succ;
        } else {
            root->node = succ;
        }
        succ->parent = node->parent;

        if (color == RB_BLACK) {
            rb_node_t **proot = &root->node;
            rb_erase_fixup(proot, child, parent);
        }

        rb_erase_update_stats(root, node);
        return;
    }

    parent = node->parent;
    color = node->color;

    if (child) child->parent = parent;
    if (parent) {
        if (node == parent->left) parent->left = child;
        else parent->right = child;
    } else {
        root->node = child;
    }

    if (color == RB_BLACK) {
        rb_node_t **proot = &root->node;
        rb_erase_fixup(proot, child, parent);
    }

    rb_erase_update_stats(root, node);
}

static inline void rb_clear_hint_if_match(rb_root_t *root, rb_node_t *node) {
    rb_node_t *hint_node = RB_ATOMIC_LOAD(&root->hint);
    if (hint_node == node) {
        rb_node_t *expected = node;
        RB_ATOMIC_CAS(&root->hint, &expected, NULL);
    }
}

static inline void rb_erase(rb_root_t *root, rb_node_t *node) {
    if (!root || !node) return;
    RB_WLOCK(root);
    rb_clear_hint_if_match(root, node);
    rb_erase_raw(root, node);
    /* [OPT] 写锁内独占，普通自减即可 */
    root->cnt--;
    RB_WUNLOCK(root);
}

/* ============================================================
 *  范围操作
 * ============================================================ */

static inline rb_node_t *rb_find_first_ge(rb_node_t *tree, const rb_node_t *lo_key, rb_compare_t cmp) {
    if (!tree) return NULL;
    if (!lo_key) return rb_first(tree);

    rb_node_t *start = NULL;
    while (tree) {
        if (cmp(tree, lo_key) >= 0) {
            start = tree;
            tree = tree->left;
        } else {
            tree = tree->right;
        }
    }
    return start;
}

static inline size_t rb_snapshot_range(rb_root_t *root,
                                       const rb_node_t *lo_key,
                                       const rb_node_t *hi_key,
                                       rb_compare_t cmp,
                                       rb_node_t **out_nodes,
                                       size_t max_nodes) {
    if (!root || !cmp || !out_nodes || max_nodes == 0) return 0;
    RB_RDLOCK(root);

    size_t count = 0;
    rb_node_t *cur = rb_find_first_ge(root->node, lo_key, cmp);

    while (cur && count < max_nodes) {
        if (hi_key && cmp(cur, hi_key) >= 0) break;
        out_nodes[count++] = cur;
        cur = rb_next(cur);
    }

    RB_RDUNLOCK(root);
    return count;
}

static inline size_t rb_snapshot_by_flag(rb_root_t *root, uint8_t flag_mask,
                                         bool match_exact, rb_node_t **out_nodes,
                                         size_t max_nodes) {
    if (!root || !out_nodes || max_nodes == 0) return 0;
    RB_RDLOCK(root);

    size_t count = 0;
    rb_node_t *cur = rb_first(root->node);
    while (cur && count < max_nodes) {
        uint8_t flags = RB_ATOMIC_LOAD(&cur->flags);
        bool match = match_exact ? ((flags & RB_FLAG_TEMP_MASK) == flag_mask) : ((flags & flag_mask) != 0);
        if (match) out_nodes[count++] = cur;
        cur = rb_next(cur);
    }

    RB_RDUNLOCK(root);
    return count;
}

static inline size_t rb_erase_range(rb_root_t *root,
                                    const rb_node_t *lo_key,
                                    const rb_node_t *hi_key,
                                    rb_compare_t cmp,
                                    rb_free_cb_t free_cb, void *arg) {
    if (!root || !cmp) return 0;
    RB_WLOCK(root);

    size_t erased = 0;
    rb_node_t *cur = rb_find_first_ge(root->node, lo_key, cmp);

    while (cur) {
        if (hi_key && cmp(cur, hi_key) >= 0) break;
        rb_node_t *next = rb_next(cur);

        rb_clear_hint_if_match(root, cur);
        rb_erase_raw(root, cur);
        /* [OPT] 写锁内独占，普通自减即可 */
        root->cnt--;
        if (free_cb) free_cb(cur, arg);
        erased++;
        cur = next;
    }

    RB_WUNLOCK(root);
    return erased;
}

/* ============================================================
 *  O(1) 统计接口
 * ============================================================ */
static inline void rb_get_stats(rb_root_t *root, rb_stats_t *stats) {
    if (!root || !stats) return;
    
    stats->total   = 0;
    stats->cold    = 0;
    stats->warm    = 0;
    stats->hot     = 0;
    stats->pinned  = 0;

    RB_RDLOCK(root);
    /* 读锁下并发读取，需使用原子 LOAD */
    stats->total   = RB_ATOMIC_LOAD(&root->cnt);
    stats->cold    = RB_ATOMIC_LOAD(&root->stat_cold);
    stats->warm    = RB_ATOMIC_LOAD(&root->stat_warm);
    stats->hot     = RB_ATOMIC_LOAD(&root->stat_hot);
    stats->pinned  = RB_ATOMIC_LOAD(&root->stat_pinned);
    RB_RDUNLOCK(root);
}

/* ============================================================
 *  内联辅助：反转 right 链表（Morris 后序遍历用）
 * ============================================================ */

static inline rb_node_t *rb_reverse_right_chain(rb_node_t *head) {
    rb_node_t *prev = NULL;
    while (head) {
        rb_node_t *next = head->right;
        head->right = prev;
        prev = head;
        head = next;
    }
    return prev;
}

/* ============================================================
 *  树的后序遍历（非破坏性 Morris 遍历）
 *  警告：该函数会临时修改树结构，调用方必须持有写锁！
 * ============================================================ */

static inline void rb_postorder_iter(rb_root_t *root, rb_visit_t cb, void *arg) {
    if (!root || !cb || !root->node) return;

    rb_node_t dump;
    dump.parent = NULL;
    dump.left = root->node;
    dump.right = NULL;
    root->node->parent = &dump;

    rb_node_t *cur = &dump;
    while (cur) {
        if (!cur->left) {
            cur = cur->right;
        } else {
            rb_node_t *prev = cur->left;
            while (prev->right && prev->right != cur) {
                prev = prev->right;
            }

            if (!prev->right) {
                prev->right = cur;
                cur = cur->left;
            } else {
                prev->right = NULL;

                rb_node_t *reversed = rb_reverse_right_chain(cur->left);

                rb_node_t *tmp = reversed;
                while (tmp) {
                    cb(tmp, arg);
                    tmp = tmp->right;
                }

                rb_reverse_right_chain(reversed);

                cur = cur->right;
            }
        }
    }

    if (root->node) root->node->parent = NULL;
}

static inline void rb_clear(rb_root_t *root, rb_free_cb_t free_cb, void *arg) {
    if (!root || !free_cb) return;
    RB_WLOCK(root);
    rb_postorder_iter(root, free_cb, arg);

    /* [OPT] 写锁内独占，直接普通赋值清零 */
    root->node = NULL;
    root->hint = NULL;
    root->cnt = 0;
    root->stat_cold = 0;
    root->stat_warm = 0;
    root->stat_hot = 0;
    root->stat_pinned = 0;
    RB_WUNLOCK(root);
}

/* ============================================================
 *  分片树管理
 * ============================================================ */

static inline uint32_t rb_hash_u64(const void *key) {
    uint64_t val = (uint64_t)(uintptr_t)key;
    val = (val ^ (val >> 30)) * 0xbf58476d1ce4e5b9ULL;
    val = (val ^ (val >> 27)) * 0x94d049bb133111ebULL;
    val = val ^ (val >> 31);
    return (uint32_t)val;
}

static inline uint32_t rb_default_hash(const void *key) {
    uintptr_t val = (uintptr_t)key;
    uint32_t hash = 2166136261u;
    uint8_t *bytes = (uint8_t*)&val;
    for (size_t i = 0; i < sizeof(uintptr_t); i++) {
        hash ^= bytes[i];
        hash *= 16777619u;
    }
    return hash;
}

static inline bool rb_sharded_init(rb_sharded_root_t *sroot,
                                   uint32_t shard_num,
                                   const rb_shard_ops_t *ops,
                                   rb_lock_fn wlock, rb_unlock_fn wunlock,
                                   rb_rdlock_fn rlock, rb_rdunlock_fn runlock,
                                   rb_alloc_lock_fn alloc_lock,
                                   rb_free_lock_fn free_lock,
                                   rb_alloc_fn mem_alloc,
                                   rb_free_fn mem_free) {
    if (!sroot || !ops || !ops->hash_fn || !ops->key_of || !ops->cmp)
        return false;
    if (!mem_alloc || !mem_free)
        return false;
    if (shard_num == 0 || (shard_num & (shard_num - 1)) != 0)
        return false;

    sroot->shard_num  = shard_num;
    sroot->shard_mask = shard_num - 1;
    
    sroot->mem_alloc = mem_alloc;
    sroot->mem_free  = mem_free;
    sroot->shards = (rb_root_t*)sroot->mem_alloc(sizeof(rb_root_t) * shard_num);
    if (!sroot->shards) return false;

    sroot->ops = *ops;

    for (uint32_t i = 0; i < shard_num; i++) {
        void *ctx = alloc_lock ? alloc_lock(i) : NULL;
        if (alloc_lock && !ctx) {
            for (uint32_t j = 0; j < i; j++) {
                if (free_lock) free_lock(sroot->shards[j].lock_ctx);
            }
            sroot->mem_free(sroot->shards);
            sroot->shards = NULL;
            return false;
        }
        rb_root_init(&sroot->shards[i], wlock, wunlock, rlock, runlock, ctx);
    }
    return true;
}

static inline void rb_sharded_clear(rb_sharded_root_t *sroot,
                                    rb_free_cb_t free_cb, void *arg,
                                    rb_free_lock_fn free_lock) {
    if (!sroot || !sroot->shards) return;

    for (uint32_t i = 0; i < sroot->shard_num; i++) {
        rb_clear(&sroot->shards[i], free_cb, arg);
        if (free_lock) free_lock(sroot->shards[i].lock_ctx);
        sroot->shards[i].lock_ctx = NULL;
    }

    sroot->mem_free(sroot->shards);
    sroot->shards = NULL;
    sroot->shard_num = 0;
    sroot->shard_mask = 0;
    
    sroot->ops.hash_fn = NULL;
    sroot->ops.key_of  = NULL;
    sroot->ops.cmp     = NULL;
    
    sroot->mem_alloc = NULL;
    sroot->mem_free = NULL;
}

static inline rb_root_t* rb_get_shard(rb_sharded_root_t *sroot, const void *key) {
    if (!sroot || sroot->shard_num == 0 || !sroot->ops.hash_fn) return NULL;
    return &sroot->shards[sroot->ops.hash_fn(key) & sroot->shard_mask];
}

static inline void rb_sharded_insert(rb_sharded_root_t *sroot, rb_node_t *node) {
    if (!sroot || !node || !sroot->ops.key_of) return;
    const void *key = sroot->ops.key_of(node);
    rb_root_t *shard = rb_get_shard(sroot, key);
    rb_insert(shard, node, sroot->ops.cmp);
}

static inline rb_node_t *rb_sharded_search(rb_sharded_root_t *sroot, const rb_node_t *key_node) {
    if (!sroot || !key_node || !sroot->ops.key_of) return NULL;
    const void *key = sroot->ops.key_of(key_node);
    rb_root_t *shard = rb_get_shard(sroot, key);
    return rb_search(shard, key_node, sroot->ops.cmp);
}

static inline void rb_sharded_erase(rb_sharded_root_t *sroot, rb_node_t *node) {
    if (!sroot || !node || !sroot->ops.key_of) return;
    const void *key = sroot->ops.key_of(node);
    rb_root_t *shard = rb_get_shard(sroot, key);
    rb_erase(shard, node);
}

/* 
 * 哈希分片不支持按范围定位单分片，必须遍历全部分片。
 * 警告：在大规模分片树中执行此操作会带来显著性能开销。
 */
static inline size_t rb_sharded_erase_range(rb_sharded_root_t *sroot,
                                            const rb_node_t *lo_key,
                                            const rb_node_t *hi_key,
                                            rb_free_cb_t free_cb, void *arg) {
    if (!sroot || !sroot->ops.cmp) return 0;
    size_t total_erased = 0;

    for (uint32_t i = 0; i < sroot->shard_num; i++) {
        total_erased += rb_erase_range(&sroot->shards[i], lo_key, hi_key,
                                        sroot->ops.cmp, free_cb, arg);
    }
    return total_erased;
}

/* 分片全局统计聚合接口 */
static inline void rb_sharded_get_stats(rb_sharded_root_t *sroot, rb_stats_t *stats) {
    if (!sroot || !stats) return;
    rb_stats_t tmp;
    stats->total = 0;
    stats->cold = 0;
    stats->warm = 0;
    stats->hot = 0;
    stats->pinned = 0;

    for (uint32_t i = 0; i < sroot->shard_num; i++) {
        rb_get_stats(&sroot->shards[i], &tmp);
        stats->total  += tmp.total;
        stats->cold   += tmp.cold;
        stats->warm   += tmp.warm;
        stats->hot    += tmp.hot;
        stats->pinned += tmp.pinned;
    }
}

/* 
 * 分片范围快照接口，遍历全部分片收集节点
 * 注意：返回的节点数组仅保证单个分片内有序，全局不保证有序。
 * 若需全局有序结果，需调用方自行对 out_nodes 排序。
 */
static inline size_t rb_sharded_snapshot_range(rb_sharded_root_t *sroot,
                                               const rb_node_t *lo_key,
                                               const rb_node_t *hi_key,
                                               rb_node_t **out_nodes,
                                               size_t max_nodes) {
    if (!sroot || !sroot->ops.cmp || !out_nodes || max_nodes == 0) return 0;
    size_t count = 0;
    for (uint32_t i = 0; i < sroot->shard_num && count < max_nodes; i++) {
        count += rb_snapshot_range(&sroot->shards[i], lo_key, hi_key,
                                   sroot->ops.cmp, out_nodes + count, max_nodes - count);
    }
    return count;
}

/* ============================================================
 *  校验接口
 *  约束：会递归遍历整棵树，调用期间必须持有读锁或写锁，
 *        否则并发修改会导致遍历崩溃。
 * ============================================================ */
#ifndef RB_DISABLE_VALIDATE
static inline int rb_validate_full_helper(const rb_node_t *n,
                                          const rb_node_t *lo,
                                          const rb_node_t *hi,
                                          rb_compare_t cmp, int depth) {
    if (!n) return 1;
    if (depth > 4096) return -1;

    if (lo && cmp(n, lo) < 0) return 0;
    if (hi && cmp(n, hi) >= 0) return 0;
    if (n->color == RB_RED) {
        if ((n->left && n->left->color == RB_RED) ||
            (n->right && n->right->color == RB_RED))
            return 0;
    }
    int lh = rb_validate_full_helper(n->left, lo, n, cmp, depth + 1);
    if (lh < 0) return -1;
    int rh = rb_validate_full_helper(n->right, n, hi, cmp, depth + 1);
    if (rh < 0) return -1;

    if (lh == 0 || rh == 0 || lh != rh) return 0;
    return lh + (n->color == RB_BLACK ? 1 : 0);
}

static inline bool rb_validate(const rb_root_t *root, rb_compare_t cmp) {
    if (!root || !root->node) return true;
    if (root->node->color != RB_BLACK) return false;
    int res = rb_validate_full_helper(root->node, NULL, NULL, cmp, 0);
    return res > 0;
}
#endif /* RB_DISABLE_VALIDATE */

#ifdef __cplusplus
}
#endif

#endif /* _RB_TREE_H_ */