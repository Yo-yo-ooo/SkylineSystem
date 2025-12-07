#include <arch/x86_64/schedule/sched.h>
#include <arch/x86_64/schedule/syscall.h>
#include <klib/errno.h>
#include <elf/elf.h>
#include <mem/pmm.h>
#include <klib/algorithm/queue.h>

uint64_t sys_rt_sigaction(uint64_t signum,uint64_t act,uint64_t oldact,
    GENERATE_IGN3()){
    IGNV_3();
    if (signum < 1 || signum > 64 || 
        signum == LINUX_SIGKILL || 
        signum == LINUX_SIGSTOP) {
        return -EINVAL;
    }
    proc_t *tproc = Schedule::this_proc();
    sigaction_t* action = (sigaction_t*)act;
    sigaction_t* oldaction = (sigaction_t*)oldact;
    if(oldact != NULL){
        __memcpy(oldact,&tproc->sig_handlers[signum],sizeof(sigaction_t));
    }else if (act != NULL) {
        __memcpy(&(tproc->sig_handlers[signum]), action, sizeof(sigaction_t));
    }else{
        return -EFAULT;
    }


    return 0;
}