#pragma once
#ifndef _X86_64_SCHEDULE_SIGNAL_H
#define _X86_64_SCHEDULE_SIGNAL_H

#include <stdint.h>
#include <stddef.h>
#include <klib/types.h>

typedef union sigval {
	int sival_int;
	void *sival_ptr;
} sigval_t;

typedef struct siginfo_t {
    int32_t      si_signo;     /* Signal number 信号编号 */
    int32_t      si_errno;     /* An errno value 如果为非零值则错误代码与之关联 */
    int32_t      si_code;      /* Signal code 说明进程如何接收信号以及从何处收到*/
    int32_t      si_trapno;    /* Trap number that caused
                                hardware-generated signal
                                (unused on most architectures) */
    pid_t    si_pid;       /* Sending process ID适用于SIGCHLD，代表被终止进程的PID  */
    uid_t    si_uid;       /* Real user ID of sending process适用于SIGCHLD,代表被终止进程所拥有进程的UID  */
    int32_t      si_status;    /* Exit value or signal 适用于SIGCHLD，代表被终止进程的状态 */
    clock_t  si_utime;     /* User time consumed 适用于SIGCHLD，代表被终止进程所消耗的用户时间 */
    clock_t  si_stime;     /* System time consumed 适用于SIGCHLD，代表被终止进程所消耗系统的时间 */
    sigval_t si_value;     /* Signal value */
    int32_t      si_int;       /* POSIX.1b signal */
    void    *si_ptr;       /* POSIX.1b signal */
    int32_t      si_overrun;   /* Timer overrun count;
                                POSIX.1b timers */
    int32_t      si_timerid;   /* Timer ID; POSIX.1b timers */
    void    *si_addr;      /* Memory location which caused fault */
    int32_t     si_band;      /* Band event (was int32_t in
                                glibc 2.3.2 and earlier) */
    int32_t      si_fd;        /* File descriptor */
    int16_t    si_addr_lsb;  /* Least significant bit of address
                                (since Linux 2.6.32) */
    void    *si_call_addr; /* Address of system call instruction
                                (since Linux 3.5) */
    int32_t      si_syscall;   /* Number of attempted system call
                                (since Linux 3.5) */
    uint32_t si_arch;  /* Architecture of attempted system call
                                (since Linux 3.5) */
}siginfo_t;

typedef struct {
    uint32_t sig[1024 / 64];
} sigset_t;

typedef struct sigaction_t{
    void (*handler)(int);
    void (*sa_sigaction)(int32_t, siginfo_t *, void *);
    sigset_t sa_mask;
    uint32_t sa_flags;
    void (*sa_restorer)(void);
} sigaction_t;


#endif// _X86_64_SCHEDULE_SIGNAL_H