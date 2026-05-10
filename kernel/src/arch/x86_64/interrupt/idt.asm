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

%define OFFSET_CPU_ININTR 2684 

%macro isr_common_logic 1
    pushaq

    ; --- Set InIntr = true ---
    call this_cpu          ; return rax = cpu_t*
    mov byte [rax + OFFSET_CPU_ININTR], 1

    mov rdi, rsp           
    call idt_exception_handler

    call this_cpu     
    mov byte [rax + OFFSET_CPU_ININTR], 0

    popaq
    add rsp, 16
    iretq
%endmacro

; 然后修改你的 stub 宏
%macro isr_no_err_stub 1
int_stub%+%1:
    push 0
    push %1
    isr_common_logic %1
%endmacro

%macro isr_err_stub 1
int_stub%+%1:
    push %1
    isr_common_logic %1
%endmacro

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

idt_int_table:
    %assign i 0
    %rep 256
    dq int_stub%+i
    %assign i i+1
    %endrep
