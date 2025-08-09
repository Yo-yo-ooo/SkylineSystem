#pragma once

#include <klib/klib.h>

//#include <mem/heap.h>
#include <fs/vfs.h>

#include <arch/x86_64/interrupt/idt.h>
#include <arch/x86_64/smp/smp.h>

#define SCHED_VEC 48

#define THREAD_ZOMBIE 0
#define THREAD_RUNNING 1
#define THREAD_BLOCKED 2
#define THREAD_SLEEPING 3

#define TFLAGS_WAITING4 1
#define TFLAGS_PREEMPTED 2

#define SCHED_PREEMPTION_MAX 16

typedef struct proc_t proc_t;

typedef struct {
    vfs_inode_t *node;
    size_t off;
    int32_t flags;
} fd_t;

typedef struct {
    unsigned long sig[1024 / 64];
} sigset_t;

typedef struct {
    void *handler;
    unsigned long sa_flags;
    void (*sa_restorer)(void);
    sigset_t sa_mask;
} sigaction_t;

typedef struct thread_t {
    uint64_t thread_stack; // GS+0
    uint64_t kernel_rsp;   // GS+8

    uint64_t id;
    uint32_t cpu_num;
    uint32_t priority;
    uint32_t preempt_count;
    uint64_t kernel_stack;
    int state;
    uint64_t stack;
    context_t ctx;
    uint64_t fs;
    bool user;
    uint64_t sig_deliver;
    uint64_t sig_mask;
    context_t sig_ctx;
    uint64_t sig_stack;
    uint64_t sig_fs;
    pagemap_t *pagemap;
    uint64_t exit_code;
    uint64_t flags;
    uint64_t waiting_status;
    char *fx_area;
    struct thread_t *next;
    struct thread_t *prev;
    struct thread_t *list_next; // In the cpu thread list
    struct thread_t *list_prev;
    struct proc_t *parent;
} thread_t;

typedef struct proc_t {
    uint64_t id;
    sigaction_t sig_handlers[64];
    vfs_inode_t *cwd;
    thread_t *threads;
    pagemap_t *pagemap;
    struct proc_t *parent; // In case of fork
    struct proc_t *children;
    struct proc_t *sibling;
    fd_t *fd_table[256];
    int fd_count;
} proc_t;

namespace Schedule{
    extern uint64_t sched_pid;
    extern proc_t *sched_proclist[256];

    

    namespace Useless{
        extern "C"{
            void Switch(context_t *ctx);
            void Preempt(context_t *ctx);
        }

        void ProcessAddThread(proc_t *parent, thread_t *thread);
        
        void AddThread(cpu_t *cpu, thread_t *thread);

        uint8_t Demote(cpu_t *cpu, thread_t *thread);
    }

    void Init();
    void Install();

    proc_t *NewProcess(bool user);
    void PrepareUserStack(thread_t *thread, int argc, char *argv[], char *envp[]);
    thread_t *NewKernelThread(proc_t *parent, uint32_t cpu_num, int priority, void *entry);
    thread_t *NewThread(proc_t *parent, uint32_t cpu_num, int priority, vfs_inode_t *node, int argc, char *argv[], char *envp[]);
    thread_t *ForkThread(proc_t *proc, thread_t *parent, void *frame);
    proc_t *ForkProcess();
    thread_t *this_thread();
    proc_t *this_proc();
    void Exit(int code);
    void Yield();
    void PAUSE();
    void Resume();
}