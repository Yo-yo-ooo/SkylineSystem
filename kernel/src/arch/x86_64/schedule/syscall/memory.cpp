/*
* SPDX-License-Identifier: GPL-2.0-only
* File: memory.cpp
* Copyright (C) 2026 Yo-yo-ooo
*
* This file is part of SkylineSystem.
*
* SkylineSystem is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
*/
#include <arch/x86_64/schedule/sched.h>
#include <arch/x86_64/schedule/syscall.h>
#include <klib/errno.h>
#include <elf/elf.h>
#include <mem/pmm.h>
#include <klib/algorithm/queue.h>


#define SYS_MAP_FAILED ((uint64_t)-1ULL)

extern volatile spinlock_t pmm_lock;
extern "C" void mmu_invlpg(uint64_t vaddr);

uint64_t sys_mmap_(void *addr, uint64_t length, uint64_t mode, uint64_t flags, uint64_t offset) {
    if (__builtin_expect(length == 0, 0)) {
        return SYS_MAP_FAILED;
    }

    thread_t *current_thread = Schedule::this_thread();
    if (__builtin_expect(!current_thread || !current_thread->pagemap, 0)) {
        return SYS_MAP_FAILED;
    }

    pagemap_t *pagemap = current_thread->pagemap;
    uint64_t page_count = 0;

    // --- 1. 处理长度单位 ---
    // 根据上下文推测，第二个 mode 应该是 3
    if (mode == 2) {
        page_count = length; // length 代表请求的页数
    } else if (mode == 3) {
        page_count = DIV_ROUND_UP(length, PAGE_SIZE); // length 代表字节数，需按页对齐
    } else {
        return SYS_MAP_FAILED; // 不支持的 mode 
    }

    if (__builtin_expect(page_count == 0, 0)) {
        return SYS_MAP_FAILED;
    }

    // --- 2. 准备分配属性 ---
    // sys_mmap_ 作为提供给用户态的接口，强制加上 MM_USER 权限
    uint64_t pt_flags = MM_READ | MM_WRITE | MM_USER;

    // --- 3. 申请虚拟地址空间 ---
    spinlock_lock(&pagemap->vma_lock);
    uint64_t vaddr = VMM::Useless::InternalAlloc(pagemap, page_count, pt_flags);
    if (__builtin_expect(!vaddr, 0)) {
        spinlock_unlock(&pagemap->vma_lock);
        return SYS_MAP_FAILED;
    }

    // --- 4. 申请物理内存并建立页表映射 ---
    Interrupt::Mask(); // 关闭中断，保护 CPU 局部缓存 (PCP)
    cpu_t* cpu = this_cpu();
    uint64_t i = 0;

    // 4.1 尝试使用 2MB 大页 (Huge Pages) 进行批量映射
    while ((page_count - i) >= 512) {
        spinlock_lock(&pmm_lock);
        void* phys_ptr = PMM::RequestHuge();
        spinlock_unlock(&pmm_lock);

        if (__builtin_expect(!!phys_ptr, 1)) {
            VMM::Map(pagemap, vaddr + (i * PAGE_SIZE), (uint64_t)phys_ptr, pt_flags | MM_LARGE_2MB);
            i += 512;
        } else {
            break; // 大物理页耗尽，跳出循环，降级走 4K 小页分配
        }
    }

    // 4.2 剩余部分使用 4KB 小页进行映射
    for (; i < page_count; i++) {
        void* phys_ptr = nullptr;

        // 优先从当前 CPU 的本地缓存池获取物理页
        if (__builtin_expect(!!(cpu && cpu->pmm_cache_count > 0), 1)) {
            phys_ptr = cpu->pmm_cache[--cpu->pmm_cache_count];
        } else {
            // 本地缓存为空，向全局 PMM 申请并补充缓存
            spinlock_lock(&pmm_lock);
            phys_ptr = PMM::GlobalRequestSingle();
            
            if (__builtin_expect(!!(phys_ptr && cpu), 1)) {
                while (cpu->pmm_cache_count < PMM_PCP_BATCH) {
                    void* batch_page = PMM::GlobalRequestSingle();
                    if (!batch_page) break;
                    cpu->pmm_cache[cpu->pmm_cache_count++] = batch_page;
                }
            }
            spinlock_unlock(&pmm_lock);
        }

        // 4.3 物理内存耗尽 (OOM) 处理与安全回滚
        if (__builtin_expect(!phys_ptr, 0)) {
            Interrupt::Unmask();
            kerrorln("sys_mmap_: OOM at page %d (VA: 0x%lx)!", i, vaddr);
            
            // 回滚：释放已映射的物理页和页表条目
            uint64_t j = 0;
            while (j < i) {
                uint64_t current_va = vaddr + j * PAGE_SIZE;
                uint64_t cur_pt_flags = VMM::Useless::GetPhysicsFlags(pagemap, current_va);
                uint64_t pa = PTE_MASK(cur_pt_flags);
                
                if (cur_pt_flags & MM_LARGE_2MB) {
                    if (pa) PMM::FreeHuge((void*)pa);
                    VMM::Unmap(pagemap, current_va);
                    j += 512;
                } else {
                    if (pa) PMM::Free((void*)pa);
                    VMM::Unmap(pagemap, current_va);
                    j++;
                }
                mmu_invlpg(current_va); // 刷新 TLB
            }
            
            // 释放虚拟地址区间
            VMM::Useless::InternalFree(pagemap, vaddr, page_count);
            spinlock_unlock(&pagemap->vma_lock);
            return SYS_MAP_FAILED;
        }
        
        // 建立 4KB 映射
        VMM::Map(pagemap, vaddr + (i * PAGE_SIZE), (uint64_t)phys_ptr, pt_flags);
    }
    
    Interrupt::Unmask(); // 恢复中断
    
    // --- 5. 注册 VMA 映射记录 ---
    VMM::NewMapping(pagemap, vaddr, page_count, pt_flags);
    spinlock_unlock(&pagemap->vma_lock);

    return vaddr; // 返回映射成功的虚拟首地址
}

