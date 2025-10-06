#pragma once
#ifndef _SYSCALL_X86_64_H
#define _SYSCALL_X86_64_H

#include <stdint.h>
#include <stddef.h>
#include <libfg.h>

typedef struct hal_intr_PtReg {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbx, rcx, rdx, rsi, rdi, rbp;
    uint64_t ds, es;
    uint64_t rax;
    uint64_t func, errCode;
    uint64_t rip, cs, rflags, rsp, ss;
} __attribute__ ((packed)) hal_intr_PtReg;

#define _call_syscallInst \
__asm__ volatile ( \
	"syscall			\n\t" \
	: "=a"(val) \
	: "D"(idx), "S"((uint64_t)&regs) \
	: "memory" \
); 

static inline __attribute__ ((always_inline)) uint64_t hal_task_syscall_api0(uint64_t idx) {
	hal_intr_PtReg regs; uint64_t val;
	_call_syscallInst
	return val;
}

static inline __attribute__ ((always_inline)) uint64_t hal_task_syscall_api1(uint64_t idx, uint64_t arg0) {
	hal_intr_PtReg regs; uint64_t val;
	regs.rdi = arg0;
	_call_syscallInst
	return val;
}

static inline __attribute__ ((always_inline)) uint64_t hal_task_syscall_api2(uint64_t idx, uint64_t arg0, uint64_t arg1) {
	hal_intr_PtReg regs; uint64_t val;
	regs.rdi = arg0;
	regs.rsi = arg1;
	_call_syscallInst
	return val;
}

static inline __attribute__ ((always_inline)) uint64_t hal_task_syscall_api3(uint64_t idx, uint64_t arg0, uint64_t arg1, uint64_t arg2) {
	hal_intr_PtReg regs; uint64_t val;
	regs.rdi = arg0;
	regs.rsi = arg1;
	regs.rdx = arg2;
	_call_syscallInst
	return val;
}

static inline __attribute__ ((always_inline)) uint64_t hal_task_syscall_api4(uint64_t idx, uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3) {
	hal_intr_PtReg regs; uint64_t val;
	regs.rdi = arg0;
	regs.rsi = arg1;
	regs.rdx = arg2;
	regs.rcx = arg3;
	_call_syscallInst
	return val;
}

static inline __attribute__ ((always_inline)) uint64_t hal_task_syscall_api5(uint64_t idx, uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4) {
	hal_intr_PtReg regs; uint64_t val;
	regs.rdi = arg0;
	regs.rsi = arg1;
	regs.rdx = arg2;
	regs.rcx = arg3;
	regs.r8 = arg4;
	_call_syscallInst
	return val;
}

static inline __attribute__ ((always_inline)) uint64_t hal_task_syscall_api6(uint64_t idx, uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5) {
	hal_intr_PtReg regs; uint64_t val;
	regs.rdi = arg0;
	regs.rsi = arg1;
	regs.rdx = arg2;
	regs.rcx = arg3;
	regs.r8 = arg4;
	regs.r9 = arg5;
	_call_syscallInst

	return val;
}

#undef _call_syscallInst

#define hal_task_syscall0(index) \
	hal_task_syscall_api0(index)
#define hal_task_syscall(x, index, ...) \
	hal_task_syscall_api##x(index, map(x, (uint64_t), __VA_ARGS__))

#define hal_task_syscall1(index, ...) \
	hal_task_syscall(1, index, __VA_ARGS__)

#define hal_task_syscall2(index, ...) \
	hal_task_syscall(2, index, __VA_ARGS__)

#define hal_task_syscall3(index, ...) \
	hal_task_syscall(3, index, __VA_ARGS__)

#define hal_task_syscall4(index, ...) \
	hal_task_syscall(4, index, __VA_ARGS__)

#define hal_task_syscall5(index, ...) \
	hal_task_syscall(5, index, __VA_ARGS__)

#define hal_task_syscall6(index, ...) \
	hal_task_syscall(6, index, __VA_ARGS__)

#endif