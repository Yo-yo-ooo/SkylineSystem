//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
#include <arch/x86_64/schedule/sched.h>
#include <arch/x86_64/schedule/syscall.h>
#include <klib/errno.h>
#include <elf/elf.h>
#include <arch/x86_64/cpu.h>
#include <mem/pmm.h>

spinlock_t dbgout_lock = 0;

uint64_t sys_dbgsout(uint64_t CharsAddr,uint64_t OutSize,GENERATE_IGN4()){
    IGNV_4();
    if(!is_user_range(CharsAddr,OutSize))
        return -EFAULT;
    spinlock_lock(&dbgout_lock);
    kinfo("App Serial Output: ");
    char *buf = (char*)CharsAddr;
    for(size_t i = 0;i < OutSize;i++){
        Serial::_Write(*(buf + i));
    }
    kprintf("\n");
    spinlock_unlock(&dbgout_lock);
    return OutSize;
}