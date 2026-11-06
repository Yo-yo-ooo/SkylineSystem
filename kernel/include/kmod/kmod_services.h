//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * kmod_services.h —— Tier0 共享服务契约（klib / sync / algo），供**每个模块**使用
 * ============================================================================
 * 背景：模块（内建组件与可加载 .ko）都需要内核 Tier0 的公共工具，但不应各自
 * 复制一份实现。Tier0 提供两种共享方式：
 *
 *   1) 直接符号共享（无需 .in）：
 *      klib 中 **C 链接** 的函数（strlen/strcmp/sprintf_/snprintf_、
 *      hashmap_* 与 art_* 容器、spinlock_*、kcalloc/krealloc/kmalloc_aligned
 *      等）加入内核 .kout 导出表；模块代码直接调用时，ELF 未定义符号在加载期
 *      按名字解析到内核地址。
 *      例：extern "C" size_t strlen(const char *);  →  直接调用。
 *
 *   2) 契约 ops 共享（推荐批量/含 C++ 链接工具）：
 *      本头定义三个版本化 ops 契约，模块用 KMOD_IMPORT_KLIB/SYNC/ALGO 导入：
 *        kernel.klib.v1 —— 字符串/格式化/排序/位图/字节加载/随机/panic
 *        kernel.sync.v1 —— 自旋锁 + 中断状态保存/恢复
 *        kernel.algo.v1 —— hashmap / ART / Queue / FIFO / List 容器
 *      内核在导入绑定阶段做机械校验：契约名相等 + abi 区间 + ops 结构尺寸
 *      完全相等（模块用 sizeof(kmod_*_ops_t) 作为 expect_size），不匹配即整批
 *      加载事务回滚。
 *
 * ABI 规则：
 *   - 结构体只允许**末尾追加**字段；追加必须升 ABI 版本并保留旧版本导出；
 *   - 模块通过 abi_min/abi_max 声明可接受版本，用 KMOD_IMPORT_* 宏时默认
 *     abi_min=1/abi_max=0（0=不设上界）+ 精确 expect_size；
 *   - 为保持布局可验证，本头不定义任何可变布局类型（全部为不透明 void*）。
 *
 * 明确**不共享**：转置序旧内存 API (_memcpy/_memset/_memmove/_memcmp)、
 * 静态缓冲 to_string/ConvertHexToString（非线程安全）、header-only 模板
 * （rbtree 等由模块自行编译头文件）、渲染器/随机数后端实现细节。
 * ==========================================================================*/
#ifndef _KMOD_SERVICES_H_
#define _KMOD_SERVICES_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <kmod/kmod_abi.h>   /* 契约名常量 + KMOD_ABI_STATIC_ASSERT */

