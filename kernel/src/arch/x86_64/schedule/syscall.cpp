//#include <arch/x86_64/smp/smp.h>
#include <klib/klib.h>
#include <arch/x86_64/schedule/syscall.h>
#include <klib/kio.h>
#include <errno.h>
#include <arch/x86_64/schedule/sched.h>

uint64_t SYSCALL_NR(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t){
    return NULL;
}

uint64_t (*syscall_lists[256])(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t) = \
    {SYSCALL_NR};

void dump_REG(syscall_frame_t *frame){
    kinfoln("START DUMP REG");
    kinfoln("RAX:0x%X RBX:0x%X RCX:0x%X RDX:0x%X",frame->rax,frame->rbx,frame->rcx,frame->rdx);
    kinfoln("RSI:0x%X RDI:0x%X RBP:0x%X RSP:0x%X",frame->rsi,frame->rdi,frame->rbp,frame->rsp);
    kinfoln("R8:0x%X R9:0x%X R10:0x%X R11:0x%X",frame->r8,frame->r9,frame->r10,frame->r11);
    kinfoln("END DUMP REG");
}

extern "C" void syscall_handler(syscall_frame_t *frame) {
    //kinfoln("HIT SYSCALL!");
    frame->rax = syscall_lists[frame->rax]
    (frame->rdi, frame->rsi, frame->rdx, frame->r10, frame->r8, frame->r9);
    return;
}

extern "C" void syscall_entry();

void syscall_init() {
    
    syscall_lists[0] = sys_read;
    syscall_lists[1] = sys_write;
    syscall_lists[2] = sys_open;

    syscall_lists[8] = sys_lseek;
    syscall_lists[32] = sys_dup;
    syscall_lists[33] = sys_dup2;
    syscall_lists[39] = sys_getpid;
    syscall_lists[57] =  sys_fork;
    syscall_lists[59] =  sys_execve;

    uint64_t efer = rdmsr(IA32_EFER);
    efer |= (1 << 0);
    wrmsr(IA32_EFER, efer);
    uint64_t star = 0;
    star |= ((uint64_t)0x08 << 32);
    star |= ((uint64_t)0x13 << 48);
    wrmsr(IA32_STAR, star);
    wrmsr(IA32_LSTAR, (uint64_t)syscall_entry);
    kinfo("User INIT(syscall_init): enabled.\n");
}