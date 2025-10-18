.section .data
msg:
    .ascii "Hello World! \nTHIS IS SKYLINESYSTEM USER PATTERN\n"
len = . - msg

.section .text
.global phl
phl:
    mov $1, %rdi            # fd
    mov $msg, %rsi          # buffer
    mov $len, %rdx          # count
    mov $1, %rax            # write(2)系统调用号，64位系统为1
    syscall
    retq