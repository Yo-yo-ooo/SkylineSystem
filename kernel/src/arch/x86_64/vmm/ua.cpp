/*
* SPDX-License-Identifier: GPL-2.0-only
* File: vmm.cpp
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
#include <stddef.h>
#include <arch/x86_64/allin.h>
#include <conf.h>
#include <arch/x86_64/vmm/vmm.h>

extern volatile bool IsPM5LVL;

namespace VMM {
    namespace UserAccess {
        
        /**
         * @brief 将内核数据安全地拷贝到当前用户进程的虚拟地址空间
         * * @param pagemap  当前进程的页表指针
         * @param u_dest   用户态目标虚拟地址
         * @param k_src    内核态源数据地址
         * @param len      拷贝长度
         */
        void CopyToUser(pagemap_t* pagemap, uint64_t u_dest, const void* k_src, uint64_t len) {
            size_t offset = 0;
            const uint8_t* src_ptr = (const uint8_t*)k_src;

            while (offset < len) {
                uint64_t curr_u_vaddr = u_dest + offset;
                size_t page_offset = curr_u_vaddr & (PAGE_SIZE - 1);
                size_t remaining_in_page = PAGE_SIZE - page_offset;
                size_t to_copy = (len - offset < remaining_in_page) ? (len - offset) : remaining_in_page;

                uint64_t paddr = VMM::GetPhysics(pagemap, curr_u_vaddr);
                if (paddr == 0 || paddr == (uint64_t)-1) {
                    return; 
                }

                // Properly integrated the HIGHER_HALF macro rather than a loose offset variable
                void* hhdm_dest = (void*)((uint8_t*)HIGHER_HALF((void*)paddr) + page_offset);
                
                __memcpy(hhdm_dest, src_ptr + offset, to_copy);
                offset += to_copy;
            }
        }

        /**
         * @brief 从用户进程的虚拟地址空间安全地拷贝数据到内核
         * * @param pagemap  当前进程的页表指针
         * @param k_dest   内核态目标地址
         * @param u_src    用户态源虚拟地址
         * @param len      拷贝长度
         * @return true    拷贝成功
         * @return false   发生错误（地址非法、未映射或越界）
         */
        bool CopyFromUser(pagemap_t* pagemap, void* k_dest, const void* u_src, uint64_t len) {
            if (len == 0) return true;
            if (!k_dest || !u_src) return false;

            uint64_t start_u_vaddr = (uint64_t)u_src;

            // Dynamically check the top of the user boundary depending on 4-level or 5-level configuration
            uint64_t user_space_end = IsPM5LVL ? USER_SPACE_END_5LVL : USER_SPACE_END_4LVL;

            if (start_u_vaddr > user_space_end || (user_space_end - start_u_vaddr < len)) {
                return false; 
            }

            size_t offset = 0;
            uint8_t* dest_ptr = (uint8_t*)k_dest;

            while (offset < len) {
                uint64_t curr_u_vaddr = start_u_vaddr + offset;
                
                size_t page_offset = curr_u_vaddr & (PAGE_SIZE - 1);
                size_t remaining_in_page = PAGE_SIZE - page_offset;
                size_t to_copy = (len - offset < remaining_in_page) ? (len - offset) : remaining_in_page;

                uint64_t paddr = VMM::GetPhysics(pagemap, curr_u_vaddr);
                
                if (paddr == 0 || paddr == (uint64_t)-1) {
                    return false; 
                }

                void* hhdm_src = (void*)((uint8_t*)HIGHER_HALF((void*)paddr) + page_offset);
                
                __memcpy(dest_ptr + offset, hhdm_src, to_copy);
                offset += to_copy;
            }
            return true;
        }

    } // namespace UserAccess
} // namespace VMM