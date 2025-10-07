.section .data
msg:
    .ascii "Hello World!\n"
len = . - msg

.section .text
phl:
    mov $1, %rdi            # fd
    mov $msg, %rsi          # buffer
    mov $len, %rdx          # count
    mov $1, %rax            # write(2)系统调用号，64位系统为1
    syscall