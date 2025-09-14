#pragma once

#ifndef _SYSCALL_H
#define _SYSCALL_H

#include <stdint.h>
#include <stddef.h>

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

#define IA32_EFER  0xC0000080
#define IA32_STAR  0xC0000081
#define IA32_LSTAR 0xC0000082
#define IA32_CSTAR 0xC0000083

void syscall_init();

uint64_t sys_read(uint32_t fd_idx, void *buf, size_t count);
uint64_t sys_write(uint32_t fd_idx, void *buf, size_t count);
int64_t sys_lseek(uint32_t fd_idx, int64_t offset, int32_t whence);
uint64_t sys_open(const char *path, int flags, unsigned int mode);
uint64_t sys_execve(const char *u_pathname, const char **u_argv, const char **u_envp);

#endif