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

        // 检查 [start, start + page_count * PAGE_SIZE) 是否完全空闲（无任何重叠）
        bool IsRangeFree(pagemap_t *pagemap, uint64_t start, uint64_t page_count) {
            uint64_t end = start + page_count * PAGE_SIZE;
            
            vma_region_t *prev_r = vma_tree_find_le(&pagemap->vma_tree, start);
            if (prev_r && (prev_r->start + prev_r->page_count * PAGE_SIZE) > start) {
                return false; 
            }
            
            vma_region_t *next_r = vma_tree_find_gt(&pagemap->vma_tree, start);
            if (next_r && next_r->start < end) {
                return false; 
            }
            
            return true;
        }

        // 修复：增加 pagemap 参数，安全处理 vma_cursor 缓存失效问题
        void RemoveRegion(pagemap_t *pagemap, vma_region_t *region) {
            if (!pagemap || !region) return;

            // 如果游标正好指向要删除的节点，将游标回退到前一个节点
            if (pagemap->vma_cursor == region) {
                pagemap->vma_cursor = region->prev;
            }

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

            // 1. Hint 优先路径
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

            // 2. Next-Fit 搜索路径
            vma_region_t *best_after = nullptr;
            uint64_t      best_addr  = 0;
            bool          found      = false; // 修复：引入 found 标志位消除地址 0 的误判
            
            rb_node_t *start_node = (pagemap->vma_cursor && pagemap->vma_cursor != pagemap->vma_head) 
                                    ? &pagemap->vma_cursor->rb_node 
                                    : rb_first(pagemap->vma_tree.node);

            if (!start_node) {
                if (hi - lo >= need) {
                    best_addr = lo;
                    found = true;
                }
            } else {
                rb_node_t *cur_node = start_node;
                bool is_first_iteration = true;
                
                while (cur_node && (is_first_iteration || cur_node != start_node)) {
                    is_first_iteration = false;
                    vma_region_t *cur_r = container_of(cur_node, vma_region_t, rb_node);
                    rb_node_t *next_node = rb_next(cur_node);
                    
                    uint64_t prev_end = cur_r->start + cur_r->page_count * PAGE_SIZE;
                    uint64_t cur_start = next_node ? container_of(next_node, vma_region_t, rb_node)->start : hi;
                    
                    if (cur_start - prev_end >= need) {
                        best_after = cur_r;
                        best_addr  = prev_end;
                        found = true;
                        break;
                    }
                    
                    if (next_node) {
                        cur_node = next_node;
                    } else {
                        cur_node = rb_first(pagemap->vma_tree.node);
                        if (cur_node) {
                            vma_region_t *first_r = container_of(cur_node, vma_region_t, rb_node);
                            if (first_r->start - lo >= need) {
                                best_after = nullptr; 
                                best_addr  = lo;
                                found = true;
                                break;
                            }
                        }
                    }
                }
            }

            // 修复：使用 found 判断是否成功找到空洞
            if (!found) {
                return 0; 
            }

            vma_region_t *after = best_after ? best_after : pagemap->vma_head;
            vma_region_t *r = InsertRegion(after, best_addr, page_count, flags);
            r->rb_root = &pagemap->vma_tree;
            rb_insert(&pagemap->vma_tree, &r->rb_node, vma_rb_cmp);
            pagemap->vma_cursor = r; 
            return best_addr;
        }

    } // namespace VMA
} // namespace VMM