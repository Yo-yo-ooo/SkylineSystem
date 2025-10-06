//#include <arch/x86_64/smp/smp.h>
#include <klib/klib.h>
#include <arch/x86_64/schedule/syscall.h>
#include <klib/kio.h>
#include <errno.h>
#include <arch/x86_64/schedule/sched.h>

void dump_REG(syscall_frame_t *frame){
    kinfoln("START DUMP REG");
    kinfoln("RAX:0x%X RBX:0x%X RCX:0x%X RDX:0x%X",frame->rax,frame->rbx,frame->rcx,frame->rdx);
    kinfoln("RSI:0x%X RDI:0x%X RBP:0x%X RSP:0x%X",frame->rsi,frame->rdi,frame->rbp,frame->rsp);
    kinfoln("R8:0x%X R9:0x%X R10:0x%X R11:0x%X",frame->r8,frame->r9,frame->r10,frame->r11);
    kinfoln("END DUMP REG");
}

extern "C" void syscall_handler(syscall_frame_t *frame) {
    kinfoln("HIT SYSCALL!");
    switch (frame->rax) {
        case 1: 
            dump_REG(frame);
            kinfoln("Hello In USER!!!");
            break;
        case 2:
            dump_REG(frame);
            frame->rax = sys_write(frame->rdi, (void*)frame->rsi, frame->rdx);
            break;
        case 15: // sigreturn
            //sys_sigreturn(frame);
            break;
        case 57: // fork
            frame->rax = sys_fork(frame);
            break;
        case 60:
            Schedule::Exit(frame->rdi);
            break;
        default: {
            kerrorln("Undefined syscall:%d",frame->rax);
            frame->rax = -ENOSYS;
            return;
        }
    }
    return;
}

extern "C" void syscall_entry();

void syscall_init() {
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