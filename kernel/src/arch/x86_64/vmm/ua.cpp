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

namespace VMM {
    namespace UserAccess {
        
        /**
         * @brief 将内核数据安全地拷贝到当前用户进程的虚拟地址空间
         * 
         * @param pagemap  当前进程的页表指针
         * @param u_dest   用户态目标虚拟地址
         * @param k_src    内核态源数据地址
         * @param len      拷贝长度
         * @return true    拷贝成功
         * @return false   发生错误（地址非法、未映射或越界）
         */
        void CopyToUser(pagemap_t* pagemap, uint64_t u_dest, const void* k_src, uint64_t len) {
            size_t offset = 0;
            const uint8_t* src_ptr = (const uint8_t*)k_src;

            while (offset < len) {
                uint64_t curr_u_vaddr = u_dest + offset;
                size_t page_offset = curr_u_vaddr % PAGE_SIZE;
                size_t remaining_in_page = PAGE_SIZE - page_offset;
                size_t to_copy = (len - offset < remaining_in_page) ? (len - offset) : remaining_in_page;

                // 1. 获取物理地址并校验
                uint64_t paddr = VMM::GetPhysics(pagemap, curr_u_vaddr);
                if (paddr == 0) {
                    // 此时 u_dest 对应的虚拟地址未映射，应标记为错误或直接 Panic
                    // 在实际系统中，这里应该抛出异常或通过返回值返回 false
                    return; 
                }

                // 2. 转换到 HHDM
                void* hhdm_dest = (void*)((paddr & ~0xFFFULL) + hhdm_offset + page_offset);
                
                // 3. 拷贝
                __memcpy(hhdm_dest, src_ptr + offset, to_copy);

                offset += to_copy;
            }
        }

        bool CopyFromUser(pagemap_t* pagemap, void* k_dest, const void* u_src, uint64_t len) {
            size_t offset = 0;
            const uint8_t* u_src_ptr = (const uint8_t*)u_src;

            while (offset < len) {
                uint64_t curr_u_vaddr = (uint64_t)u_src_ptr + offset;
                size_t page_offset = curr_u_vaddr % PAGE_SIZE;
                size_t to_copy = min(len - offset, PAGE_SIZE - page_offset);

                // 核心：获取用户态物理页并转换
                uint64_t paddr = VMM::GetPhysics(pagemap, curr_u_vaddr);
                if (paddr == 0) return false; // 地址未映射

                void* hhdm_src = (void*)((paddr & ~0xFFFULL) + hhdm_offset + page_offset);
                __memcpy((uint8_t*)k_dest + offset, hhdm_src, to_copy);

                offset += to_copy;
            }
            return true;
        }

    } // namespace UserAccess
} // namespace VMM