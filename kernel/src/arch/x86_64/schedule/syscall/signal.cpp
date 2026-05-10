/*
* SPDX-License-Identifier: GPL-2.0-only
* File: signal.cpp
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

uint64_t sys_rt_sigaction(uint64_t signum,uint64_t act,uint64_t oldact,
    GENERATE_IGN3()){
    IGNV_3();
    if (signum < 1 || signum > 64 || 
        signum == LINUX_SIGKILL || 
        signum == LINUX_SIGSTOP) {
        return -EINVAL;
    }
    proc_t *tproc = Schedule::this_proc();
    sigaction_t* action = (sigaction_t*)act;
    sigaction_t* oldaction = (sigaction_t*)oldact;
    if(oldact != NULL){
        __memcpy(oldact,&tproc->sig_handlers[signum],sizeof(sigaction_t));
    }else if (act != NULL) {
        __memcpy(&(tproc->sig_handlers[signum]), action, sizeof(sigaction_t));
    }else{
        return -EFAULT;
    }


    return 0;
}