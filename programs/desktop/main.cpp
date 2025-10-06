#include <syscall.h>

int main(){
    asm volatile("mov $1, %rax\n\t"
                 "syscall\n\t");
    //sys_write(1,(void*)"Hello, World!\n",14);
    return 0;
}