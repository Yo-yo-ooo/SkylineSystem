
//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
#include <limine.h>
#include <arch/x86_64/allin.h>
#include <conf.h>
#include <arch/x86_64/vmm/vmm.h>


extern volatile bool IsPM5LVL;

namespace VMM{
    namespace VMA {

        /* ---------- 红黑树比较函数：按 start 地址排序 ---------- */
        static int vma_rb_cmp(const rb_node_t *a, const rb_node_t *b) {
            const vma_region_t *ra = container_of(a, vma_region_t, rb_node);
            const vma_region_t *rb = container_of(b, vma_region_t, rb_node);
            if (ra->start < rb->start) return -1;
            if (ra->start > rb->start) return  1;
            return 0;
        }

        /* ---------- 树内查找：start <= addr 的最大区域 ---------- */
        static vma_region_t *vma_tree_find_le(rb_root_t *root, uint64_t addr) {
            rb_node_t *cur = root->node;
            vma_region_t *best = nullptr;
            while (cur) {
                vma_region_t *r = container_of(cur, vma_region_t, rb_node);
                if (r->start <= addr) {
                    best = r;
                    cur  = cur->right;
                } else {
                    cur  = cur->left;
                }
            }
            return best;
        }

        /* ---------- 树内查找：start > addr 的最小区域 ---------- */
        static vma_region_t *vma_tree_find_gt(rb_root_t *root, uint64_t addr) {
            rb_node_t *cur = root->node;
            vma_region_t *best = nullptr;
            while (cur) {
                vma_region_t *r = container_of(cur, vma_region_t, rb_node);
                if (r->start > addr) {
                    best = r;
                    cur  = cur->left;
                } else {
                    cur  = cur->right;
                }
            }
            return best;
        }

        /* ==========================================================
        *  SetStart — 初始化哨兵 + 红黑树
        * ========================================================== */
        void SetStart(pagemap_t *pagemap, uint64_t start, uint64_t page_count) {
            vma_region_t *sentinel = HIGHER_HALF((vma_region_t*)PMM::Request());
            sentinel->start       = start;
            sentinel->page_count  = 0;
            sentinel->flags       = 0;
            sentinel->next        = sentinel;
            sentinel->prev        = sentinel;
            sentinel->rb_root     = nullptr;      /* 哨兵不加入树 */
            pagemap->vma_head     = sentinel;
            pagemap->vma_cursor   = sentinel;

            /* 初始化红黑树（无内部锁，由外部 vma_lock spinlock 保护）*/
            rb_root_init(&pagemap->vma_tree,
                        nullptr, nullptr,
                        nullptr, nullptr,
                        nullptr);
        }

        /* ==========================================================
        *  InsertRegion — 链表插入（底层操作，树插入由调用方完成）
        *  保持原版签名和行为兼容
        * ========================================================== */
        vma_region_t *InsertRegion(vma_region_t *after,
                                uint64_t start,
                                uint64_t page_count,
                                uint64_t flags) {
            vma_region_t *region = HIGHER_HALF((vma_region_t*)PMM::Request());
            region->start      = start;
            region->page_count = page_count;
            region->flags      = flags;
            region->rb_root    = nullptr;         /* 调用方负责设置并插入树 */
            region->prev       = after;
            region->next       = after->next;
            after->next->prev  = region;
            after->next        = region;
            return region;
        }

        /* ==========================================================
        *  AddRegion — 尾部追加区域（链表 + 红黑树同步插入）
        * ========================================================== */
        vma_region_t *AddRegion(pagemap_t *pagemap,
                                uint64_t start,
                                uint64_t page_count,
                                uint64_t flags) {
            vma_region_t *region = InsertRegion(pagemap->vma_head->prev,
                                                start, page_count, flags);
            /* 同步插入红黑树 */
            region->rb_root = &pagemap->vma_tree;
            rb_insert(&pagemap->vma_tree, &region->rb_node, vma_rb_cmp);
            return region;
        }

        /* ==========================================================
        *  RemoveRegion — 从红黑树 + 链表同步移除并释放
        *  通过 rb_root 反向指针自动定位树根，调用方无需额外操作
        * ========================================================== */
        void RemoveRegion(vma_region_t *region) {
            /* 从红黑树中移除 */
            if (region->rb_root) {
                rb_erase(region->rb_root, &region->rb_node);
                region->rb_root = nullptr;
            }
            /* 从链表中移除 */
            region->next->prev = region->prev;
            region->prev->next = region->next;
            PMM::Free(PHYSICAL((void*)region));
        }

