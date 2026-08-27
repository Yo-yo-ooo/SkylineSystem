//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT

#pragma once

#ifndef _X86_64_SYSCALL_H_
#define _X86_64_SYSCALL_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#define VMM_FLAG_PRESENT        (1 << 0)        /* P   */
#define VMM_FLAG_READWRITE      (1 << 1)        /* R/W */
#define VMM_FLAG_USER           (1 << 2)        /* U/S */
#define VMM_FLAG_WRITETHROUGH   (1 << 3)        /* PWT */
#define VMM_FLAG_CACHE_DISABLE  (1 << 4)        /* PCD */
#define VMM_FLAG_PAT            (1 << 7)        /* PAT */


#define syscall(num, a1, a2, a3, a4, a5, a6) ({ \
    long _ret; \
    register long _rax asm("rax") = (long)(num); \
    register long _rdi asm("rdi") = (long)(a1);  \
    register long _rsi asm("rsi") = (long)(a2);  \
    register long _rdx asm("rdx") = (long)(a3);  \
    register long _r10 asm("r10") = (long)(a4); \
    register long _r8  asm("r8")  = (long)(a5);  \
    register long _r9  asm("r9")  = (long)(a6);  \
    asm volatile("syscall" \
        : "+r"(_rax) \
        : "r"(_rdi), "r"(_rsi), "r"(_rdx), \
          "r"(_r10), "r"(_r8), "r"(_r9) \
        : "rcx", "r11", "memory" \
    ); \
    _ret = _rax; \
    _ret; \
})

uint64_t sys_mmap(uint64_t addr,uint64_t length, uint64_t mode,uint64_t flags,uint64_t offset);
uint64_t sys_munmap(uint64_t addr,uint64_t length);
uint64_t sys_fread(int32_t fd_idx, uint64_t buf, uint64_t count);
uint64_t sys_fwrite(int32_t fd_idx, uint64_t buf, uint64_t count);
uint64_t sys_flseek(int32_t fd_idx, uint64_t offset, uint64_t whence);
uint64_t sys_fopen(uint64_t path, uint64_t flags);
uint64_t sys_fclose(int32_t fd);
uint64_t sys_fsize(int32_t fd);
void sys_exit(uint64_t status);
uint64_t sys_pmmapSHARE(
    uint64_t dst_pid, uint64_t dst_addr, uint64_t length,
    uint64_t flags,   uint64_t src_pid, uint64_t src_addr
);
uint64_t sched_yield();
uint64_t sys_load(uint64_t pathname, uint64_t argv, uint64_t envp);
uint64_t sys_launch(uint64_t pid);
uint64_t sys_getpid();
uint64_t sys_gettid();
uint64_t sys_getsysinfo(uint64_t buf, uint64_t buflen);
uint64_t sys_thread_launch(uint64_t entry, uint64_t hint);

#ifdef __cplusplus
}
#endif

#endif