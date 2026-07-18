; SPDX-License-Identifier: GPL-2.0-only
; File: idt.asm
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
[extern idt_exception_handler]
[extern this_cpu]
[global idt_int_table]

%define CPU_ININTR_OFF 2692

; 保存所有通用寄存器
%macro pushaq 0
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
%endmacro

; 恢复所有通用寄存器
%macro popaq 0
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
%endmacro

%macro isr_common_logic 1
    pushaq

    ; 1. 特权级判断与 swapgs
    mov rax, [rsp + 144]    ; 144 = 15*8(pushaq) + 8(err/no) + 8(int_no) + 8(RIP) = 136? 
                            ; 修正计算: 120(pushaq) + 16(stub压栈) = 136 为 RIP, 144 为 CS. 逻辑正确。
    test al, 3
    jz %%kernel_mode_entry  ; 【修复】使用 %% 避免宏展开标号重定义
    swapgs
%%kernel_mode_entry:

    ; 2. 使用 inc 代替 mov，支持中断/异常的嵌套重入
    inc byte gs:[CPU_ININTR_OFF]

    ; 3. 准备传递给 C 语言的参数 (rdi = rsp)
    mov rdi, rsp
    
    ; 4. 解决对齐问题：保存真实的 rsp 到 r12 
    ;  使用 r12 代替 rbx。因为 rbx 已经被 pushaq 压栈，用 r12 更清晰且 r12 也是 callee-saved。
    mov r12, rsp
    
    ; 5. 强制 16 字节对齐
    and rsp, ~15
    
    ; 6. 核心：清空方向标志位，满足 C ABI 要求！
    cld

    ; 调用 C 处理函数
    call idt_exception_handler

    ; 7. 恢复真实的未对齐栈指针
    mov rsp, r12            ; 【修复】恢复 r12 保存的栈指针

    ; 8. 嵌套退出：使用 dec 恢复状态
    dec byte gs:[CPU_ININTR_OFF]

    ; 9. 恢复 GS
    mov rax, [rsp + 144]
    test al, 3
    jz %%kernel_mode_exit   ; 【修复】使用 %%
    swapgs
%%kernel_mode_exit:

    popaq
    add rsp, 16             ; 跳过 we pushed int_no and error_code/dummy
    iretq
%endmacro

%macro isr_no_err_stub 1
int_stub%+%1:
    push 0                  ; 压入假错误码，统一栈结构
    push %1                 ; 压入中断号
    isr_common_logic %1
%endmacro

; 有错误码中断桩函数
%macro isr_err_stub 1
int_stub%+%1:
    push %1                 ; 压入中断号（CPU已自动压入错误码）
    isr_common_logic %1
%endmacro

; 生成 0~255 全部中断桩函数
%assign i 0
%rep 256
%if !(i == 8 || (i >= 10 && i <= 14) || i == 17 || i == 21 || i == 29 || i == 30)
isr_no_err_stub i
%else
isr_err_stub i
%endif
%assign i i+1
%endrep

section .data
; 中断向量表（256个中断处理函数地址）
idt_int_table:
    %assign i 0
    %rep 256
    dq int_stub%+i
    %assign i i+1
    %endrep