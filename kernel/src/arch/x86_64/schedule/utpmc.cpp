//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
// This File Is Using To Parase execve->UTPMC Model

#include <elf/elf.h>
#include <drivers/dev/dev.h>

uint64_t UTPMC_PARASE(SysExecveARG *utpmc_args) {
    uint64_t processed_count = 0;

    // 1. 遍历链表，直到 Next 为空指针
    while (utpmc_args != nullptr) {
        // 2. 遍历 3 个 64 位 Bitmap 组成的 192 位图
        for (int word_idx = 0; word_idx < 3; word_idx++) {
            // 假设原逻辑中 ~ 是必须的，意味着 AvailBitmap 中 0 表示可用，取反后 1 表示可用
            // 如果实际上 1 就表示可用，请去掉波浪号 ~ 
            uint64_t current_bitmap = ~utpmc_args->AvailBitmap[word_idx];
            
            // 3. 提取并遍历当前 64 位字中所有为 1 的位
            while (current_bitmap != 0) {
                // 获取最低位连续 0 的个数，即第一个可用位的索引
                uint8_t bit_idx = __builtin_ctzll(current_bitmap);
                
                // 计算在 Reqs 数组中的全局索引
                uint16_t req_idx = word_idx * 64 + bit_idx;
                
                // 防止越界（因为有 3*64=192 位，但 Reqs 只有 127 个）
                if (req_idx < 127) {
                    // 4. 实现解析 CmdResID!
                    uint64_t cmd_res_id = utpmc_args->Reqs[req_idx].CmdResID;
                    
                    // 高 32 位为 Cmd
                    uint32_t cmd = (uint32_t)(cmd_res_id >> 32);
                    // 低 32 位为 ResID
                    uint32_t res_id = (uint32_t)(cmd_res_id & 0xFFFFFFFFULL);
                    uint64_t addr = utpmc_args->Reqs[req_idx].Addr;
                    uint64_t data_addr = utpmc_args->Reqs[req_idx].DATAAddr;
                    uint64_t size = utpmc_args->Reqs[req_idx].Size;
                    // 根据 Cmd 执行相应操作
                    if (cmd == 0) {
                        // Cmd=0: Read-Only MMAP
                        
                        // TODO: 执行 Read-Only MMAP 逻辑
                        Dev::DeviceMemoryMap(res_id,0,size,MM_READ,0,addr);
                    } else if (cmd == 1) {
                        // Cmd=1: Read+Write MMAP
                        // TODO: 执行 Read+Write MMAP 逻辑
                    }
                    
                    processed_count++;
                }
                
                // 5. 清除最低位的 1，以查找下一个可用位
                // 等价于 current_bitmap &= (current_bitmap - 1);
                current_bitmap &= current_bitmap - 1;
            }
        }
        
        // 移动到下一个节点
        utpmc_args = (SysExecveARG *)utpmc_args->Next;
    }

    return processed_count;
}