.section .data
msg:
    .ascii "Hello World! \nTHIS IS SkylineSystem USER PATTERN\n"
len = . - msg

.section .text
.global exit
.global phl
phl:
    mov $1, %rdi            # fd
    mov $msg, %rsi          # buffer
    mov $len, %rdx          # count
    mov $1, %rax            # write(2)系统调用号，64位系统为1
    syscall
    retq

exit:
    mov $0, %rdi          # exit code 0
    mov $60,%rax
    syscall