//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
//#include <arch/x86_64/smp/smp.h>
#include <klib/klib.h>
#include <arch/x86_64/schedule/syscall.h>
#include <klib/kio.h>
#include <errno.h>
#include <arch/x86_64/schedule/sched.h>
#include <arch/x86_64/schedule/syscalln.h>

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
    if (frame->rax >= 512 || syscall_lists[frame->rax] == nullptr) {
        frame->rax = -ENOSYS; 
        return;
    }

    auto func = syscall_lists[frame->rax];

    //kinfoln("Attempting to call index %ld at addr 0x%p", frame->rax, func);
    frame->rax = func(frame->rdi, frame->rsi, frame->rdx, frame->r10, frame->r8, frame->r9);

    //kinfoln("Syscall returned %ld", frame->rax);
}

#include <klib/algorithm/queue.h>
extern "C" void syscall_entry();

// RFLAGS 关键标志位
#define RFLAGS_TF   (1 << 8)  // Trap Flag (单步调试)
#define RFLAGS_IF   (1 << 9)  // Interrupt Flag (中断响应)
#define RFLAGS_DF   (1 << 10) // Direction Flag (方向标志)
#define RFLAGS_IOPL (3 << 12) // I/O Privilege Level
#define RFLAGS_NT   (1 << 14) // Nested Task
#define RFLAGS_AC   (1 << 18) // Alignment Check

/*
syscall_lists[x] = sys_xxxx //! <---- it means some feature in this syscall doesn't complete!
syscall_lists[x] = sys_xxxx //? <---- it means i don't know how to impl this syscall
*/
void syscall_init() {
    
    syscall_lists[SYSCALL_FOPEN] = sys_fopen;
    syscall_lists[SYSCALL_FWRITE] = sys_fwrite;
    syscall_lists[SYSCALL_FREAD] = sys_fread;
    syscall_lists[SYSCALL_FCLOSE] = sys_fclose;
    syscall_lists[SYSCALL_FLSEEK] = sys_flseek;
    syscall_lists[SYSCALL_FSIZE] = sys_fsize;
    
    syscall_lists[7] = sys_gettid;
    syscall_lists[8] = sys_getpid;
    syscall_lists[9] = sys_exit;
    syscall_lists[12] = sys_munmap;
    syscall_lists[13] = sys_mmap;
    syscall_lists[14] = sched_yield;
    syscall_lists[18] = sys_arch_prctl;
    syscall_lists[20] = sys_getrandom;
    syscall_lists[21] = sys_dev_mmap;
    syscall_lists[23] = sys_fork;
    syscall_lists[24] = sys_dbgsout;
    syscall_lists[25] = sys_dev_getinfo;
    syscall_lists[26] = sys_dev_ioctl;
    

    uint64_t efer = rdmsr(IA32_EFER);
    efer |= (1 << 0);
    wrmsr(IA32_EFER, efer);
    uint64_t star = 0;
    star |= ((uint64_t)0x08 << 32);
    star |= ((uint64_t)0x13 << 48);
    wrmsr(IA32_STAR, star);
    wrmsr(IA32_LSTAR, (uint64_t)syscall_entry);
    uint64_t fmask = RFLAGS_IF | RFLAGS_DF | RFLAGS_TF | RFLAGS_IOPL | RFLAGS_NT | RFLAGS_AC;
    wrmsr(IA32_FMASK, fmask);
    kinfo("User INIT(syscall_init): enabled.\n");
}