#include <arch/x86_64/syscall.h>

uint64_t syscall6(uint64_t num,uint64_t a, \
uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f){
    uint64_t ret;
    asm volatile("mov %0, %%rax" : : "r"(num));
    asm volatile("mov %0, %%rdi" : : "r"(a));
    asm volatile("mov %0, %%rsi" : : "r"(b));
    asm volatile("mov %0, %%rdx" : : "r"(c));
    asm volatile("mov %0, %%r10" : : "r"(d));
    asm volatile("mov %0, %%r8" : : "r"(e));
    asm volatile("mov %0, %%r9" : : "r"(f));
    asm volatile("syscall");
    asm volatile("mov %%rax, %0" : "=r"(ret));
    return ret;
}

uint64_t syscall5(uint64_t num,uint64_t a, \
uint64_t b,uint64_t c,uint64_t d,uint64_t e){
    uint64_t ret;
    asm volatile("mov %0, %%rax" : : "r"(num));
    asm volatile("mov %0, %%rdi" : : "r"(a));
    asm volatile("mov %0, %%rsi" : : "r"(b));
    asm volatile("mov %0, %%rdx" : : "r"(c));
    asm volatile("mov %0, %%r10" : : "r"(d));
    asm volatile("mov %0, %%r8" : : "r"(e));
    asm volatile("syscall");
    asm volatile("mov %%rax, %0" : "=r"(ret));
    return ret;
}

uint64_t syscall4(uint64_t num,uint64_t a, \
uint64_t b,uint64_t c,uint64_t d){
    uint64_t ret;
    asm volatile("mov %0, %%rax" : : "r"(num));
    asm volatile("mov %0, %%rdi" : : "r"(a));
    asm volatile("mov %0, %%rsi" : : "r"(b));
    asm volatile("mov %0, %%rdx" : : "r"(c));
    asm volatile("mov %0, %%r10" : : "r"(d));
    asm volatile("syscall");
    asm volatile("mov %%rax, %0" : "=r"(ret));
    return ret;
}

uint64_t syscall3(uint64_t num,uint64_t a, \
uint64_t b,uint64_t c){
    uint64_t ret;
    asm volatile("mov %0, %%rax" : : "r"(num));
    asm volatile("mov %0, %%rdi" : : "r"(a));
    asm volatile("mov %0, %%rsi" : : "r"(b));
    asm volatile("mov %0, %%rdx" : : "r"(c));
    asm volatile("syscall");
    asm volatile("mov %%rax, %0" : "=r"(ret));
    return ret;
}

uint64_t syscall2(uint64_t num,uint64_t a, \
uint64_t b){
    uint64_t ret;
    asm volatile("mov %0, %%rax" : : "r"(num));
    asm volatile("mov %0, %%rdi" : : "r"(a));
    asm volatile("mov %0, %%rsi" : : "r"(b));
    asm volatile("syscall");
    asm volatile("mov %%rax, %0" : "=r"(ret));
    return ret;
}

uint64_t syscall1(uint64_t num,uint64_t a){
    uint64_t ret;
    asm volatile("mov %0, %%rax" : : "r"(num));
    asm volatile("mov %0, %%rdi" : : "r"(a));
    asm volatile("syscall");
    asm volatile("mov %%rax, %0" : "=r"(ret));
    return ret;
}

uint64_t syscall0(uint64_t num){
    uint64_t ret;
    asm volatile("mov %0, %%rax" : : "r"(num));
    asm volatile("syscall");
    asm volatile("mov %%rax, %0" : "=r"(ret));
    return ret;
}