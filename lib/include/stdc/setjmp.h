//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT

/*
 * setjmp.h — ISO C 非局部跳转 (SkylineSystem 用户态 libc)
 *
 * x86_64 SysV 调用约定下的最小 jmp_buf 布局 (实现: lib/base/arch/x86_64/setjmp.S):
 *   [0] rbx   [1] rbp   [2] r12   [3] r13   [4] r14   [5] r15
 *   [6] rsp   (setjmp 返回之后的栈指针, 即 ret 之后的 rsp)
 *   [7] rip   (setjmp 调用点的返回地址)
 *
 * 与 glibc 的 __jmp_buf_tag 布局不兼容 —— 本仓库的 setjmp/longjmp 是
 * 完全自洽的一对实现, 不允许与宿主 libc 的 setjmp 混用。
 */

#pragma once
#ifndef SKYLINE_LIB_SETJMP_H
#define SKYLINE_LIB_SETJMP_H

#ifdef __cplusplus
extern "C" {
#endif

typedef long int __sky_jmp_buf[8];
typedef __sky_jmp_buf jmp_buf;

/* ISO C: setjmp 正常返回 0; longjmp 之后以传入的 val (val==0 时取 1) 返回 */
int setjmp(jmp_buf env) __attribute__((__returns_twice__));

/* ISO C: val == 0 时调用点实际得到 1; longjmp 不返回 */
void longjmp(jmp_buf env, int val) __attribute__((__noreturn__));

#ifdef __cplusplus
}
#endif

#endif /* SKYLINE_LIB_SETJMP_H */
