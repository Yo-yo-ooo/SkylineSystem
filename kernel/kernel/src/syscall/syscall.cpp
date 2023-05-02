#include "./syscall.h"

void syscall_init(){
    
    //设置IA32_STAR寄存器
    wrmsr(0xc0000081, 0, (24 << 16) | 8);
    wrmsr(0xC0000082, 0, 0);
    wrmsr(0xC0000083, 0, 0);
    //设置IA32_FMASK寄存器, 不改变RFLAGS
    wrmsr(0xc0000084, 0, 0);
}