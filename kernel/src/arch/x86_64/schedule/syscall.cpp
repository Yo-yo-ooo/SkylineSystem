#include <arch/x86_64/schedule/sched.h>
#include <arch/x86_64/smp/smp.h>
#include <klib/klib.h>
#include <arch/x86_64/schedule/syscall.h>

u64 syscall_rsvd(syscall_args a) {return 0;}

void* syscall_table[] = {
    syscall_rsvd, // 0
};


extern "C" void syscall_handle(registers* r) {
    if (syscall_table[r->rax] != NULL) {
        syscall_args args;
        args.arg1 = (void*)r->rdi;
        args.arg2 = (void*)r->rsi;
        args.arg3 = (void*)r->rdx;
        args.arg4 = (void*)r->r10;
        args.arg5 = (void*)r->r8;
        args.arg6 = (void*)r->r9;
        args.r = r;
        u64(*func)(syscall_args) = syscall_table[r->rax];
        u64 res = func(args);
        r->rax = res;
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