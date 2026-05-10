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
    ; Currently running on user gs, switch to the kernel gs
    cli
    swapgs ; switch
    mov [gs:0], rsp ; Save user stack in gs
    mov rsp, [gs:8] ; Kernel stack

    push qword rsp
    push qword 0x1b
    push qword [gs:0]
    push r11
    push qword 0x23
    push qword rcx
    push qword 0
    push qword 0

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
    call syscall_handler

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

    add rsp, 16
    pop rcx
    add rsp, 8
    pop r11
    pop qword [gs:0]
    add rsp, 8

    pop qword [gs:8]

    mov rsp, [gs:0]
    swapgs ; swap again, now kernel stack is the kernel gs again
    o64 sysret

