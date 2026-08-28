#include <syscall.h>

void PGPThread(){
    for(;;){
        sys_yield();
    }
}

// Get Ready for Parallel Graphics Processing
void RegThreadPerCpu(){
    SysInfo *SYSI = (SysInfo*)sys_sysinfo((uint64_t)0);
    if((uint64_t)SYSI < 0) return;
    for (uint64_t i = 0;i < SYSI->ncpus;i++)
        sys_thread_launch((uint64_t)PGPThread,i);
    sys_sysinfo((uint64_t)SYSI); // Release SYSI pointer to kernel
}