// SPDX-FileCopyrightText: 2026 Yo-yo-ooo
// SPDX-License-Identifier: GPL-2.0-only
#include <limine.h>
#include <conf.h>
#include <arch/x86_64/vmm/vmm.h>
#include <mem/pmm.h>
#include <klib/algorithm/rbtree.h>
#include <klib/klib.h>

extern volatile bool IsPM5LVL;

// ─── 分支提示:只标「结构性不对称」的分支 ───
// 判据:空指针守卫、调用模式恒定(如 hint==0)、罕见事件。
// ~50/50 的比较(r->start <= addr)不标 —— 猜错比不猜糟。
#define vma_likely(x)     __builtin_expect(!!(x), 1)
#define vma_unlikely(x)   __builtin_expect(!!(x), 0)

namespace VMM{
    namespace VMA {

        

        // 区域终点:三处重复表达式收敛(下溢钳制只需写一次)
        static inline uint64_t region_end(const vma_region_t *r) {
            return r->start + r->page_count * PAGE_SIZE;
        }

        static int vma_rb_cmp(const rb_node_t *a, const rb_node_t *b) {
            const vma_region_t *ra = container_of(a, vma_region_t, rb_node);
            const vma_region_t *rb = container_of(b, vma_region_t, rb_node);
            if (ra->start < rb->start) return -1;
            if (ra->start > rb->start) return  1;
            return 0;
        }

        static vma_region_t *vma_tree_find_le(rb_root_t *root, uint64_t addr) {
            rb_node_t *cur = root->node;
            vma_region_t *best = nullptr;
            while (vma_likely(cur)) {
                vma_region_t *r = container_of(cur, vma_region_t, rb_node);
                /* 预取:两个孩子指针此刻已在手(本行已命中),
                   比较期间让内存系统并行去取两个候选孩子的行。
                   树在 L1/L2 内 → 预取命中被丢弃(≈1 µop 开销);
                   树超出 L2   → 逐级串行 miss 变成重叠。
                   prefetch 对规范地址永不故障,NULL 也安全。 */
                __builtin_prefetch(cur->left);
                __builtin_prefetch(cur->right);
                if (r->start <= addr) {
                    best = r;
                    cur  = cur->right;
                } else {
                    cur  = cur->left;
                }
            }
            return best;
        }

        static vma_region_t *vma_tree_find_gt(rb_root_t *root, uint64_t addr) {
            rb_node_t *cur = root->node;
            vma_region_t *best = nullptr;
            while (vma_likely(cur)) {
                vma_region_t *r = container_of(cur, vma_region_t, rb_node);
                __builtin_prefetch(cur->left);
                __builtin_prefetch(cur->right);
                if (r->start > addr) {
                    best = r;
                    cur  = cur->left;
                } else {
                    cur  = cur->right;
                }
            }
            return best;
        }

        // Detach a region from both the ordered list and the rb-tree, release it.
        static void vma_destroy_locked(pagemap_t *pagemap, vma_region_t *r) {
            if (pagemap->vma_cursor == r) pagemap->vma_cursor = r->prev;
            if (r->rb_root) { rb_erase(r->rb_root, &r->rb_node); r->rb_root = nullptr; }
            r->next->prev = r->prev;
            r->prev->next = r->next;
            PMM::Free(PHYSICAL((void*)r));
        }

        // Split @region at a page-aligned @split_addr. The original node keeps
        // [start, split); a freshly allocated node owns [split, end) and inherits
        // the flags. Returns the new tail node, nullptr on a bad argument.
        vma_region_t *SplitRegion(pagemap_t *pagemap, vma_region_t *region, uint64_t split_addr) {
            if (vma_unlikely(!pagemap || !region || region == pagemap->vma_head)) return nullptr;
            uint64_t split = ALIGN_DOWN(split_addr, PAGE_SIZE);
            uint64_t r_start = region->start;
            uint64_t r_end   = region_end(region);
            if (vma_unlikely(split <= r_start || split >= r_end)) return nullptr;

            uint64_t head_pages = (split - r_start) / PAGE_SIZE;
            uint64_t tail_pages = (r_end - split) / PAGE_SIZE;

            vma_region_t *tail = InsertRegion(region, split, tail_pages, region->flags);
            if (vma_unlikely(!tail)) return nullptr;
            region->page_count = head_pages;
            tail->rb_root = &pagemap->vma_tree;
            rb_insert(&pagemap->vma_tree, &tail->rb_node, vma_rb_cmp);
            return tail;
        }

