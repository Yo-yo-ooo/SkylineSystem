//#include <arch/x86_64/smp/smp.h>
#include <klib/klib.h>
#include <arch/x86_64/schedule/syscall.h>
#include <klib/kio.h>
#include <errno.h>

u64 syscall_rsvd(syscall_args a) {return 0;}

void* syscall_handler_table[] = {
    syscall_rsvd,
};


extern "C" void syscall_handler(syscall_frame_t *frame) {
    if (frame->rax > sizeof(syscall_handler_table) / sizeof(uint64_t) || !syscall_handler_table[frame->rax]) {
        kerror("Unhandled syscall %d.\n", frame->rax);
        frame->rax = -ENOSYS;
        return;
    }
    switch (frame->rax) {
        case 15: // sigreturn
            //sys_sigreturn(frame);
            break;
        case 57: // fork
            frame->rax = sys_fork(frame);
            break;
        default: {
            uint64_t(*handler)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) =
                syscall_handler_table[frame->rax];
            frame->rax = handler(frame->rdi, frame->rsi, frame->rdx, frame->r10, frame->r8, frame->r9);
            break;
        }
    }
}

extern "C" void syscall_entry();

void user_init() {
    u64 efer = rdmsr(IA32_EFER);
    efer |= (1 << 0);
    wrmsr(IA32_EFER, efer);
    u64 star = 0;
    star |= ((u64)0x28 << 32); // kernel cs
    star |= ((u64)0x33 << 48); // user cs (it loads 0x30 + 16 for cs, which is 0x40 and + 8 for ss 0x38)
    wrmsr(IA32_STAR, star);
    wrmsr(IA32_LSTAR, (u64)syscall_entry);
    wrmsr(IA32_CSTAR, 0);
    wrmsr(IA32_CSTAR+1, 0x200);
    kinfo("   User INIT(user_init): enabled.\n");
}