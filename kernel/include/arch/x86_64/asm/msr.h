#pragma once
#ifndef _x86_64_MSR_H
#define _x86_64_MSR_H

#include <stdint.h>
#include <stddef.h>
#include <pdef.h>


constexpr uint32_t IA32_MSR_APIC_BASE = 0x1B;

constexpr uint32_t IA32_MSR_PAT = 0x277;
constexpr uint32_t IA32_MSR_MTRR_DEF_TYPE = 0x2FF;
constexpr uint32_t IA32_MSR_MTRRCAP = 0xFE;
constexpr uint32_t IA32_MSR_MTRR_PHYSBASE0 = 0x200;
constexpr uint32_t IA32_MSR_MTRR_PHYSMASK0 = 0x201;
constexpr uint32_t IA32_MSR_MTRR_PHYSBASE1 = 0x202;

constexpr uint32_t IA32_MSR_TSC_DEADLINE = 0x6E0;
constexpr uint32_t IA32_MSR_X2APIC_BASE = 0x800;

constexpr uint32_t IA32_MSR_MISC_ENABLE = 0x1a0;
constexpr uint32_t IA32_MSR_XSS = 0xda0;
constexpr uint32_t IA32_MSR_XFD = 0x1C4;
constexpr uint32_t IA32_MSR_XFD_ERR = 0x1C5;

constexpr uint32_t IA32_MSR_SYSENTER_CS = 0x174;
constexpr uint32_t IA32_MSR_SYSENTER_ESP = 0x174;
constexpr uint32_t IA32_MSR_SYSENTER_EIP = 0x174;

constexpr uint32_t IA32_MSR_EFER = 0xC0000080;

constexpr uint32_t IA32_MSR_FS_BASE = 0xC0000100;
constexpr uint32_t IA32_MSR_GS_BASE = 0xC0000101;
constexpr uint32_t IA32_MSR_KERNEL_GS_BASE = 0xC0000102;

// IA32_MSR_LSTAR is the address SYSCALL jumps to from ring3
constexpr uint32_t IA32_MSR_LSTAR = 0xC0000082;

// IA32_MSR_FMASK is a mask of bits to be removed from RFLAGS
// In SYSCALL; RFLAGS &= ~IA32_MSR_FMASK
constexpr uint32_t IA32_MSR_FMASK = 0xC0000084;

// IA32_MSR_STAR stores the CS (Code Segment) and SS (Stack Segment) info
// at bits [47:32] and [63:48] respectively.
constexpr uint32_t IA32_MSR_STAR = 0xC0000081;

#endif