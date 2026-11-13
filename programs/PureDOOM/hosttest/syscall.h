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
uint64_t sys_sysinfo(uint64_t ARG);

/* SysInfo: the shim reads mem_total/mem_free for its zone-size policy and the
   compact-build memory markers. Layout mirrors the kernel ABI in
   lib/include/base/arch/x86_64/syscall.h (only the fields the shim touches are
   meaningful on the host; the harness fills total/free with a 2 GiB profile). */
typedef struct SysInfo {
    uint64_t occupy_pages;
    uint64_t magic;
    uint32_t abi_version;
    uint32_t kernel_version;
    uint32_t ncpus;
    uint32_t cpu_family;
    uint32_t cpu_features_edx;
    uint32_t cpu_features_ecx;
    char     cpu_brand[48];
    uint64_t mem_total;
    uint64_t mem_free;
    uint64_t mem_used;
    uint64_t uptime_ms;
    uint64_t nprocs;
    uint64_t nthreads_approx;
    uint64_t ctx_switches;
    uint64_t reserved[8];
    uint32_t simd_mask[];
} SysInfo;
uint64_t host_syscall(uint64_t n, uint64_t a1, uint64_t a2, uint64_t a3,
                      uint64_t a4, uint64_t a5, uint64_t a6);

#define syscall(num, a1, a2, a3, a4, a5, a6) \
    host_syscall((uint64_t)(num), (uint64_t)(a1), (uint64_t)(a2), \
                 (uint64_t)(a3), (uint64_t)(a4), (uint64_t)(a5), (uint64_t)(a6))