#ifdef __cplusplus
extern "C" {
#endif

#define KMOD_KLIB_ABI_V1  1u
#define KMOD_SYNC_ABI_V1  1u
#define KMOD_ALGO_ABI_V1  1u

/* ------------------------------------------------------------------ */
/* kernel.klib.v1：字符串/格式化/排序/位图/字节序/随机/panic          */
/* ------------------------------------------------------------------ */
typedef struct kmod_klib_ops {
    uint32_t abi_version;      /* = KMOD_KLIB_ABI_V1 */
    uint32_t ops_size;         /* = sizeof(kmod_klib_ops_t) */

    size_t   (*strlen)(const char *s);
    int32_t  (*strcmp)(const char *a, const char *b);
    int32_t  (*strncmp)(const char *a, const char *b, size_t n);
    char    *(*strcpy)(char *dst, const char *src);
    char    *(*strncpy)(char *dst, const char *src, size_t n);
    char    *(*strcat)(char *dst, const char *src);
    char    *(*strchr)(const char *s, int32_t c);
    char    *(*strtok)(char *s, const char *delim);
    int32_t  (*atoi)(char *s);

    /* kprintf 家族（变参；禁止在 IRQ/调度临界路径使用） */
    int32_t  (*sprintf_)(char *buf, const char *fmt, ...);
    int32_t  (*snprintf_)(char *buf, size_t n, const char *fmt, ...);

    void     (*qsort)(void *base, size_t num, size_t width,
                      int32_t (*cmp)(const void *a, const void *b));

    void     (*bitmap_set)(uint8_t *bitmap, uint64_t bit);
    void     (*bitmap_clear)(uint8_t *bitmap, uint64_t bit);
    bool     (*bitmap_get)(uint8_t *bitmap, uint64_t bit);

    uint16_t (*kld16)(const uint8_t *p);   /* little-endian 加载 */
    uint32_t (*kld32)(const uint8_t *p);
    uint64_t (*kld64)(const uint8_t *p);

    uint64_t (*random_u64)(void);          /* 内核 RNG（lehmer64） */
    uint64_t (*random_range)(uint64_t lo, uint64_t hi);  /* [lo,hi]；lo>hi 返回 lo */

    void     (*panic)(const char *message); /* Tier0 Panic（打印并停机） */
} kmod_klib_ops_t;

/* ------------------------------------------------------------------ */
/* kernel.sync.v1：自旋锁与中断状态（热路径只做最小包装）              */
/* ------------------------------------------------------------------ */
typedef struct kmod_sync_ops {
    uint32_t abi_version;      /* = KMOD_SYNC_ABI_V1 */
    uint32_t ops_size;

    void     (*spinlock_lock)(int32_t *lock);
    void     (*spinlock_unlock)(int32_t *lock);
    uint64_t (*irq_save)(void);            /* 关中断并返回原 flags（配合 restore） */
    void     (*irq_restore)(uint64_t flags);
    int32_t  (*irq_state)(void);           /* 1 = 中断开启，0 = 关闭 */
} kmod_sync_ops_t;

/* ------------------------------------------------------------------ */
/* kernel.algo.v1：klib 容器（全部用不透明 void* 句柄，布局由内核私有） */
/* ------------------------------------------------------------------ */
typedef uint64_t (*kmod_hm_hash_fn)(const void *item, uint64_t seed0,
                                    uint64_t seed1);
typedef int32_t  (*kmod_hm_cmp_fn)(const void *a, const void *b, void *udata);
typedef void     (*kmod_hm_free_fn)(void *item);
typedef bool     (*kmod_hm_scan_fn)(const void *item, void *udata);
typedef int32_t  (*kmod_art_cb_fn)(void *data, const uint8_t *key,
                                   uint32_t key_len, void *value);

typedef struct kmod_algo_ops {
    uint32_t abi_version;      /* = KMOD_ALGO_ABI_V1 */
    uint32_t ops_size;

    /* ---- hashmap（与 klib/algorithm/hmap.h 语义一致） ---- */
    void        *(*hashmap_new)(size_t elsize, size_t cap, uint64_t seed0,
                                uint64_t seed1, kmod_hm_hash_fn hash,
                                kmod_hm_cmp_fn compare, kmod_hm_free_fn elfree,
                                void *udata);
    void         (*hashmap_free)(void *map);
    size_t       (*hashmap_count)(const void *map);
    const void  *(*hashmap_get)(const void *map, const void *item);
    const void  *(*hashmap_set)(void *map, const void *item);
    const void  *(*hashmap_delete)(void *map, const void *item);
    bool         (*hashmap_scan)(void *map, kmod_hm_scan_fn iter, void *udata);
    bool         (*hashmap_iter)(void *map, size_t *i, void **item);
    uint64_t     (*hashmap_sip)(const void *data, size_t len, uint64_t seed0,
                                uint64_t seed1);

    /* ---- ART（与 klib/algorithm/art.h 语义一致；句柄为 art_tree*） ---- */
    int32_t      (*art_tree_init)(void *tree);
    int32_t      (*art_tree_destroy)(void *tree);
    void        *(*art_insert)(void *tree, const uint8_t *key, int32_t key_len,
                               void *value);
    void        *(*art_delete)(void *tree, const uint8_t *key, int32_t key_len);
    void        *(*art_search)(const void *tree, const uint8_t *key,
                               int32_t key_len);
    int32_t      (*art_iter)(void *tree, kmod_art_cb_fn cb, void *data);
    int32_t      (*art_iter_prefix)(void *tree, const uint8_t *prefix,
                                    int32_t prefix_len, kmod_art_cb_fn cb,
                                    void *data);
    void        *(*art_minimum)(void *tree);
    void        *(*art_maximum)(void *tree);

    /* ---- Queue（klib/algorithm/queue.h） ---- */
    void        *(*queue_create)(void);
    void        *(*queue_append)(void *queue, void *data);
    void        *(*queue_dequeue)(void *queue);
    void         (*queue_destroy)(void *queue);

    /* ---- FIFO（klib/fifo.h） ---- */
    void        *(*fifo_create)(uint64_t cap, uint64_t item_size);
    void         (*fifo_push)(void *f, void *val);
    void         (*fifo_pop)(void *f, void *buffer);
    void         (*fifo_get)(void *f, void *buffer);

    /* ---- List（klib/list.h） ---- */
    void        *(*list_create)(void);
    void         (*list_add)(void *list, void *value);
    void         (*list_remove)(void *list, void *item);
    void        *(*list_iterate)(void *list, bool wrap);
    void        *(*list_find)(void *list, void *value);
} kmod_algo_ops_t;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* _KMOD_SERVICES_H_ */
