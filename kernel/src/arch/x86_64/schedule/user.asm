; SPDX-License-Identifier: GPL-2.0-only
; File: user.asm
; Copyright (C) 2026 Yo-yo-ooo
;
; This file is part of SkylineSystem.
;
; SkylineSystem is free software; you can redistribute it and/or modify
; it under the terms of the GNU General Public License as published by
; the Free Software Foundation; either version 2 of the License, or
; (at your option) any later version.
;
; This program is distributed in the hope that it will be useful,
; but WITHOUT ANY WARRANTY; without even the implied warranty of
; MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
; GNU General Public License for more details.
;
; You should have received a copy of the GNU General Public License
; along with this program; if not, write to the Free Software
; Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.

[bits 64]
[section .text]
[global syscall_entry]
[extern syscall_handler]

[bits 64]
[section .text]
[global syscall_entry]
[extern syscall_handler]

syscall_entry:
    swapgs                           ; 切换为内核 GS

    mov [gs:16], rsp                 ; 保存用户栈到 cpu_t.user_scratch
    mov rsp, [gs:8]                  ; 切换到内核栈

    ; 构建标准中断栈帧 (严格匹配 context_t)
    push qword 0x1B                  ; 用户态 SS
    push qword [gs:16]               ; 用户态 RSP
    push r11                         ; RFLAGS (syscall 会把 rflags 存入 r11)
    push qword 0x23                  ; 用户态 CS
    push rcx                         ; 用户态 RIP (syscall 会把 rip 存入 rcx)
    push qword 0                     ; 错误码（占位）
    push qword 0                     ; 中断号（占位）

    ; 保存通用寄存器 (严格对齐你 IDT 中的 pushaq 顺序)
    push rax
    push rcx
    push rdx
    push rbx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    ; 准备传递 context_t 指针
    mov rdi, rsp
    
    ; 强制符合 C 语言 ABI 规范
    cld 
    ; 注：此处一共压入了 7 + 15 = 22 个 8 字节 (176 字节)。
    ; 176 是 16 的倍数，因此只要你的 [gs:8] 初始化时是 16 字节对齐的，
    ; 这里的 RSP 天然对齐，无需额外执行 and rsp, ~15。

    call syscall_handler

    ; 恢复通用寄存器 (严格对齐你 IDT 中的 popaq 顺序)
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rbp
    pop rbx
    pop rdx
    pop rcx
    pop rax

    add rsp, 16                      ; 跳过中断号、错误码
    pop rcx                          ; 恢复用户 RIP（sysret 消费 rcx）
    add rsp, 8                       ; 跳过 CS
    pop r11                          ; 恢复 RFLAGS（sysret 消费 r11）
    pop rsp                          ; 恢复用户栈指针

    swapgs                           ; 切回用户态 GS
    o64 sysret                       ; 或者写 sysretq