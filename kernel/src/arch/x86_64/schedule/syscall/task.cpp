#include <arch/x86_64/schedule/sched.h>
#include <arch/x86_64/schedule/syscall.h>
#include <klib/errno.h>
#include <elf/elf.h>
#include <mem/pmm.h>

uint64_t sys_getpid(uint64_t ign_0, uint64_t ign_1, uint64_t ign_2, \
    uint64_t ign_3,uint64_t ign_4,uint64_t ign_5) {
    IGNORE_VALUE(ign_0);IGNORE_VALUE(ign_1);IGNORE_VALUE(ign_2);
    IGNORE_VALUE(ign_3);IGNORE_VALUE(ign_4);IGNORE_VALUE(ign_5);

    return Schedule::this_proc()->id;
}

uint64_t sys_dup(uint64_t filedesc,uint64_t ign_0, uint64_t ign_1, \
    uint64_t ign_2,uint64_t ign_3,uint64_t ign_4) {
    IGNORE_VALUE(ign_0);IGNORE_VALUE(ign_1);IGNORE_VALUE(ign_2);
    IGNORE_VALUE(ign_3);IGNORE_VALUE(ign_4);

    fd_t *fd = Schedule::this_proc()->fd_table[filedesc];
    Schedule::this_proc()->fd_table[filedesc + 1] = fd;
    Schedule::this_proc()->fd_count++;

    return Schedule::this_proc()->fd_count;
}

uint64_t sys_dup2(uint64_t filedesc,uint64_t filedesc2, uint64_t ign_0, \
    uint64_t ign_1,uint64_t ign_2,uint64_t ign_3) {
    IGNORE_VALUE(ign_0);IGNORE_VALUE(ign_1);
    IGNORE_VALUE(ign_2);IGNORE_VALUE(ign_3);

    if(filedesc == filedesc2)
        return filedesc2;
    Schedule::this_proc()->fd_table[filedesc2] = Schedule::this_proc()->fd_table[filedesc];

    return filedesc;
}

uint64_t sys_exit(uint64_t code,uint64_t ign_0, uint64_t ign_1, \
    uint64_t ign_2,uint64_t ign_3,uint64_t ign_4) {
    IGNORE_VALUE(ign_0);IGNORE_VALUE(ign_1);IGNORE_VALUE(ign_2);
    IGNORE_VALUE(ign_3);IGNORE_VALUE(ign_4);

    Schedule::Exit(code);

    return 0;
}

uint64_t sched_yield(uint64_t ign_0, uint64_t ign_1, \
    uint64_t ign_2,uint64_t ign_3,uint64_t ign_4,uint64_t ign_5){
    IGNORE_VALUE(ign_0);IGNORE_VALUE(ign_1);IGNORE_VALUE(ign_2);
    IGNORE_VALUE(ign_3);IGNORE_VALUE(ign_4);IGNORE_VALUE(ign_5);

    Schedule::Yield();
    return 0;
}
