; SPDX-License-Identifier: GPL-2.0-only
; File: idt.asm

[bits 64]
[extern idt_exception_handler]
[extern this_cpu]
[global idt_int_table]

%define CPU_ININTR_OFF 2332

; 严格按照 context_t 结构体的顺序压栈 (r15 在最底部，即最低地址)
%macro pushaq 0
    push r15
    push r14
    push r13
    push r12
    push r11
    push r10
    push r9
    push r8
    push rdi
    push rsi
    push rbp
    push rbx
    push rdx
    push rcx
    push rax
%endmacro

%macro popaq 0
    pop rax
    pop rcx
    pop rdx
    pop rbx
    pop rbp
    pop rsi
    pop rdi
    pop r8
    pop r9
    pop r10
    pop r11
    pop r12
    pop r13
    pop r14
    pop r15
%endmacro

%macro isr_common_logic 1
    ; 因为设置了 IST=1，CPU 无条件压入了 SS 和 RSP
    ; 栈结构为 7 个元素 (和用户态完全一致)
    ; [rsp]    int_no
    ; [rsp+8]  err_code
    ; [rsp+16] RIP
    ; [rsp+24] CS
    ; [rsp+32] RFLAGS
    ; [rsp+40] RSP
    ; [rsp+48] SS
    
    pushaq

    ; CS 在 120(pushaq) + 24 = 144
    mov rax, [rsp + 144]    ; CS
    test al, 3
    jz %%kernel_mode_entry
    swapgs
%%kernel_mode_entry:

    ; 【关键修复】：删除硬编码偏移，防止破坏 cpu_t 结构体！
    ; inc byte gs:[CPU_ININTR_OFF]

    mov rdi, rsp
    mov r12, rsp
    and rsp, ~15
    cld

    call idt_exception_handler

    mov rsp, r12

    ; 【关键修复】：删除硬编码偏移！
    ; dec byte gs:[CPU_ININTR_OFF]

    ; RFLAGS 在 120 + 32 = 152
    mov rax, [rsp + 152]     ; RFLAGS
    and rax, ~0x4000         ; 清除 NT 位
    mov [rsp + 152], rax

    mov rax, [rsp + 144]    ; CS
    test al, 3
    jz %%kernel_mode_exit
    swapgs
%%kernel_mode_exit:

    popaq
    
    ; 因为 IST=1，CPU 压入了完整的 5 个元素 (RIP, CS, RFLAGS, RSP, SS)
    ; 所以直接跳过 int_no 和 err_code，执行 iretq 即可完美返回！
    add rsp, 16
    iretq
%endmacro

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