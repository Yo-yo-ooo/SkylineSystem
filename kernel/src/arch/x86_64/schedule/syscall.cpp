//#include <arch/x86_64/smp/smp.h>
#include <klib/klib.h>
#include <arch/x86_64/schedule/syscall.h>
#include <klib/kio.h>
#include <errno.h>
#include <arch/x86_64/schedule/sched.h>

extern "C" void syscall_handler(syscall_frame_t *frame) {
    kinfoln("HIT SYSCALL!");
    switch (frame->rax) {
        case 1: 
            kinfoln("Hello In USER!!!");
            break;
        case 2:
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