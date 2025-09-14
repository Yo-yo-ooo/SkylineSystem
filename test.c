int main(){    
    asm volatile(
        "mov 1, %rax\n\t"/* 
        "mov 0, %rdi\n\t"
        "mov 0, %rsi\n\t"
        "mov 0, %rdx\n\t"
        "mov 0, %r10\n\t"
        "mov 0, %r8 \n\t"
        "mov 0, %r9 \n\t" */
        "syscall\n\t"
    );

    return 0;
}