uint64_t sys_mmap(uint64_t addr_,uint64_t length, uint64_t mode, \
    uint64_t flags,uint64_t offset){
    return sys_mmap_((void*)addr_,length,mode,flags,offset);
}

extern bool VMM_IsPM5LVL;
uint64_t sys_munmap(uint64_t addr, uint64_t length,
    uint64_t ign_0, uint64_t ign_1, uint64_t ign_2, uint64_t ign_3)
{
    IGNORE_VALUE(ign_0);
    IGNORE_VALUE(ign_1);
    IGNORE_VALUE(ign_2);
    IGNORE_VALUE(ign_3);

    // ========== 1. 基础参数合法性校验 ==========
    if (length == 0) {
        return -22; // EINVAL：长度不能为0
    }

    //const uint64_t PAGE_MASK = PAGE_SIZE - 1;
    uint64_t va_start = addr & ~PAGE_MASK;               // 向下页对齐
    uint64_t va_end   = (addr + length + PAGE_MASK) & ~PAGE_MASK; // 向上页对齐

    // 地址溢出校验
    if (va_end < va_start) {
        return -22;
    }

    // 禁止用户态释放内核高半地址空间
    const uint64_t KERNEL_VA_BASE = VMM_IsPM5LVL ? USER_SPACE_END_5LVL : USER_SPACE_END_4LVL;
    if (va_start >= KERNEL_VA_BASE) {
        return -22;
    }
    if (va_end > KERNEL_VA_BASE) {
        va_end = KERNEL_VA_BASE;
    }

    // 对齐后为空区间
    if (va_start >= va_end) {
        return -22;
    }

    // ========== 2. 获取当前进程页表 ==========
    thread_t *cur_thread = Schedule::this_thread();
    if (!cur_thread || !cur_thread->pagemap) {
        return -1; // 上下文无效
    }
    pagemap_t *pm = cur_thread->pagemap;

    // ========== 3. 持有VMA锁，原子操作地址空间 ==========
    spinlock_lock(&pm->vma_lock);

    if (!pm->vma_head) {
        spinlock_unlock(&pm->vma_lock);
        return -22;
    }

    bool found_mapping = false;
    vma_region_t *region = pm->vma_head->next;

    // 遍历VMA链表（你的VMA按虚拟地址升序排列）
    while (region != pm->vma_head) {
        uint64_t region_end = region->start + region->page_count * PAGE_SIZE;
        vma_region_t *next_region = region->next; // 提前保存next，防止删除后链表断裂

        // 无重叠：当前区域完全在目标区间前面，跳过
        if (region_end <= va_start) {
            region = next_region;
            continue;
        }

        // 无重叠：当前区域完全在目标区间后面，遍历结束（VMA有序，后面地址更大）
        if (region->start >= va_end) {
            break;
        }

        // 存在重叠，开始处理
        found_mapping = true;

        // 情况A：区域起始在目标区间前面 → 拆分，前半段保留，后半段继续处理
        if (region->start < va_start) {
            bool split_ok = VMM::VMA::SplitRegion(region, va_start);
            if (!split_ok) {
                spinlock_unlock(&pm->vma_lock);
                return -12; // ENOMEM：拆分VMA内存不足
            }
            // 拆分后 region 是前半段（保留），跳到后半段继续处理
            region = region->next;
            region_end = region->start + region->page_count * PAGE_SIZE;
            next_region = region->next;
        }

        // 情况B：区域结束在目标区间后面 → 拆分，后半段保留，前半段释放
        if (region_end > va_end) {
            bool split_ok = VMM::VMA::SplitRegion(region, va_end);
            if (!split_ok) {
                spinlock_unlock(&pm->vma_lock);
                return -12;
            }
            // 拆分后 region 就是要释放的前半段
            region_end = va_end;
            next_region = region->next;
        }

        // 情况C：区域完全落在目标区间内 → 整块释放
        // 1. 释放物理页面
        VMM::Useless::FreePhysicalPages(pm, region->start, region_end);

        // 2. 修正Next-Fit缓存指针，防止野指针
        if (pm->vma_head->left == region) {
            pm->vma_head->left = region->prev;
        }

        // 3. 从VMA链表移除并释放结构体
        VMM::VMA::RemoveRegion(region);

        region = next_region;
    }

    spinlock_unlock(&pm->vma_lock);

    // 没有找到任何重叠的映射区域
    if (!found_mapping) {
        return -22;
    }

    return 0;
}

