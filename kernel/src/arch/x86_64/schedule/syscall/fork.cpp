#include <arch/x86_64/schedule/sched.h>
#include <arch/x86_64/schedule/syscall.h>

uint64_t sys_fork(syscall_frame_t *frame){
    Schedule::PAUSE();
    proc_t* proc = Schedule::ForkProcess();
    thread_t *thread = Schedule::ForkThread(proc, Schedule::this_thread(), frame);
    Schedule::Resume();
    return proc->id;
}