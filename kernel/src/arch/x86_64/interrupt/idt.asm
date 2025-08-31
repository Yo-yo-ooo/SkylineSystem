[bits 64]
[extern idt_exception_handler]
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

%macro isr_no_err_stub 1
int_stub%+%1:
    push 0  ; When the CPU doesn't push any error code.
    push %1
    pushaq

    mov rdi, rsp
    call idt_exception_handler

    popaq
    add rsp, 16 ; pop int32_t number and error code.
    iretq
%endmacro

%macro isr_err_stub 1
int_stub%+%1:
    ; The CPU already pushed error code.
    push %1
    pushaq

    mov rdi, rsp
    call idt_exception_handler

    popaq
    add rsp, 16 ; pop int32_t number and error code.
    iretq
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
