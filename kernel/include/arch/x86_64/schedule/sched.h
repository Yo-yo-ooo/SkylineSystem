//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <klib/klib.h>
#include <arch/x86_64/interrupt/idt.h>
#include <arch/x86_64/smp/smp.h>
#include <klib/algorithm/rbtree.h> 

extern "C++" {

#define SCHED_VEC 48
#define THREAD_ZOMBIE 0
#define THREAD_RUNNING 1
#define THREAD_BLOCKED 2
#define THREAD_SLEEPING 3
#define TFLAGS_WAITING 1
#define SCHED_PREEMPTION_MAX 16

typedef struct proc_t proc_t;
#include <fs/fd.h>

typedef struct thread_t {
    uint64_t thread_stack; // RAX+0
    uint64_t kernel_rsp;   // RAX+8

    uint64_t id;
    uint32_t cpu_num;
    uint32_t priority;
    uint32_t preempt_count;
    uint64_t kernel_stack;
    int32_t state;
    uint64_t stack;
    context_t ctx;
    uint64_t fs;
    bool user;
    uint64_t sig_deliver;
    uint64_t sig_mask;
    context_t sig_ctx;
    uint64_t sig_stack;
    uint64_t sig_fs;
    volatile pagemap_t *pagemap;
    uint64_t exit_code;
    uint64_t flags;
    uint64_t waiting_status;
    char *fx_area;
    struct thread_t *next;
    struct thread_t *prev;
    struct thread_t *list_next;
    struct thread_t *list_prev;
    struct proc_t *parent;
    uint64_t wakeup_tick;

    bool IsTrusted;
    
    uint64_t timer_wakeup;       
    thread_t* timer_next;        
    thread_t* timer_prev;        
    thread_t** timer_bucket;     

    bool IsForkThread;

    uint64_t wait_ticks;         
    uint64_t tls_base;           
    uint64_t tls_pages;          

    uint64_t custom_quantum; 

    // ==========================================
    // RA-MLFQ 资源感知专用字段
    // ==========================================
    int64_t held_resource_id;      
    int64_t requested_resource_id; 
    int32_t original_priority;     
    struct thread_t *res_wait_next; 
    struct thread_t *res_wait_prev; 
} thread_t;

typedef struct proc_t {
    uint64_t id;
    thread_t *threads;
    pagemap_t *pagemap;
    struct proc_t *parent;
    struct proc_t *children;
    struct proc_t *sibling;
    int32_t fd_count;
    fd_manager_t *FDMan;
} proc_t;

typedef struct procl{
    proc_t *proc;
} procl_t;

// 资源节点包装器
typedef struct KernelResource {
    rb_node_t node;          
    int64_t res_id;          
    volatile thread_t *owner;
    thread_t *wait_head;     
} KernelResource_t;

extern rb_sharded_root_t res_tree;

namespace Schedule{
    extern uint64_t procl_count;
    extern procl_t *sched_proclist;

    namespace Internal{
        void Switch(context_t *ctx);
        void Preempt(context_t *ctx);

        void ProcessAddThread(proc_t *parent, thread_t *thread);
        
        void RemoveFromQueue(cpu_t *cpu, thread_t *thread);
        void InsertToQueue(cpu_t *cpu, thread_t *thread);
        void Demote(cpu_t *cpu, thread_t *thread);
        void Promote(cpu_t *cpu, thread_t *thread);
        thread_t *Pick(cpu_t *cpu);

        void TimerRemove(thread_t* t);
        void TimerAdd(cpu_t* cpu, thread_t* t, uint64_t expires);
        void TimerCascade(cpu_t* cpu, thread_t** tv, int idx);
    }

    namespace Signal{
        int32_t Raise(proc_t *process, int32_t signal);
        void DefaultHandler(int32_t signal);
    }

    void Init();
    void Install();

    proc_t *NewProcess(bool user);
    void PrepareUserStack(thread_t *thread, int32_t argc, char *argv[], char *envp[]);
    thread_t *NewKernelThread(proc_t *parent, uint32_t cpu_num, int32_t priority, void *entry);
    thread_t *NewThread(proc_t *parent, uint32_t cpu_num, int32_t priority, const char *Path, int32_t argc, char *argv[], char *envp[]);
    thread_t *ForkThread(proc_t *proc, thread_t *parent, void *frame);
    proc_t *ForkProcess();
    thread_t *this_thread();
    proc_t *this_proc();
    void Exit(int32_t code);
    void Yield();
    void PAUSE();
    void Tick();
    void Resume();

    void DeleteThread(cpu_t *cpu, thread_t *thread);
    void DeleteProc(proc_t *proc);
    void FreeThreadResources(thread_t *thread);
    void PROC_KILL(proc_t *proc);

    bool AcquireResource(int64_t res_id);
    void ReleaseResource(int64_t res_id);
    void InitResourceTable();
}

}