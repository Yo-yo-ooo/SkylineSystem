#include <limine.h>
#include <stddef.h>
#include <arch/x86_64/allin.h>
#include <conf.h>
#include <arch/x86_64/vmm/vmm.h>

namespace VMM{
    
    namespace VMA
    {
        bool SplitRegion(vma_region_t *origin, uint64_t split_addr) {
            if (!origin) return false;
            uint64_t origin_end = origin->start + origin->page_count * PAGE_SIZE;

            // 拆分地址必须在区域内部，且页对齐
            if (split_addr <= origin->start || split_addr >= origin_end) return false;
            if ((split_addr & (PAGE_SIZE - 1)) != 0) return false;

            uint64_t front_pages = (split_addr - origin->start) / PAGE_SIZE;
            uint64_t back_pages  = origin_end / PAGE_SIZE - split_addr / PAGE_SIZE;

            // 分配新VMA结构体（和现有逻辑一致：物理页+内核高半映射）
            vma_region_t *back_region = (vma_region_t*)HIGHER_HALF(PMM::Request());
            if (!back_region) return false;
            _memset(back_region, 0, sizeof(vma_region_t));

            back_region->start = split_addr;
            back_region->page_count = back_pages;
            back_region->flags = origin->flags;

            // 插入双向链表，保持地址升序
            back_region->prev = origin;
            back_region->next = origin->next;
            origin->next->prev = back_region;
            origin->next = back_region;

            // 修改原区域大小为前半段
            origin->page_count = front_pages;
            return true;
        }
        void SetStart(pagemap_t *pagemap, uint64_t start, uint64_t page_count){
            vma_region_t *region = (vma_region_t*)HIGHER_HALF(PMM::Request());
            region->start = start;
            region->page_count = page_count;
            region->flags = MM_READ | MM_WRITE;
            region->next = region;
            region->prev = region;
            region->left = nullptr;          // [新增] Next-Fit 缓存初始为空
            pagemap->vma_head = region;
        }
        vma_region_t *AddRegion(pagemap_t *pagemap, uint64_t start, uint64_t page_count, uint64_t flags){
            vma_region_t *region = (vma_region_t*)HIGHER_HALF(PMM::Request());
            region->start = start;
            region->page_count = page_count;
            region->flags = flags;
            region->prev = pagemap->vma_head->prev;
            region->next = pagemap->vma_head;
            pagemap->vma_head->prev->next = region;
            pagemap->vma_head->prev = region;
            return region;
        }
        vma_region_t *InsertRegion(vma_region_t *after, uint64_t start, uint64_t page_count, uint64_t flags){
            vma_region_t *region = (vma_region_t*)HIGHER_HALF(PMM::Request());
            region->start = start;
            region->page_count = page_count;
            region->flags = flags;
            region->prev = after;
            region->next = after->next;
            after->next->prev = region;
            after->next = region;
            return region;
        }

        void RemoveRegion(vma_region_t *region) {
            region->next->prev = region->prev;
            region->prev->next = region->next;
            PMM::Free(PHYSICAL(region));
        }
    } // namespace VMA
}