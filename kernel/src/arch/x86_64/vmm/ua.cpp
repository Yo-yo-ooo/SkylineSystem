//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
#include <stddef.h>
#include <arch/x86_64/allin.h>
#include <conf.h>
#include <arch/x86_64/vmm/vmm.h>

extern volatile bool IsPM5LVL;

// CoW 软件标志位（与 Fork/HandlePF 中保持一致）
#define VMM_COW_BIT (1ULL << 55)

namespace VMM {
    namespace UserAccess {

        /**
         * @brief 将内核数据安全地拷贝到当前用户进程的虚拟地址空间
         *
         * - 校验用户态地址范围，防止越界访问内核空间
         * - 自动处理 CoW 页（避免通过 HHDM 绕过用户页表写保护）
         * - 感知 4K/2M/1G 页大小，减少 GetPageInfo 调用次数
         *
         * @param pagemap  当前进程的页表指针
         * @param u_dest   用户态目标虚拟地址
         * @param k_src    内核态源数据地址
         * @param len      拷贝长度（字节）
         * @return true    拷贝成功
         * @return false   地址非法 / 未映射 / 越界 / 权限不足
         */
        bool CopyToUser(pagemap_t* pagemap, uint64_t u_dest, const void* k_src, uint64_t len) {
            if (len == 0) return true;
            if (!pagemap || !k_src) return false;

            // 用户态地址上界（依 4 级 / 5 级分页而定）
            uint64_t user_space_end = IsPM5LVL ? USER_SPACE_END_5LVL : USER_SPACE_END_4LVL;

            // 越界 / 溢出检查：等价于 (u_dest + len - 1) > user_space_end，但不会溢出
            if (u_dest > user_space_end)                       return false;
            if (user_space_end - u_dest + 1 < len)             return false;

            spinlock_lock(&pagemap->vma_lock);

            size_t offset = 0;
            const uint8_t* src_ptr = (const uint8_t*)k_src;

            while (offset < len) {
                uint64_t curr_u_vaddr = u_dest + offset;

                                Useless::PageInfo info = VMM::Useless::GetPageInfo(pagemap, curr_u_vaddr);
                if (info.size == 0) {
                    // 主动按需分页：用户堆内存可能只有虚拟地址没有物理页
                    uint64_t page_start = curr_u_vaddr & ~(PAGE_SIZE - 1);
                    void* new_phys = PMM::Request();
                    if (!new_phys) {
                        spinlock_unlock(&pagemap->vma_lock);
                        return false; // OOM
                    }
                    // 映射到用户进程空间，赋予用户读写权限
                    VMM::Map4K(pagemap, page_start, (uint64_t)new_phys, MM_READ | MM_WRITE | MM_USER);
                    __asm__ volatile ("invlpg (%0)" : : "r"(page_start) : "memory");
                    
                    // 重新获取映射信息，拿到物理地址
                    info = VMM::Useless::GetPageInfo(pagemap, curr_u_vaddr);
                    if (info.size == 0) {
                        spinlock_unlock(&pagemap->vma_lock);
                        return false; // 映射失败
                    }
                }

                bool is_writable = (info.flags & MM_WRITE) != 0;
                bool is_cow      = (info.flags & VMM_COW_BIT) != 0;

                // 既不可写、又不是 CoW ——> 拒绝写入（避免污染只读映射）
                if (!is_writable && !is_cow) {
                    spinlock_unlock(&pagemap->vma_lock);
                    return false;
                }

                                // CoW 处理：必须先复制再写，否则通过 HHDM 直接写会污染共享页
                if (is_cow) {
                    uint64_t new_flags = info.flags;
                    new_flags &= ~VMM_COW_BIT; // 清 CoW 软件位
                    new_flags |= MM_WRITE;     // 恢复可写

                    // 必须对齐到页边界
                    uint64_t page_start = curr_u_vaddr & ~(info.size - 1);

                    if (info.size == PAGE_1GB) {
                        VMM::Unmap(pagemap, page_start);
                        __asm__ volatile ("invlpg (%0)" : : "r"(page_start) : "memory");
                        for (int j = 0; j < 512; j++) {
                            uint64_t v = page_start + j * PAGE_2MB;
                            uint64_t p = info.phys + j * PAGE_2MB;
                            uint64_t new_phys = (uint64_t)PMM::Request2MB();
                            __memcpy(HIGHER_HALF((void*)new_phys), HIGHER_HALF((void*)p), PAGE_2MB);
                            VMM::Map2M(pagemap, v, new_phys, new_flags);
                        }
                        info = VMM::Useless::GetPageInfo(pagemap, curr_u_vaddr); // 重新获取信息
                    } else if (info.size == PAGE_2MB) {
                        uint64_t new_phys = (uint64_t)PMM::Request2MB();
                        __memcpy(HIGHER_HALF((void*)new_phys), HIGHER_HALF((void*)info.phys), PAGE_2MB);
                        VMM::Map2M(pagemap, page_start, new_phys, new_flags);
                        info.phys = new_phys;
                    } else {
                        uint64_t new_phys = (uint64_t)PMM::Request();
                        __memcpy(HIGHER_HALF((void*)new_phys), HIGHER_HALF((void*)info.phys), PAGE_SIZE);
                        VMM::Map4K(pagemap, page_start, new_phys, new_flags);
                        info.phys = new_phys;
                    }

                    __asm__ volatile ("invlpg (%0)" : : "r"(curr_u_vaddr) : "memory");
                }

                // 在当前页（或巨页）内拷贝
                size_t page_offset       = curr_u_vaddr & (info.size - 1);
                size_t remaining_in_page = info.size - page_offset;
                size_t to_copy           = (len - offset < remaining_in_page)
                                           ? (len - offset) : remaining_in_page;

                void* hhdm_dest = (void*)((uint8_t*)HIGHER_HALF((void*)info.phys) + page_offset);
                __memcpy(hhdm_dest, src_ptr + offset, to_copy);

                offset += to_copy;
            }

            spinlock_unlock(&pagemap->vma_lock);
            return true;
        }

