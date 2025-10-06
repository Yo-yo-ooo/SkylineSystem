#include <syscall.h>

int main(){
    uint64_t a;
    asm volatile("mov $1, %rax\n\t"
                 "syscall\n\t" : "=a" (a));
    sys_write(1,(void*)"Hello, World!\n",14);
    while(1);
    return 0;
}