        /* ==========================================================
        *  FindRegion — O(log n) 红黑树查找
        *  原版 O(n) 线性扫描 → 优化为 O(log n) 树搜索
        * ========================================================== */
        vma_region_t *FindRegion(pagemap_t *pagemap, uint64_t addr) {
            uint64_t page_addr = ALIGN_DOWN(addr, PAGE_SIZE);

            /* 在树中找 start <= page_addr 的最大区域 */
            vma_region_t *best = vma_tree_find_le(&pagemap->vma_tree, page_addr);
            if (!best) return nullptr;

            uint64_t end = best->start + best->page_count * PAGE_SIZE;
            if (page_addr >= best->start && page_addr < end)
                return best;
            return nullptr;
        }

        /* ==========================================================
        *  InternalAlloc — 红黑树优化分配
        *  - hint 路径：O(log n) 前驱/后继查找
        *  - best-fit 路径：O(n) 树中序遍历（替代链表扫描）
        * ========================================================== */
        uint64_t InternalAlloc(pagemap_t *pagemap,
                            uint64_t page_count,
                            uint64_t flags,
                            uint64_t hint) {
            const uint64_t need = page_count * PAGE_SIZE;
            const uint64_t lo   = pagemap->vma_head->start;
            const uint64_t hi   = is_user_address(0)
                                    ? 0xFFFF800000000000ULL
                                    : USER_SPACE_END_5LVL;

            /* ----------------------------------------------------
            * 1. hint 优先 — O(log n) 查找前驱和后继
            * ---------------------------------------------------- */
            if (hint >= lo && hint + need <= hi) {
                vma_region_t *prev_r = vma_tree_find_le(&pagemap->vma_tree, hint);
                vma_region_t *next_r = vma_tree_find_gt(&pagemap->vma_tree, hint);

                uint64_t prev_end  = prev_r
                    ? (prev_r->start + prev_r->page_count * PAGE_SIZE)
                    : lo;
                uint64_t cur_start = next_r ? next_r->start : hi;

                if (hint >= prev_end && hint + need <= cur_start) {
                    /* 链表前驱 = 树前驱；若无树前驱则插入到哨兵之后 */
                    vma_region_t *after = prev_r ? prev_r : pagemap->vma_head;
                    vma_region_t *r = InsertRegion(after, hint, page_count, flags);
                    r->rb_root = &pagemap->vma_tree;
                    rb_insert(&pagemap->vma_tree, &r->rb_node, vma_rb_cmp);
                    pagemap->vma_cursor = r;
                    return hint;
                }
            }

            /* ----------------------------------------------------
            * 2. Best-fit 扫描 — 红黑树中序遍历
            *    间隙 = cur.start - prev.end
            * ---------------------------------------------------- */
            vma_region_t *best_after = nullptr;   /* 最佳插入点（链表前驱）*/
            uint64_t      best_addr  = 0;
            uint64_t      best_gap   = UINT64_MAX;

            rb_node_t *prev = nullptr;
            rb_node_t *cur  = rb_first(pagemap->vma_tree.node);

            while (cur) {
                vma_region_t *cur_r  = container_of(cur,  vma_region_t, rb_node);
                vma_region_t *prev_r = prev ? container_of(prev, vma_region_t, rb_node) : nullptr;

                uint64_t prev_end  = prev_r
                    ? (prev_r->start + prev_r->page_count * PAGE_SIZE)
                    : lo;
                uint64_t cur_start = cur_r->start;
                uint64_t gap       = cur_start - prev_end;

                if (gap >= need && gap < best_gap) {
                    best_after = prev_r;     /* nullptr 表示插到哨兵后 */
                    best_addr  = prev_end;
                    best_gap   = gap;
                }

                prev = cur;
                cur  = rb_next(cur);
            }

            /* 检查最后一个区域到 hi 的间隙 */
            if (prev) {
                vma_region_t *prev_r   = container_of(prev, vma_region_t, rb_node);
                uint64_t      prev_end = prev_r->start + prev_r->page_count * PAGE_SIZE;
                uint64_t      gap      = hi - prev_end;
                if (gap >= need && gap < best_gap) {
                    best_after = prev_r;
                    best_addr  = prev_end;
                    best_gap   = gap;
                }
            } else {
                /* 树为空：整个 [lo, hi) 可用 */
                if (hi - lo >= need) {
                    best_addr = lo;
                    best_gap  = hi - lo;
                }
            }

            if (best_gap == UINT64_MAX)
                return 0;

            /* 执行插入：链表 + 红黑树同步 */
            vma_region_t *after = best_after ? best_after : pagemap->vma_head;
            vma_region_t *r = InsertRegion(after, best_addr, page_count, flags);
            r->rb_root = &pagemap->vma_tree;
            rb_insert(&pagemap->vma_tree, &r->rb_node, vma_rb_cmp);
            pagemap->vma_cursor = r;
            return best_addr;
        }

    } // namespace VMA
    
} // namespace VMM
