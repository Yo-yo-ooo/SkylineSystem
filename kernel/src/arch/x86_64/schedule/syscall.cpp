/*
* SPDX-License-Identifier: GPL-2.0-only
* File: syscall.cpp
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
//#include <arch/x86_64/smp/smp.h>
#include <klib/klib.h>
#include <arch/x86_64/schedule/syscall.h>
#include <klib/kio.h>
#include <errno.h>
#include <arch/x86_64/schedule/sched.h>

static uint64_t 
SYSCALL_NULL(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t){return NULL;}

uint64_t (*syscall_lists[512])(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t) = \
    {SYSCALL_NULL};

void dump_REG(syscall_frame_t *frame){
    kinfoln("START DUMP REG");
    kinfoln("RAX:0x%X RBX:0x%X RCX:0x%X RDX:0x%X",frame->rax,frame->rbx,frame->rcx,frame->rdx);
    kinfoln("RSI:0x%X RDI:0x%X RBP:0x%X RSP:0x%X",frame->rsi,frame->rdi,frame->rbp,frame->rsp);
    kinfoln("R8:0x%X R9:0x%X R10:0x%X R11:0x%X",frame->r8,frame->r9,frame->r10,frame->r11);
    kinfoln("END DUMP REG");
}

extern "C" void syscall_handler(syscall_frame_t *frame) {
    // 1. 安全检查：防止用户态传一个 rax = 9999 导致内核空指针解引用
    // 1. Safe check: prevent user from passing 
    // rax = 9999 which may cause null pointer dereference in kernel
    if (frame->rax >= 512 || syscall_lists[frame->rax] == nullptr) {
        frame->rax = -ENOSYS; 
        return;
    }

    // 2. Get function pointer from syscall_lists
    auto func = syscall_lists[frame->rax];

    kinfoln("Attempting to call index %ld at addr 0x%p", frame->rax, func);
    frame->rax = func(frame->rdi, frame->rsi, frame->rdx, frame->r10, frame->r8, frame->r9);

    kinfoln("Syscall returned %ld", frame->rax);
}

#include <klib/algorithm/queue.h>
extern "C" void syscall_entry();

/*
syscall_lists[x] = sys_xxxx //! <---- it means some feature in this syscall doesn't complete!
syscall_lists[x] = sys_xxxx //? <---- it means i don't know how to impl this syscall
*/
void syscall_init() {

    syscall_lists[7] = sys_gettid;
    syscall_lists[8] = sys_getpid;
    syscall_lists[9] = sys_exit;
    syscall_lists[13] = nullptr;
    syscall_lists[14] = sched_yield;
    syscall_lists[18] = sys_arch_prctl;
    syscall_lists[20] = sys_getrandom;
    syscall_lists[23] = sys_fork;
    syscall_lists[24] = sys_dbgsout;
    

    uint64_t efer = rdmsr(IA32_EFER);
    efer |= (1 << 0);
    wrmsr(IA32_EFER, efer);
    uint64_t star = 0;
    star |= ((uint64_t)0x08 << 32);
    star |= ((uint64_t)0x13 << 48);
    wrmsr(IA32_STAR, star);
    wrmsr(IA32_LSTAR, (uint64_t)syscall_entry);
    kinfo("User INIT(syscall_init): enabled.\n");
}