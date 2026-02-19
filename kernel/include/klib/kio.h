/*
* SPDX-License-Identifier: GPL-2.0-only
* File: kio.h
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
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#if defined(__x86_64__) || defined(__i386__)

#define close_interrupt __asm__ volatile("cli":::"memory")
#define open_interrupt __asm__ volatile("sti":::"memory")

static inline void io_out8(uint16_t port, uint8_t data) {
    __asm__ volatile("outb %b0, %w1" : : "a"(data), "Nd"(port));
}

static inline uint8_t io_in8(uint16_t port) {
    uint8_t data;
    __asm__ volatile("inb %w1, %b0" : "=a"(data) : "Nd"(port));
    return data;
}

static inline uint16_t io_in16(uint16_t port) {
    uint16_t data;
    __asm__ volatile("inw %w1, %w0" : "=a"(data) : "Nd"(port));
    return data;
}

static inline void io_out16(uint16_t port, uint16_t data) {
    __asm__ volatile("outw %w0, %w1" : : "a"(data), "Nd"(port));
}

static inline uint32_t io_in32(uint16_t port) {
    uint32_t data;
    __asm__ volatile("inl %1, %0" : "=a"(data) : "Nd"(port));
    return data;
}

static inline void io_out32(uint16_t port, uint32_t data) {
    __asm__ volatile("outl %0, %1" : : "a"(data), "Nd"(port));
}

static inline void flush_tlb(uint64_t addr) {
    __asm__ volatile("invlpg (%0)"::"r" (addr) : "memory");
}

static inline uint64_t get_cr3(void) {
    uint64_t cr0;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr0));
    return cr0;
}

static inline uint64_t get_rsp(void) {
    uint64_t rsp;
    __asm__ volatile("mov %%rsp, %0" : "=r"(rsp));
    return rsp;
}

static inline uint64_t get_rflags() {
    uint64_t rflags;
    __asm__ volatile (
            "pushfq\n"
            "pop %0\n"
            : "=r"(rflags)
            :
            : "memory"
            );
    return rflags;
}

static inline void set_rflags(uint64_t rflags){
    __asm__ volatile (
            "push %0\n"
            "popfq\n" : : "r"(rflags) : "memory");
}

static inline void xsaves(uint8_t* ptr, uint64_t mask)
{
	uint32_t eax = mask;
	uint32_t edx = mask >> 32;

	asm volatile("xsave %0" : "=m"(*ptr) : "a" (eax), "d" (edx));
}

static inline void xrstors(uint8_t* ptr, uint64_t mask)
{
	uint32_t eax = mask;
	uint32_t edx = mask >> 32;

	asm volatile("xrstor %0" :: "m"(*ptr), "a" (eax), "d" (edx));
}


static inline void set_cr4(uint64_t cr4){
    __asm__ volatile("mov %0,%%cr4" : :"r"(cr4) : "memory");
}
static inline uint64_t get_cr4(void){
    uint64_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4) : : "memory");
    return cr4;
}

static inline void xsetbv(uint32_t index, uint64_t value)
{
	uint32_t eax = value;
	uint32_t edx = value >> 32;

	asm volatile("xsetbv" :: "a" (eax), "d" (edx), "c" (index));
}

static inline uint64_t xgetbv(uint32_t index)
{
	uint32_t eax, edx;

	asm volatile("xgetbv" : "=a" (eax), "=d" (edx) : "c" (index));
	return eax + ((uint64_t)edx << 32);
}

static inline void mmio_write32(uint32_t *addr, uint32_t data) {
    *(volatile uint32_t *) addr = data;
}

static inline uint32_t mmio_read32(void *addr) {
    return *(volatile uint32_t *) addr;
}

static inline uint64_t mmio_read64(void* addr) {
    return *(volatile uint64_t *) addr;
}

static inline void mmio_write64(void* addr, uint64_t data){
    *(volatile uint64_t *) addr = data;
}

static inline uint64_t load(uint64_t *addr) {
    uint64_t ret = 0;
    __asm__ volatile (
            "lock xadd %[ret], %[addr];"
            : [addr] "+m"(*addr),
    [ret] "+r"(ret)
    : : "memory"
    );
    return ret;
}

static inline void store(uint64_t *addr, uint32_t value) {
    __asm__ volatile (
            "lock xchg %[value], %[addr];"
            : [addr] "+m"(*addr),
    [value] "+r"(value)
    : : "memory"
    );
}

static inline void cpuid(const uint32_t leaf,
      const uint32_t subleaf,
      uint64_t *const a,
      uint64_t *const b,
      uint64_t *const c,
      uint64_t *const d)
{
    asm volatile("cpuid"
                 : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                 : "a" (leaf), "c" (subleaf));
}

static inline bool cpuid(uint32_t leaf, uint32_t subleaf,
          uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx) {
    uint32_t cpuid_max;
    asm volatile ("cpuid"
                  : "=a" (cpuid_max)
                  : "a" (leaf & 0x80000000)
                  : "ebx", "ecx", "edx");
    if (leaf > cpuid_max)
        return false;
    asm volatile ("cpuid"
                  : "=a" (*eax), "=b" (*ebx), "=c" (*ecx), "=d" (*edx)
                  : "a" (leaf), "c" (subleaf));
    return true;
}

static inline void outb(uint16_t port, uint8_t value) {
    asm volatile ("outb %%al, %1"  : : "a" (value), "Nd" (port) : "memory");
}

static inline void outw(uint16_t port, uint16_t value) {
    asm volatile ("outw %%ax, %1"  : : "a" (value), "Nd" (port) : "memory");
}

static inline void outd(uint16_t port, uint32_t value) {
    asm volatile ("outl %%eax, %1" : : "a" (value), "Nd" (port) : "memory");
}

static inline uint8_t inb(uint16_t port) {
    uint8_t value;
    asm volatile ("inb %1, %%al"  : "=a" (value) : "Nd" (port) : "memory");
    return value;
}

static inline uint16_t inw(uint16_t port) {
    uint16_t value;
    asm volatile ("inw %1, %%ax"  : "=a" (value) : "Nd" (port) : "memory");
    return value;
}

static inline uint32_t ind(uint16_t port) {
    uint32_t value;
    asm volatile ("inl %1, %%eax" : "=a" (value) : "Nd" (port) : "memory");
    return value;
}

#define inl ind
#define outl outd

#define IA32_EFER  0xC0000080
#define IA32_STAR  0xC0000081
#define IA32_LSTAR 0xC0000082
#define IA32_CSTAR 0xC0000083
#define IA32_FMASK 0xC0000084

#define FS_BASE 0xC0000100
#define GS_BASE 0xC0000102
#define KERNEL_GS_BASE 0xC0000102

inline void io_wait()
{
    inb(0x80);
    outb(0x80, 0);
    //asm volatile ("outb %%al, $0x80" : : "a"(0));
} 

inline void io_wait(uint64_t ms)
{
    while (ms--)
    {
        inb(0x80);
        outb(0x80, 0);
    }
} 

static inline void mmoutb(uintptr_t addr, uint8_t value) {
    asm volatile (
        "movb %1, (%0)"
        :
        : "r" (addr), "ir" (value)
        : "memory"
    );
}

static inline void mmoutw(uintptr_t addr, uint16_t value) {
    asm volatile (
        "movw %1, (%0)"
        :
        : "r" (addr), "ir" (value)
        : "memory"
    );
}

static inline void mmoutd(uintptr_t addr, uint32_t value) {
    asm volatile (
        "movl %1, (%0)"
        :
        : "r" (addr), "ir" (value)
        : "memory"
    );
}



#if defined (__x86_64__)
static inline void mmoutq(uintptr_t addr, uint64_t value) {
    asm volatile (
        "movq %1, (%0)"
        :
        : "r" (addr), "r" (value)
        : "memory"
    );
}
#endif

static inline uint8_t mminb(uintptr_t addr) {
    uint8_t ret;
    asm volatile (
        "movb (%1), %0"
        : "=r" (ret)
        : "r"  (addr)
        : "memory"
    );
    return ret;
}

static inline uint16_t mminw(uintptr_t addr) {
    uint16_t ret;
    asm volatile (
        "movw (%1), %0"
        : "=r" (ret)
        : "r"  (addr)
        : "memory"
    );
    return ret;
}

static inline uint32_t mmind(uintptr_t addr) {
    uint32_t ret;
    asm volatile (
        "movl (%1), %0"
        : "=r" (ret)
        : "r"  (addr)
        : "memory"
    );
    return ret;
}

#if defined (__x86_64__)
static inline uint64_t mminq(uintptr_t addr) {
    uint64_t ret;
    asm volatile (
        "movq (%1), %0"
        : "=r" (ret)
        : "r"  (addr)
        : "memory"
    );
    return ret;
}
#endif

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t edx, eax;
    asm volatile ("rdmsr"
                  : "=a" (eax), "=d" (edx)
                  : "c" (msr)
                  : "memory");
    return ((uint64_t)edx << 32) | eax;
}

static inline void wrmsr(uint32_t msr, uint64_t value) {
    uint32_t edx = value >> 32;
    uint32_t eax = (uint32_t)value;
    asm volatile ("wrmsr"
                  :
                  : "a" (eax), "d" (edx), "c" (msr)
                  : "memory");
}

static inline uint64_t rdtsc(void) {
    uint32_t edx, eax;
    asm volatile ("rdtsc" : "=a" (eax), "=d" (edx));
    return ((uint64_t)edx << 32) | eax;
}

#define rdrand(type) ({ \
    type rdrand__ret; \
    asm volatile ( \
        "1: " \
        "rdrand %0;" \
        "jnc 1b;" \
        : "=r" (rdrand__ret) \
    ); \
    rdrand__ret; \
})

#define rdseed(type) ({ \
    type rdseed__ret; \
    asm volatile ( \
        "1: " \
        "rdseed %0;" \
        "jnc 1b;" \
        : "=r" (rdseed__ret) \
    ); \
    rdseed__ret; \
})

#define write_cr(reg, val) do { \
    asm volatile ("mov %0, %%cr" reg :: "r" (val) : "memory"); \
} while (0)

#define read_cr(reg) ({ \
    size_t read_cr__cr; \
    asm volatile ("mov %%cr" reg ", %0" : "=r" (read_cr__cr) :: "memory"); \
    read_cr__cr; \
})

#define locked_read(var) ({ \
    typeof(*var) locked_read__ret = 0; \
    asm volatile ( \
        "lock xadd %0, %1" \
        : "+r" (locked_read__ret) \
        : "m" (*(var)) \
        : "memory" \
    ); \
    locked_read__ret; \
})

#define locked_write(var, val) do { \
    __auto_type locked_write__ret = val; \
    asm volatile ( \
        "lock xchg %0, %1" \
        : "+r" ((locked_write__ret)) \
        : "m" (*(var)) \
        : "memory" \
    ); \
} while (0)

static inline void bit_set1(uint64_t *addr, uint64_t index) { 
	__asm__ volatile (
		"btsq %1, %0		\n\t"
		: "+m"(*addr)
		: "r"(index)
		: "memory");
}

static inline void bit_set0(uint64_t *addr, uint64_t index) {
	__asm__ volatile (
		"btrq %1, %0		\n\t"
		: "+m"(*addr)
		: "r"(index)
		: "memory");
}

static inline void bit_set0_16(uint16_t *addr, uint16_t index) { 
	__asm__ volatile (
		"btrw %1, %0		\n\t"
		: "+m"(*addr)
		: "r"(index)
		: "memory");
}

static inline void bit_set1_16(uint16_t *addr, uint16_t index) { 
	__asm__ volatile (
		"btsw %1, %0		\n\t"
		: "+m"(*addr)
		: "r"(index)
		: "memory");
}

static inline void bit_set0_32(uint32_t *addr, uint32_t index) {
	__asm__ volatile (
		"btrl %1, %0		\n\t"
		: "+m"(*addr)
		: "r"(index)
		: "memory");
}

static inline void bit_set1_32(uint32_t *addr, uint32_t index) { 
	__asm__ volatile (
		"btsl %1, %0		\n\t"
		: "+m"(*addr)
		: "r"(index)
		: "memory");
}


static inline void xcr0_write(uint32_t xcr, uint64_t value)
{
    uint32_t eax = (uint32_t)value;
    uint32_t edx = value >> 32;
    asm volatile("xsetbv" : : "a"(eax), "d"(edx), "c"(xcr) : "memory");
}

static inline uint64_t cr4_read()
{
    uint64_t cr4;
    asm volatile("mov %%cr4, %0" : "=r"(cr4));
    return cr4;
}

static inline void cr4_write(uint64_t value)
{
    asm volatile("mov %0, %%cr4" : : "r"(value));
}

static inline uint64_t cr3_read()
{
    uint64_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}

static inline void cr3_write(uint64_t value)
{
    asm volatile("mov %0, %%cr3" : : "r"(value));
}

static inline uint64_t cr2_read()
{
    uint64_t cr2;
    asm volatile("mov %%cr2, %0" : "=r"(cr2));
    return cr2;
}

static inline void cr2_write(uint64_t value)
{
    asm volatile("mov %0, %%cr2" : : "r"(value));
}

static inline uint64_t cr0_read()
{
    uint64_t cr0;
    asm volatile("mov %%cr0, %0" : "=r"(cr0));
    return cr0;
}

static inline void cr0_write(uint64_t value)
{
    asm volatile("mov %0, %%cr0" : : "r"(value));
}

static inline void delay(uint64_t cycles) {
    uint64_t next_stop = rdtsc() + cycles;

    while (rdtsc() < next_stop);
}

#define mfence() __asm__ volatile ("mfence 	\n\t" : : : "memory")

#elif defined (__riscv)

static inline uint64_t rdtsc(void) {
    uint64_t v;
    asm volatile ("rdcycle %0" : "=r"(v));
    return v;
}

#define csr_read(csr) ({\
    size_t v;\
    asm volatile ("csrr %0, " csr : "=r"(v));\
    v;\
})

#define csr_write(csr, v) ({\
    size_t old;\
    asm volatile ("csrrw %0, " csr ", %1" : "=r"(old) : "r"(v));\
    old;\
})

#define make_satp(mode, ppn) (((size_t)(mode) << 60) | ((size_t)(ppn) >> 12))

#define locked_read(var) ({ \
    typeof(*var) locked_read__ret; \
    asm volatile ( \
        "ld %0, (%1); fence r, rw" \
        : "=r"(locked_read__ret) \
        : "r"(var) \
        : "memory" \
    ); \
    locked_read__ret; \
})

extern size_t bsp_hartid;

struct riscv_hart {
    struct riscv_hart *next;
    const char *isa_string;
    size_t hartid;
    uint32_t acpi_uid;
    uint8_t mmu_type;
    uint8_t flags;
};

#define RISCV_HART_COPROC  ((uint8_t)1 << 0)  // is a coprocessor
#define RISCV_HART_HAS_MMU ((uint8_t)1 << 1)  // `mmu_type` field is valid

extern struct riscv_hart *hart_list;

bool riscv_check_isa_extension_for(size_t hartid, const char *ext, size_t *maj, size_t *min);

static inline bool riscv_check_isa_extension(const char *ext, size_t *maj, size_t *min) {
    return riscv_check_isa_extension_for(bsp_hartid, ext, maj, min);
}

void init_riscv(void);

#elif defined (__loongarch64)

#define LOONGARCH_CSR_TVAL 0x42

static inline uint64_t rdtsc(void) {
    uint64_t v;
    asm volatile ("csrrd %0, %1" : "=r" (v) : "i" (LOONGARCH_CSR_TVAL));
    return v;
}

#endif




//HAL

#if defined(__x86_64__) || defined(__i386__)

#define ENABLE_INTERRUPTS() asm volatile ("sti")
#define DISABLE_INTERRUPTS() asm volatile ("cli")

#elif defined(__riscv)

#define ENABLE_INTERRUPTS() asm volatile ("csrsi mstatus, 8")
#define DISABLE_INTERRUPTS() asm volatile ("csrci mstatus, 8")

#elif defined(__loongarch64)

#define ENABLE_INTERRUPTS() asm volatile ("csrsi sstatus, 1")
#define DISABLE_INTERRUPTS() asm volatile ("csrci sstatus, 1")

#endif