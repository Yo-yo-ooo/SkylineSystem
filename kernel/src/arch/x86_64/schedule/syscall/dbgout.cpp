//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
#include <arch/x86_64/schedule/sched.h>
#include <arch/x86_64/schedule/syscall.h>
#include <klib/errno.h>
#include <elf/elf.h>
#include <arch/x86_64/cpu.h>
#include <mem/pmm.h>
#include <arch/x86_64/cpu/smap.h>

spinlock_t dbgout_lock = 0;

uint64_t sys_dbgsout(uint64_t CharsAddr,uint64_t OutSize,GENERATE_IGN4()){
    IGNV_4();
    if(!is_user_range(CharsAddr,OutSize)) {
        /* 拒绝要说明白: 用户态以为自己在打印, 内核却静默丢弃 —— 排查
           "日志里少了那行"时这是关键证据 */
        kwarnln("[dbgsout] rejected addr=%p size=%llu (outside user range)",
                (void*)CharsAddr, (unsigned long long)OutSize);
        return -EFAULT;
    }
    spinlock_lock(&dbgout_lock);
    kinfo("App Serial Output: ");
    char *buf = (char*)CharsAddr;
    SmapGuard ug;   // buf is a user string, read under SMAP override
    for(size_t i = 0;i < OutSize;i++){
        Serial::_Write(*(buf + i));
    }
    kprintf("\n");
    spinlock_unlock(&dbgout_lock);
    return OutSize;
}