        /**
         * @brief 从用户进程的虚拟地址空间安全地拷贝数据到内核
         *
         * - 校验用户态地址范围
         * - 感知 4K/2M/1G 页大小，减少 GetPageInfo 调用次数
         * - 读取操作不会触发 CoW
         *
         * @param pagemap  当前进程的页表指针
         * @param k_dest   内核态目标地址
         * @param u_src    用户态源虚拟地址
         * @param len      拷贝长度（字节）
         * @return true    拷贝成功
         * @return false   地址非法 / 未映射 / 越界
         */
        bool CopyFromUser(pagemap_t* pagemap, void* k_dest, const void* u_src, uint64_t len) {
            if (len == 0) return true;
            if (!pagemap || !k_dest || !u_src) return false;

            uint64_t start_u_vaddr  = (uint64_t)u_src;
            uint64_t user_space_end = IsPM5LVL ? USER_SPACE_END_5LVL : USER_SPACE_END_4LVL;

            if (start_u_vaddr > user_space_end)            return false;
            if (user_space_end - start_u_vaddr + 1 < len)  return false;

            size_t offset   = 0;
            uint8_t* dest_ptr = (uint8_t*)k_dest;

            while (offset < len) {
                uint64_t curr_u_vaddr = start_u_vaddr + offset;

                VMM::Useless::PageInfo info = VMM::Useless::GetPageInfo(pagemap, curr_u_vaddr);
                if (info.size == 0) return false; // 未映射

                size_t page_offset       = curr_u_vaddr & (info.size - 1);
                size_t remaining_in_page = info.size - page_offset;
                size_t to_copy           = (len - offset < remaining_in_page)
                                           ? (len - offset) : remaining_in_page;

                void* hhdm_src = (void*)((uint8_t*)HIGHER_HALF((void*)info.phys) + page_offset);
                __memcpy(dest_ptr + offset, hhdm_src, to_copy);

                offset += to_copy;
            }
            return true;
        }

    } // namespace UserAccess
} // namespace VMM