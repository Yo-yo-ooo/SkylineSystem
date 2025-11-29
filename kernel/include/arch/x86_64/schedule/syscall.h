#pragma once

#ifndef _SYSCALL_H
#define _SYSCALL_H

#include <stdint.h>
#include <stddef.h>

extern "C++" {

typedef struct context_t context_t;

typedef struct MapedFileInfo{
    void *fs_desc;
    uint16_t DescSize;
    uint64_t BufferBaseAddr;
    //uint64_t PID;
    uint64_t length;
    uint64_t fd;
    uint64_t RealVMFlag;
    uint64_t FakeVMFlag;
} MapedFileInfo;

typedef struct MapedFileLists{
    MapedFileInfo *Info;
    uint64_t MaxCount;
    uint64_t UsedCount;
    uint64_t NextInfoCount;
}MapedFileLists;

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

#define IA32_EFER  0xC0000080
#define IA32_STAR  0xC0000081
#define IA32_LSTAR 0xC0000082
#define IA32_CSTAR 0xC0000083

void syscall_init();

typedef uint64_t (syscall_function)(uint64_t,uint16_t,uint64_t,uint64_t,uint64_t,uint64_t);

uint64_t sys_read(uint64_t fd_idx, uint64_t buf, uint64_t count, \
    uint64_t ign_0,uint64_t ign_1,uint64_t ign_2);
uint64_t sys_write(uint64_t fd_idx, uint64_t buf, uint64_t count, \
    uint64_t ign_0,uint64_t ign_1,uint64_t ign_2);
int64_t sys_lseek(uint64_t fd_idx, uint64_t offset, uint64_t whence, \
    uint64_t ign_0,uint64_t ign_1,uint64_t ign_2);
uint64_t sys_open(uint64_t path, uint64_t flags, uint64_t mode, \
    uint64_t ign_0,uint64_t ign_1,uint64_t ign_2);
uint64_t sys_execve(uint64_t u_pathname, uint64_t u_argv, uint64_t u_envp, \
    uint64_t ign_0,uint64_t ign_1,uint64_t ign_2);
uint64_t sys_getpid(uint64_t ign_0, uint64_t ign_1, uint64_t ign_2, \
    uint64_t ign_3,uint64_t ign_4,uint64_t ign_5);
uint64_t sys_dup(uint64_t filedesc,uint64_t ign_0, uint64_t ign_1, \
    uint64_t ign_2,uint64_t ign_3,uint64_t ign_4);
uint64_t sys_dup2(uint64_t filedesc,uint64_t filedesc2, uint64_t ign_0, \
    uint64_t ign_1,uint64_t ign_2,uint64_t ign_3);
uint64_t sys_mmap(uint64_t addr_,uint64_t length, uint64_t prot, \
    uint64_t flags, uint64_t fd,uint64_t offset);
uint64_t sys_munmap(uint64_t addr, uint64_t length, \
    uint64_t ign_0,uint64_t ign_1,uint64_t ign_2,uint64_t ign_3);
uint64_t sys_brk(uint64_t addr, \
    uint64_t ign_0,uint64_t ign_1,uint64_t ign_2,uint64_t ign_3,uint64_t ign_4);
uint64_t sys_exit(uint64_t code,uint64_t ign_0, uint64_t ign_1, \
    uint64_t ign_2,uint64_t ign_3,uint64_t ign_4);
uint64_t sys_time(uint64_t tloc,uint64_t ign_0, uint64_t ign_1, \
    uint64_t ign_2,uint64_t ign_3,uint64_t ign_4);
uint64_t sched_yield(uint64_t ign_0, uint64_t ign_1, \
    uint64_t ign_2,uint64_t ign_3,uint64_t ign_4,uint64_t ign_5);
uint64_t sys_arch_prctl(uint64_t op, uint64_t extra,uint64_t ign_0, uint64_t ign_1, \
    uint64_t ign_2,uint64_t ign_3);
}
#endif