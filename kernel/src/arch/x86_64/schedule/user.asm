[bits 64]
[global syscall_entry]
[extern syscall_handle]

syscall_entry:
    ; Currently running on user gs, switch to the thread gs
    swapgs ; switch
    mov [gs:0], rsp ; Save user stack in gs
    mov rsp, [gs:8] ; Kernel stack
    push 0
    push 0
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
    call syscall_handle

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

    mov [gs:8], rsp
    mov rsp, [gs:0]
    swapgs ; swap again, now kernel stack is the kernel gs again
    o64 sysret