/*
* SPDX-License-Identifier: GPL-2.0-only
* File: task.cpp
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

uint64_t sys_getpid(uint64_t ign_0, uint64_t ign_1, uint64_t ign_2, \
    uint64_t ign_3,uint64_t ign_4,uint64_t ign_5) {
    IGNORE_VALUE(ign_0);IGNORE_VALUE(ign_1);IGNORE_VALUE(ign_2);
    IGNORE_VALUE(ign_3);IGNORE_VALUE(ign_4);IGNORE_VALUE(ign_5);

    return Schedule::this_proc()->id;
}

uint64_t sys_gettid(uint64_t ign_0, uint64_t ign_1, uint64_t ign_2, \
    uint64_t ign_3,uint64_t ign_4,uint64_t ign_5) {
    IGNORE_VALUE(ign_0);IGNORE_VALUE(ign_1);IGNORE_VALUE(ign_2);
    IGNORE_VALUE(ign_3);IGNORE_VALUE(ign_4);IGNORE_VALUE(ign_5);

    return Schedule::this_thread()->id;
}

uint64_t sys_dup(uint64_t filedesc,uint64_t ign_0, uint64_t ign_1, \
    uint64_t ign_2,uint64_t ign_3,uint64_t ign_4) {
    IGNORE_VALUE(ign_0);IGNORE_VALUE(ign_1);IGNORE_VALUE(ign_2);
    IGNORE_VALUE(ign_3);IGNORE_VALUE(ign_4);

    fd_t *fd = Schedule::this_proc()->fd_table[filedesc];
    Schedule::this_proc()->fd_table[filedesc + 1] = fd;
    Schedule::this_proc()->fd_count++;

    return Schedule::this_proc()->fd_count;
}

uint64_t sys_dup2(uint64_t filedesc,uint64_t filedesc2, uint64_t ign_0, \
    uint64_t ign_1,uint64_t ign_2,uint64_t ign_3) {
    IGNORE_VALUE(ign_0);IGNORE_VALUE(ign_1);
    IGNORE_VALUE(ign_2);IGNORE_VALUE(ign_3);

    if(filedesc == filedesc2)
        return filedesc2;
    Schedule::this_proc()->fd_table[filedesc2] = Schedule::this_proc()->fd_table[filedesc];

    return filedesc;
}

uint64_t sys_exit(uint64_t code,uint64_t ign_0, uint64_t ign_1, \
    uint64_t ign_2,uint64_t ign_3,uint64_t ign_4) {
    IGNORE_VALUE(ign_0);IGNORE_VALUE(ign_1);IGNORE_VALUE(ign_2);
    IGNORE_VALUE(ign_3);IGNORE_VALUE(ign_4);

    Schedule::Exit((int32_t)code);

    return 0;
}

uint64_t sched_yield(uint64_t ign_0, uint64_t ign_1, \
    uint64_t ign_2,uint64_t ign_3,uint64_t ign_4,uint64_t ign_5){
    IGNORE_VALUE(ign_0);IGNORE_VALUE(ign_1);IGNORE_VALUE(ign_2);
    IGNORE_VALUE(ign_3);IGNORE_VALUE(ign_4);IGNORE_VALUE(ign_5);

    Schedule::Yield();
    return 0;
}


uint64_t sys_kill(uint64_t pid,uint64_t sig, uint64_t ign_0, \
    uint64_t ign_1,uint64_t ign_2,uint64_t ign_3) {
    IGNORE_VALUE(ign_0);IGNORE_VALUE(ign_1);IGNORE_VALUE(ign_2);
    IGNORE_VALUE(ign_3);

    //Schedule::Signal::Raise(Schedule::this_proc(), LINUX_SIGKILL);
    for(uint32_t i = 0;i < smp_last_cpu;i++){
        for (uint32_t j = 0; j < THREAD_QUEUE_CNT; j++){
            thread_queue_t *tq = &smp_cpu_list[i]->thread_queues[j];
            thread_t *thread = tq->head;
            while (thread->next != nullptr){
                if(thread->parent->id == pid){
                    Schedule::Signal::Raise(thread->parent, LINUX_SIGKILL);
                    Schedule::DeleteProc(thread->parent);
                    if(thread == this_cpu()->current_thread)
                        this_cpu()->current_thread = nullptr;
                    return 0;
                }
                thread = thread->next;
            }
        }
    }

    return 0;
}
