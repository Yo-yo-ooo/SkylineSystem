//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <klib/klib.h>

#include <arch/x86_64/interrupt/idt.h>
#include <arch/x86_64/smp/smp.h>
#include <arch/x86_64/schedule/signal.h>

extern "C++" {

#define SCHED_VEC 48

#define THREAD_ZOMBIE 0
#define THREAD_RUNNING 1
#define THREAD_BLOCKED 2
#define THREAD_SLEEPING 3

#define TFLAGS_WAITING 1
// 移除 TFLAGS_PREEMPTED，不再需要人工补偿标志

#define SCHED_PREEMPTION_MAX 16

typedef struct proc_t proc_t;
#include <fs/fd.h>

typedef struct thread_t {
    uint64_t thread_stack; // RAX+0
    uint64_t kernel_rsp;   // RAX+8

    uint64_t id;
    uint32_t cpu_num;
    uint32_t priority;
    uint32_t preempt_count; // 用于时间片耗尽降级计数
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
    struct thread_t *list_next; // In the cpu thread list
    struct thread_t *list_prev;
    struct proc_t *parent;
    uint64_t wakeup_tick;

    bool IsTrusted;
    
    // 时间轮专用字段
    uint64_t timer_wakeup;       // 绝对唤醒时间
    thread_t* timer_next;        // 时间轮链表后继
    thread_t* timer_prev;        // 时间轮链表前驱
    thread_t** timer_bucket;     // 记录自己挂在哪个桶里（用于 O(1) 删除）

    bool IsForkThread;

    uint64_t wait_ticks;         // 用于 Aging 老化机制，记录在当前优先级的等待 tick 数
    uint64_t tls_base;           // TLS 区域起始虚拟地址，用于线程销毁时释放
    uint64_t tls_pages;          // TLS 区域占用的页数
} thread_t;

typedef struct proc_t {
    uint64_t id;
    sigaction_t sig_handlers[64];
    thread_t *threads;
    pagemap_t *pagemap;
    struct proc_t *parent; // In case of fork
    struct proc_t *children;
    struct proc_t *sibling;
    int32_t fd_count;
    fd_manager_t *FDMan;
} proc_t;

typedef struct procl{
    proc_t *proc;
}procl_t;

namespace Schedule{
    extern uint64_t procl_count;
    extern procl_t *sched_proclist;

    namespace Internal{
        void Switch(context_t *ctx);
        void Preempt(context_t *ctx);

        void ProcessAddThread(proc_t *parent, thread_t *thread);
        
        // 统一的队列维护接口
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
}

}