extern "C" void mmu_invlpg(uint64_t vaddr);
uint64_t sys_brk(uint64_t addr, GENERATE_IGN5()) {
    IGNV_5();
    thread_t *t = Schedule::this_thread();
    pagemap_t *pagemap = t->pagemap;

    // 1. 基础检查与 brk(0) 处理
    uint64_t current_brk = (uint64_t)t->heap + t->heap_size;
    if (addr == 0) return current_brk;
    if (addr < (uint64_t)t->heap) return current_brk;

    uint64_t old_page_count = DIV_ROUND_UP(t->heap_size, PAGE_SIZE);
    uint64_t new_size = addr - (uint64_t)t->heap;
    uint64_t new_page_count = DIV_ROUND_UP(new_size, PAGE_SIZE);

    if (new_page_count > old_page_count) {
        // 2. 扩容逻辑
        for (uint64_t i = old_page_count; i < new_page_count; i++) {
            uint64_t vaddr = (uint64_t)t->heap + (i * PAGE_SIZE);
            uint64_t paddr = (uint64_t)PMM::Request(); 
            if (!paddr) return current_brk;

            // 映射并清理内存 
            VMM::Map(pagemap, vaddr, paddr, MM_USER | MM_WRITE);
            _memset(HIGHER_HALF((void*)paddr), 0, PAGE_SIZE);
        }
    } 
    else if (new_page_count < old_page_count) {
        // 3. 缩容逻辑：绕过失效的 VMM::Free，直接操作单页
        for (uint64_t i = new_page_count; i < old_page_count; i++) {
            uint64_t vaddr = (uint64_t)t->heap + (i * PAGE_SIZE);
            uint64_t paddr = VMM::GetPhysics(pagemap, vaddr);
            
            if (paddr) {
                VMM::Unmap(pagemap, vaddr);
                PMM::Free((void*)paddr); // 还给物理内存管理器
                mmu_invlpg(vaddr);
            }
        }
    }

    // 4. 同步 VMA 链表状态
    // 否则 Fork 或退出时会因为 page_count 不匹配导致解映射错误
    bool found = false;
    vma_region_t *region = t->pagemap->vma_head->next;
    for (; region != t->pagemap->vma_head; region = region->next) {
        if (region->start == (uint64_t)t->heap) {
            region->page_count = new_page_count;
            found = true;
            break;
        }
    }
    if (!found) {
        // 错误处理或记录日志
        kwarnln("VMA region for heap not found during brk adjustment.\n"
             "This may lead to issues with fork or exit.");
         // 这里我们选择继续执行，因为 brk 本身已经成功调整了堆的大小，
         //VMA 的问题会在后续操作中暴露出来，我们可以通过日志来追踪这个问题，而不是直接失败。
    }

    t->heap_size = new_size;
    return (uint64_t)t->heap + t->heap_size; // 返回新的断点
}

uint64_t sys_mprotect(uint64_t addr, uint64_t len, uint64_t prot, \
    uint64_t ign_0,uint64_t ign_1,uint64_t ign_2) {
    IGNORE_VALUE(ign_0);IGNORE_VALUE(ign_1);IGNORE_VALUE(ign_2);

    thread_t * tthread = Schedule::this_thread();
    size_t pages = DIV_ROUND_UP(len, PAGE_SIZE);
    uint64_t vm_flags = 0;
    if (prot & 2) 
        vm_flags |= MM_READ;
    if (prot & 4) 
        vm_flags |= MM_WRITE;
    if (!(prot & 1)) 
        vm_flags |= MM_NX;

    spinlock_lock(&tthread->pagemap->vma_lock);
    VMM::MapRange(tthread->pagemap, addr, 0, vm_flags, pages);
    spinlock_unlock(&tthread->pagemap->vma_lock);

    return 0;
}