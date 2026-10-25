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
    /* 每行只打一次前缀。
       原来**每次调用**都打 "App Serial Output: " + 一个 '\n' —— 而应用
       (尤其是 DOOM) 常常把一行拆成很多次 1~2 字符的写, 于是 1 字节负载要
       放大成 22 字节串口流量。串口是**同步逐字节**写的 (115200 波特率约
       11.5KB/s), 几千次小写就白白消耗好几秒 —— 实测 DOOM 启动名的很大
       一块时间花在这里, 而且全程握着 dbgout_lock, 其它程序打印会被堵住。
       现在: 行首补一次前缀, 其余原样透传, 不再额外补换行。 */
    spinlock_lock(&dbgout_lock);
    char *buf = (char*)CharsAddr;
    SmapGuard ug;   // buf is a user string, read under SMAP override
    static bool at_line_start = true;
    for(size_t i = 0;i < OutSize;i++){
        char c = *(buf + i);
        if (at_line_start) {
            kprintf("App Serial Output: ");
            at_line_start = false;
        }
        Serial::_Write(c);
        if (c == '\n') at_line_start = true;
    }
    spinlock_unlock(&dbgout_lock);
    return OutSize;
}