        // Coalesce @region with physically adjacent list neighbours that carry
        // identical flags. Returns the surviving node (== @region if no merge).
        // Adjacency is verified by address bounds in addition to list position,
        // so a non-ordered list can only under-merge, never fuse unrelated ranges.
        vma_region_t *MergeRegion(pagemap_t *pagemap, vma_region_t *region) {
            if (vma_unlikely(!pagemap || !region || region == pagemap->vma_head)) return region;
            vma_region_t *sentinel = pagemap->vma_head;

            // Absorb following neighbours first.
            vma_region_t *cur = region->next;
            while (cur != sentinel &&
                   region_end(region) == cur->start &&
                   cur->flags == region->flags) {
                vma_region_t *nxt = cur->next;
                region->page_count += cur->page_count;
                vma_destroy_locked(pagemap, cur);
                cur = nxt;
            }

            // Then let identical predecessors absorb @region, walking backward.
            vma_region_t *prev = region->prev;
            while (prev != sentinel &&
                   region_end(prev) == region->start &&
                   prev->flags == region->flags) {
                vma_region_t *before = prev->prev;
                prev->page_count += region->page_count;
                vma_destroy_locked(pagemap, region);
                region = prev;
                prev = before;
            }
            return region;
        }

        void SetStart(pagemap_t *pagemap, uint64_t start, uint64_t page_count) {
            (void)page_count;
            /* 幂等:已初始化的 pagemap 只调整下界。
               原版重复调用会分配新哨兵(泄漏整页)并重置红黑树 ——
               若此前已有 AddRegion 登记,区域全部静默丢失。
               NewPM 一次 + elf_load 一次是既定调用序列,必须幂等。 */
            if (pagemap->vma_head != nullptr) {
                pagemap->vma_head->start = start;
                return;
            }
            vma_region_t *sentinel = HIGHER_HALF((vma_region_t*)PMM::Request());
            if (vma_unlikely(!sentinel)) return;
            sentinel->start       = start;
            sentinel->page_count  = 0;
            sentinel->flags       = 0;
            sentinel->next        = sentinel;
            sentinel->prev        = sentinel;
            sentinel->rb_root     = nullptr;
            pagemap->vma_head     = sentinel;
            pagemap->vma_cursor   = sentinel;

            rb_root_init(&pagemap->vma_tree, nullptr, nullptr, nullptr, nullptr, nullptr);
        }

        vma_region_t *InsertRegion(vma_region_t *after, uint64_t start, uint64_t page_count, uint64_t flags) {
            if (vma_unlikely(!after)) return nullptr;
            vma_region_t *region = HIGHER_HALF((vma_region_t*)PMM::Request());
            if (vma_unlikely(!region)) return nullptr;   /*  PMM 耗尽,调用方须判空 */
            region->start      = start;
            region->page_count = page_count;
            region->flags      = flags;
            region->rb_root    = nullptr;
            region->prev       = after;
            region->next       = after->next;
            after->next->prev  = region;
            after->next        = region;
            return region;
        }

        vma_region_t *AddRegion(pagemap_t *pagemap, uint64_t start, uint64_t page_count, uint64_t flags) {
            if (vma_unlikely(!pagemap->vma_head || page_count == 0)) return nullptr;
            vma_region_t *region = InsertRegion(pagemap->vma_head->prev, start, page_count, flags);
            if (vma_unlikely(!region)) return nullptr;
            region->rb_root = &pagemap->vma_tree;
            rb_insert(&pagemap->vma_tree, &region->rb_node, vma_rb_cmp);
            // Auto-coalesce adjacent ranges with identical flags so back-to-back
            // PT_LOAD/mmap regions never accumulate a duplicate tree key.
            return MergeRegion(pagemap, region);
        }

        bool IsRangeFree(pagemap_t *pagemap, uint64_t start, uint64_t page_count) {
            if (vma_unlikely(page_count == 0)) return false;
            uint64_t end = start + page_count * PAGE_SIZE;
            if (vma_unlikely(end < start)) return false;          /*  回绕 */

            vma_region_t *prev_r = vma_tree_find_le(&pagemap->vma_tree, start);
            if (prev_r && region_end(prev_r) > start)
                return false;

            vma_region_t *next_r = vma_tree_find_gt(&pagemap->vma_tree, start);
            if (next_r && next_r->start < end)
                return false;

            return true;
        }

        void RemoveRegion(pagemap_t *pagemap, vma_region_t *region) {
            if (vma_unlikely(!pagemap || !region)) return;

            // 游标恰好指向被删节点:回退(.prev 为哨兵也合法)
            if (vma_unlikely(pagemap->vma_cursor == region))
                pagemap->vma_cursor = region->prev;

            if (vma_likely(region->rb_root)) {       // 真实区域必在树上,仅哨兵为 null
                rb_erase(region->rb_root, &region->rb_node);
                region->rb_root = nullptr;
            }
            region->next->prev = region->prev;
            region->prev->next = region->next;
            PMM::Free(PHYSICAL((void*)region));
        }

        vma_region_t *FindRegion(pagemap_t *pagemap, uint64_t addr) {
            if (vma_unlikely(!pagemap || !pagemap->vma_head)) return nullptr;
            uint64_t page_addr = ALIGN_DOWN(addr, PAGE_SIZE);
            vma_region_t *best = vma_tree_find_le(&pagemap->vma_tree, page_addr);
            if (vma_unlikely(!best)) return nullptr;
            if (vma_likely(page_addr >= best->start && page_addr < region_end(best)))
                return best;                          // 调用方(VMM::Free 等)均为存在性查询
            return nullptr;
        }

