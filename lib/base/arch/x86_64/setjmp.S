/* SPDX-FileCopyrightText: 2026 Yo-yo-ooo */
/* SPDX-License-Identifier: MIT */

/*
 * lib/base/arch/x86_64/setjmp.S
 *
 * SkylineSystem 用户态 libc 的 setjmp/longjmp (x86_64 SysV 调用约定)。
 *
 * jmp_buf 布局 (与 lib/include/stdc/setjmp.h 保持一致):
 *    0(%rdi) rbx
 *    8(%rdi) rbp
 *   16(%rdi) r12
 *   24(%rdi) r13
 *   32(%rdi) r14
 *   40(%rdi) r15
 *   48(%rdi) rsp   —— setjmp 返回之后的栈指针 (即 ret 弹出返回地址后的 rsp)
 *   56(%rdi) rip   —— setjmp 的返回地址
 *
 * ISO C 语义:
 *   - setjmp 直接调用返回 0;
 *   - longjmp(env, val) 使 setjmp 看起来返回 val; val == 0 时必须变成 1;
 *   - longjmp 不返回 (noreturn)。
 *
 * 注意: 这里只有"调用环境", 不保存/恢复信号掩码 (ISO C 语义; POSIX 的
 * sigsetjmp 不在本仓库范围内)。Lua 5.4 在未定义 LUA_USE_POSIX 时走
 * plain setjmp/longjmp 路径, 与本实现完全匹配。
 */

.text

/* int setjmp(jmp_buf env) */
.globl setjmp
.type setjmp, @function
setjmp:
    movq %rbx,  0(%rdi)
    movq %rbp,  8(%rdi)
    movq %r12, 16(%rdi)
    movq %r13, 24(%rdi)
    movq %r14, 32(%rdi)
    movq %r15, 40(%rdi)
    leaq 8(%rsp), %rax          /* setjmp 返回后的栈指针 */
    movq %rax, 48(%rdi)
    movq (%rsp), %rax           /* setjmp 的返回地址 */
    movq %rax, 56(%rdi)
    xorl %eax, %eax             /* 直接调用时返回 0 */
    ret
.size setjmp, .-setjmp

/* void longjmp(jmp_buf env, int val) */
.globl longjmp
.type longjmp, @function
longjmp:
    movl %esi, %eax             /* eax = val (movl 会清零高 32 位) */
    testl %eax, %eax
    jne 1f
    movl $1, %eax               /* ISO C: val == 0 -> 1 */
1:
    movq  0(%rdi), %rbx
    movq  8(%rdi), %rbp
    movq 16(%rdi), %r12
    movq 24(%rdi), %r13
    movq 32(%rdi), %r14
    movq 40(%rdi), %r15
    movq 48(%rdi), %rsp
    movq 56(%rdi), %rdx
    jmp *%rdx
.size longjmp, .-longjmp

/* 栈不可执行标记 (与 .so 其余部分保持一致; 汇编文件没有 .note.GNU-stack
   时 ld 会给出可执行栈的告警并可能破坏 -z noexecstack 语义) */
.section .note.GNU-stack, "", @progbits
