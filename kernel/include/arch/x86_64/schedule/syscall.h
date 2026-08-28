//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
#pragma once

#ifndef _SYSCALL_H
#define _SYSCALL_H

#include <stdint.h>
#include <stddef.h>

extern "C++" {

typedef struct context_t context_t;



typedef struct syscall_args{
    void* arg1;
    void* arg2;
    void* arg3;
    void* arg4;
    void* arg5;
    void* arg6;
    context_t* r;
} syscall_args;


typedef struct syscall_frame_t{
    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t r11;
    uint64_t r10;
    uint64_t r9;
    uint64_t r8;
    uint64_t rdi;
    uint64_t rsi;
    uint64_t rbp;
    uint64_t rbx;
    uint64_t rdx;
    uint64_t rcx;
    uint64_t rax;
    uint64_t int_no;
    uint64_t error_code;
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
    uint64_t kernel_stack;
} syscall_frame_t;

uint64_t sys_fork(syscall_frame_t *frame);

typedef int64_t time_t;
typedef int64_t suseconds_t;


#define IA32_EFER  0xC0000080
#define IA32_STAR  0xC0000081
#define IA32_LSTAR 0xC0000082
#define IA32_CSTAR 0xC0000083
#define IA32_XSS   0x00000DA0

void syscall_init();

#define GENERATE_IGN2() uint64_t ign_0,uint64_t ign_1,syscall_frame_t* nullframe
#define GENERATE_IGN3() uint64_t ign_0,uint64_t ign_1,uint64_t ign_2,syscall_frame_t* nullframe
#define GENERATE_IGN4() uint64_t ign_0,uint64_t ign_1,uint64_t ign_2,\
                        uint64_t ign_3,syscall_frame_t* nullframe
#define GENERATE_IGN5() uint64_t ign_0,uint64_t ign_1,uint64_t ign_2,\
                        uint64_t ign_3,uint64_t ign_4,syscall_frame_t* nullframe
#define GENERATE_IGN6() uint64_t ign_0,uint64_t ign_1,uint64_t ign_2,\
                        uint64_t ign_3,uint64_t ign_4,uint64_t ign_5,syscall_frame_t* nullframe
#define IGNV_2() IGNORE_VALUE(ign_0);IGNORE_VALUE(ign_1);IGNORE_VALUE(nullframe);
#define IGNV_3() IGNORE_VALUE(ign_0);IGNORE_VALUE(ign_1);IGNORE_VALUE(ign_2);IGNORE_VALUE(nullframe);
#define IGNV_4() IGNORE_VALUE(ign_0);IGNORE_VALUE(ign_1);IGNORE_VALUE(ign_2);\
                IGNORE_VALUE(ign_3);IGNORE_VALUE(nullframe);
#define IGNV_5() IGNORE_VALUE(ign_0);IGNORE_VALUE(ign_1);IGNORE_VALUE(ign_2);\
                IGNORE_VALUE(ign_3);IGNORE_VALUE(ign_4);IGNORE_VALUE(nullframe);
#define IGNV_6() IGNORE_VALUE(ign_0);IGNORE_VALUE(ign_1);IGNORE_VALUE(ign_2);\
                IGNORE_VALUE(ign_3);IGNORE_VALUE(ign_4);IGNORE_VALUE(ign_5);IGNORE_VALUE(nullframe);

typedef uint64_t (syscall_function)(uint64_t,uint16_t,uint64_t,uint64_t,uint64_t,uint64_t);

uint64_t sys_fread(uint64_t fd_idx, uint64_t buf, uint64_t count, \
    GENERATE_IGN3());
uint64_t sys_fwrite(uint64_t fd_idx, uint64_t buf, uint64_t count, \
    GENERATE_IGN3());
uint64_t sys_flseek(uint64_t fd_idx, uint64_t offset, uint64_t whence, \
    GENERATE_IGN3());
uint64_t sys_fopen(uint64_t path, uint64_t flags, \
    GENERATE_IGN4());
uint64_t sys_load(uint64_t u_pathname, uint64_t u_argv, uint64_t u_envp, \
    GENERATE_IGN3());
uint64_t sys_launch(uint64_t pid,GENERATE_IGN5());
uint64_t sys_getpid(GENERATE_IGN6());
uint64_t sys_mmap(uint64_t addr_,uint64_t length, uint64_t mode, \
    uint64_t flags,uint64_t offset,uint64_t ign_0,syscall_frame_t* frame);
uint64_t sys_munmap(uint64_t addr, uint64_t length, \
    GENERATE_IGN4());
uint64_t sys_brk(uint64_t addr, \
    GENERATE_IGN5());
uint64_t sys_exit(uint64_t code,GENERATE_IGN5());
uint64_t sys_time(uint64_t tloc,GENERATE_IGN5());
uint64_t sched_yield(GENERATE_IGN6());
uint64_t sys_arch_prctl(uint64_t op, uint64_t extra,GENERATE_IGN4());
uint64_t sys_gettid(GENERATE_IGN6());
uint64_t sys_getrandom(uint64_t buf, uint64_t size, uint64_t flags,
    GENERATE_IGN3());
uint64_t sys_fclose(uint64_t fd,GENERATE_IGN5());
uint64_t sys_mkdir(uint64_t path,uint64_t mode,GENERATE_IGN4());
uint64_t sys_clock_gettime(uint64_t clkid,uint64_t tp,GENERATE_IGN4());
uint64_t sys_pmmapSHARE(
    uint64_t dst_pid,      // 目标进程（要改谁的页表）
    uint64_t dst_addr,     // 目标进程的虚拟地址
    uint64_t length,       // 映射长度（字节）
    uint64_t flags,        // 合并 PROT_* 和 MAP_*（见下文）
    uint64_t src_pid,      // 源进程（从谁那里拿物理页）
    uint64_t src_addr,      // 源进程的虚拟地址（从哪拿）
    syscall_frame_t* frame
);
uint64_t sys_thread_launch(uint64_t entry, uint64_t hint, GENERATE_IGN4());
uint64_t sys_sysinfo(GENERATE_IGN6());
uint64_t sys_dev_mmap(uint64_t DevType,uint64_t DevIDX,
uint64_t length,uint64_t prot,uint64_t offset,uint64_t VADDR,syscall_frame_t *nullframe);
uint64_t sys_dev_getinfo(
    uint64_t DevType,uint64_t DevIDX,uint64_t UserDesc,
    GENERATE_IGN3());
uint64_t sys_dev_ioctl(
    uint64_t DevType,uint64_t DevIDX,uint64_t cmd,uint64_t arg,
        GENERATE_IGN2());
uint64_t sys_dbgsout(uint64_t CharsAddr,uint64_t OutSize,GENERATE_IGN4());
uint64_t sys_fsize(uint64_t fd_idx,GENERATE_IGN5());
}
#endif