        uint64_t InternalAlloc(pagemap_t *pagemap, uint64_t page_count, uint64_t flags, uint64_t hint) {
            if (vma_unlikely(page_count == 0)) return 0;

            uint64_t need = page_count * PAGE_SIZE;
            if (vma_unlikely(need / PAGE_SIZE != page_count)) return 0;  /*  乘法回绕 */

            const uint64_t lo = pagemap->vma_head->start;
            /* 上界:lo 在哪个半区就用哪个半区顶端。
               原三目 is_user_address(0)?...:... 两种分页模式下恒真:
               4 级下用户 VMA 可被分进非规范空洞;内核 pagemap 则 hi<lo,
               全靠 hi-lo 无符号下溢「碰巧」放行。现在语义显式。 */
            const uint64_t hi = (lo >= HIGHER_HALF(0))
                              ? 0xFFFFFFFFFFFFFFFFULL
                              : (IsPM5LVL ? USER_SPACE_END_5LVL : 0x800000000000ULL);
            if (vma_unlikely(lo >= hi || need > hi - lo)) return 0;      /*  此后 hi-lo 恒正 */

            // 1. Hint 路径:现有调用方(VMM::Alloc/EAlloc)全部传 0 → 结构性罕见
            if (vma_unlikely(hint >= lo && hint <= hi - need)) {
                vma_region_t *prev_r = vma_tree_find_le(&pagemap->vma_tree, hint);
                vma_region_t *next_r = vma_tree_find_gt(&pagemap->vma_tree, hint);

                uint64_t prev_end  = prev_r ? region_end(prev_r) : lo;
                if (prev_end < lo) prev_end = lo;    /*  ELF 镜像区域可低于 lo */
                uint64_t cur_start = next_r ? next_r->start : hi;

                if (hint >= prev_end && hint + need <= cur_start) {
                    vma_region_t *r = InsertRegion(prev_r ? prev_r : pagemap->vma_head,
                                                   hint, page_count, flags);
                    if (vma_unlikely(!r)) return 0;
                    r->rb_root = &pagemap->vma_tree;
                    rb_insert(&pagemap->vma_tree, &r->rb_node, vma_rb_cmp);
                    pagemap->vma_cursor = r;
                    return hint;
                }
            }

            // 2. Next-Fit 环形扫描
            vma_region_t *best_after = nullptr;
            uint64_t      best_addr  = 0;
            bool          found      = false;

            rb_node_t *start_node = (pagemap->vma_cursor && pagemap->vma_cursor != pagemap->vma_head)
                                    ? &pagemap->vma_cursor->rb_node
                                    : rb_first(pagemap->vma_tree.node);

            if (vma_unlikely(!start_node)) {
                best_addr = lo;                       // 空树:前面守卫已保证 [lo,hi) 装得下
                found = true;
            } else {
                rb_node_t *cur_node = start_node;
                bool is_first_iteration = true;

                while (cur_node && (is_first_iteration || cur_node != start_node)) {
                    is_first_iteration = false;
                    vma_region_t *cur_r = container_of(cur_node, vma_region_t, rb_node);
                    rb_node_t *next_node = rb_next(cur_node);

                    /* 预取下一节点容器行:start 下一轮立刻要用 */
                    if (vma_likely(next_node))
                        __builtin_prefetch(container_of(next_node, vma_region_t, rb_node));

                    /* 间隙 = [max(region_end, lo), next.start|hi)。
                       先钳制再相减,消灭三处下溢:
                       区域低于 lo / 区域相邻重叠时间隙为 0 自然跳过 */
                    uint64_t gap_lo = region_end(cur_r);
                    if (gap_lo < lo) gap_lo = lo;
                    uint64_t gap_hi = next_node
                        ? container_of(next_node, vma_region_t, rb_node)->start
                        : hi;

                    /* 顺序分配(栈/fx/TLS 逐段向上顶)是主导负载:
                       游标处间隙通常一步命中 */
                    if (vma_likely(gap_hi > gap_lo && gap_hi - gap_lo >= need)) {
                        best_after = cur_r;
                        best_addr  = gap_lo;
                        found = true;
                        break;
                    }

                    if (next_node) {
                        cur_node = next_node;
                    } else {
                        // 扫过地址最大区域:回头查头部间隙 [lo, first.start)
                        cur_node = rb_first(pagemap->vma_tree.node);
                        if (cur_node) {
                            vma_region_t *first_r = container_of(cur_node, vma_region_t, rb_node);
                            if (first_r->start > lo && first_r->start - lo >= need) {  /*  钳制 */
                                best_after = nullptr;
                                best_addr  = lo;
                                found = true;
                                break;
                            }
                        }
                    }
                }
            }

            if (vma_unlikely(!found)) return 0;

            vma_region_t *after = best_after ? best_after : pagemap->vma_head;
            vma_region_t *r = InsertRegion(after, best_addr, page_count, flags);
            if (vma_unlikely(!r)) return 0;
            r->rb_root = &pagemap->vma_tree;
            rb_insert(&pagemap->vma_tree, &r->rb_node, vma_rb_cmp);
            pagemap->vma_cursor = r;
            return best_addr;
        }

    } // namespace VMA
} // namespace VMM