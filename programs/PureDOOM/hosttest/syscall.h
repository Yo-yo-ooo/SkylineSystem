/* Host-side stand-in for Skyline's <syscall.h>.
   Redirects the shim's syscalls to plain host implementations and provides
   fsize() (a Skyline libc extension the shim relies on). */
#pragma once
#include <stdint.h>
#include <stdio.h>

long     fsize(FILE* stream);
uint64_t sys_getpid(void);
uint64_t sys_yield(void);
void     sys_exit(uint64_t status);
uint64_t host_syscall(uint64_t n, uint64_t a1, uint64_t a2, uint64_t a3,
                      uint64_t a4, uint64_t a5, uint64_t a6);

#define syscall(num, a1, a2, a3, a4, a5, a6) \
    host_syscall((uint64_t)(num), (uint64_t)(a1), (uint64_t)(a2), \
                 (uint64_t)(a3), (uint64_t)(a4), (uint64_t)(a5), (uint64_t)(a6))
