#include <arch/x86_64/signal.h>
#include <arch/x86_64/syscall.h>
#include <arch/x86_64/unsiteds.h>

int sigaction(int signum,const struct sigaction * restrict act, \
struct sigaction * restrict oldact){

    int ret;
    syscall3(__NR_rt_sigaction, 
        (uint64_t)signum, 
        (uint64_t)act, 
        (uint64_t)oldact,ret
    );
    return ret;
}