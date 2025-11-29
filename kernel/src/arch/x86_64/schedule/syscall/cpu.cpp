#include <arch/x86_64/schedule/sched.h>
#include <arch/x86_64/schedule/syscall.h>
#include <klib/errno.h>
#include <elf/elf.h>
#include <mem/pmm.h>
#include <klib/kio.h>
#include <arch/x86_64/asm/prctl.h>

uint64_t sys_arch_prctl(uint64_t op, uint64_t extra,uint64_t ign_0, uint64_t ign_1, \
    uint64_t ign_2,uint64_t ign_3) {
    IGNORE_VALUE(ign_0);IGNORE_VALUE(ign_1);IGNORE_VALUE(ign_2);
    IGNORE_VALUE(ign_3);
    uint64_t temp=0;
    uint64_t *pdpt = (uint64_t*)Schedule::this_thread()->pagemap->pml4[PML4E(extra)];
    if (!PAGE_EXISTS(pdpt)) 
        temp = EFAULT;
    switch (op) {
        case ARCH_SET_FS:
            wrmsr(FS_BASE, extra);
            break;
        case ARCH_SET_GS:
            wrmsr(GS_BASE,extra);
            break;
        case ARCH_GET_FS:
            temp = rdmsr(FS_BASE);
            break;
        case ARCH_GET_GS:
            temp = rdmsr(GS_BASE);
            break;
        case ARCH_GET_CPUID:
        case ARCH_SET_CPUID:
        case ARCH_GET_XCOMP_SUPP:
        case ARCH_GET_XCOMP_PERM:
        case ARCH_REQ_XCOMP_PERM:
        case ARCH_GET_XCOMP_GUEST_PERM:
        case ARCH_REQ_XCOMP_GUEST_PERM:
            break;
        default:
            kerror("[sys_arch_prctl]: %d not implemented.\n", op);
            return -EINVAL;
            break;
    }
    return temp;
}
