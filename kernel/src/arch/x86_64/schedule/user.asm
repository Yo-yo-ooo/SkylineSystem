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

syscall_entry:
    cli
    clac                              ; force AC=0: userspace cannot pre-set it to bypass SMAP                               ; syscall不会自动关中断
    swapgs                            ; 切换为内核 GS

    mov [gs:16], rsp                  ; 保存用户栈到 cpu_t.user_scratch
    mov rsp, [gs:8]                   ; 切换到内核栈

    ; 构建标准中断栈帧 (严格匹配 context_t)
    ; 审计卫生 round 21: 0x1B/0x23 为 GDT ring-3 用户数据/代码段选择子
    ; (权威定义见 gdt 初始化表; 汇编无法用 C 宏, 裸值+注释)
    push qword 0x1B                   ; 用户态 SS
    push qword [gs:16]                ; 用户态 RSP
    push r11                          ; RFLAGS
    push qword 0x23                   ; 用户态 CS
    push rcx                          ; 用户态 RIP
    push qword 0                      ; 错误码（占位）
    push qword 0                      ; 中断号（占位）

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

    mov rdi, rsp
    cld 

    call syscall_handler

    ; ---- 系统调用返回值 (关键修复) ------------------------------------
    ; 原实现写的是 `mov [rsp + 14*8], rax` —— 依赖"syscall_handler 返回时
    ; %rax 仍是返回值"。但 %rax 是 caller-saved: 只要 C++ 侧在
    ;   frame->rax = ret;
    ; 之后还有任何一步(函数调用/辅助代码), %rax 就会被覆盖, 用户态拿到的
    ; 就是那个残留值。实测表现极具误导性: 内核日志里 handler 返回
    ; 0x320 / 0xFFFF8000FD000000, 用户态却收到 0 或 1, 于是桌面在
    ; "framebuffer info failed" 上自杀。
    ; 正确做法: 从**帧槽**读回返回值 (syscall_handler 总是写 frame->rax),
    ; 而帧槽就在 [rsp+112] —— 与 %rax 是否被覆盖无关。
    mov rax, [rsp + 14*8]

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

    add rsp, 16                       ; 跳过中断号、错误码
    pop rcx                           ; 恢复用户 RIP 到 rcx
    add rsp, 8                        ; 跳过 CS
    pop r11                           ; 恢复 RFLAGS 到 r11

    ; 此时栈顶刚好是保存的用户态 RSP。读取它并赋给 RSP 避开 gs:16 污染问题
    mov rsp, [rsp]                    

    swapgs                            ; 切回用户态 GS
    o64 sysret