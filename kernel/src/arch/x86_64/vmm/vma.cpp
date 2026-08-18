// SPDX-FileCopyrightText: 2026 Yo-yo-ooo
// SPDX-License-Identifier: GPL-2.0-only
#include <limine.h>
#include <conf.h>
#include <arch/x86_64/vmm/vmm.h>
#include <mem/pmm.h>
#include <klib/algorithm/rbtree.h>
#include <klib/klib.h>

extern volatile bool IsPM5LVL;

namespace VMM{
    namespace VMA {

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

        void SetStart(pagemap_t *pagemap, uint64_t start, uint64_t page_count) {
            vma_region_t *sentinel = HIGHER_HALF((vma_region_t*)PMM::Request());
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
            vma_region_t *region = HIGHER_HALF((vma_region_t*)PMM::Request());
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
            vma_region_t *region = InsertRegion(pagemap->vma_head->prev, start, page_count, flags);
            region->rb_root = &pagemap->vma_tree;
            rb_insert(&pagemap->vma_tree, &region->rb_node, vma_rb_cmp);
            return region;
        }

        void RemoveRegion(vma_region_t *region) {
            if (region->rb_root) {
                rb_erase(region->rb_root, &region->rb_node);
                region->rb_root = nullptr;
            }
            region->next->prev = region->prev;
            region->prev->next = region->next;
            PMM::Free(PHYSICAL((void*)region));
        }

        vma_region_t *FindRegion(pagemap_t *pagemap, uint64_t addr) {
            uint64_t page_addr = ALIGN_DOWN(addr, PAGE_SIZE);
            vma_region_t *best = vma_tree_find_le(&pagemap->vma_tree, page_addr);
            if (!best) return nullptr;
            uint64_t end = best->start + best->page_count * PAGE_SIZE;
            if (page_addr >= best->start && page_addr < end)
                return best;
            return nullptr;
        }

        uint64_t InternalAlloc(pagemap_t *pagemap, uint64_t page_count, uint64_t flags, uint64_t hint) {
            const uint64_t need = page_count * PAGE_SIZE;
            const uint64_t lo   = pagemap->vma_head->start;
            const uint64_t hi   = is_user_address(0) ? 0xFFFF800000000000ULL : USER_SPACE_END_5LVL;

            if (hint >= lo && hint + need <= hi) {
                vma_region_t *prev_r = vma_tree_find_le(&pagemap->vma_tree, hint);
                vma_region_t *next_r = vma_tree_find_gt(&pagemap->vma_tree, hint);

                uint64_t prev_end  = prev_r ? (prev_r->start + prev_r->page_count * PAGE_SIZE) : lo;
                uint64_t cur_start = next_r ? next_r->start : hi;

                if (hint >= prev_end && hint + need <= cur_start) {
                    vma_region_t *after = prev_r ? prev_r : pagemap->vma_head;
                    vma_region_t *r = InsertRegion(after, hint, page_count, flags);
                    r->rb_root = &pagemap->vma_tree;
                    rb_insert(&pagemap->vma_tree, &r->rb_node, vma_rb_cmp);
                    pagemap->vma_cursor = r;
                    return hint;
                }
            }

            vma_region_t *best_after = nullptr;
            uint64_t      best_addr  = 0;
            uint64_t      best_gap   = UINT64_MAX;

            rb_node_t *prev = nullptr;
            rb_node_t *cur  = rb_first(pagemap->vma_tree.node);

            while (cur) {
                vma_region_t *cur_r  = container_of(cur,  vma_region_t, rb_node);
                vma_region_t *prev_r = prev ? container_of(prev, vma_region_t, rb_node) : nullptr;

                uint64_t prev_end  = prev_r ? (prev_r->start + prev_r->page_count * PAGE_SIZE) : lo;
                uint64_t cur_start = cur_r->start;
                uint64_t gap       = cur_start - prev_end;

                if (gap >= need && gap < best_gap) {
                    best_after = prev_r;
                    best_addr  = prev_end;
                    best_gap   = gap;
                }

                prev = cur;
                cur  = rb_next(cur);
            }

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
                if (hi - lo >= need) {
                    best_addr = lo;
                    best_gap  = hi - lo;
                }
            }

            if (best_gap == UINT64_MAX)
                return 0;

            vma_region_t *after = best_after ? best_after : pagemap->vma_head;
            vma_region_t *r = InsertRegion(after, best_addr, page_count, flags);
            r->rb_root = &pagemap->vma_tree;
            rb_insert(&pagemap->vma_tree, &r->rb_node, vma_rb_cmp);
            pagemap->vma_cursor = r;
            return best_addr;
        }

    } // namespace VMA
} // namespace VMM