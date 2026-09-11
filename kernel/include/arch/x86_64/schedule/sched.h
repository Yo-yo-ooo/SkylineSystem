//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <klib/klib.h>
#include <arch/x86_64/interrupt/idt.h>
#include <arch/x86_64/smp/smp.h>
#include <klib/algorithm/rbtree.h> 

#define user_access_begin()  asm volatile("stac" ::: "memory")
#define user_access_end()    asm volatile("clac" ::: "memory")
struct user_access_guard {
    user_access_guard()  { asm volatile("stac" ::: "memory"); }
    ~user_access_guard() { asm volatile("clac" ::: "memory"); }
};

extern "C++" {

#define SCHED_VEC 48
#define SCHED_PREEMPTION_MAX 16
#define THREAD_INIT     0   // memset 默认值: 构造中, 任何路径不得调度/投递
#define THREAD_RUNNING  1
#define THREAD_BLOCKED  2
#define THREAD_SLEEPING 3
#define THREAD_TRANSFER 4
#define THREAD_ZOMBIE   5   


typedef struct proc_t proc_t;
#include <fs/fd.h>
typedef struct thread_t {
    /* ==== ABI 前缀: 汇编依赖 RAX+0 / RAX+8, 不得在此之前插入字段 ==== */
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
    uint64_t sig_stack;
    pagemap_t *pagemap;
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

    uint64_t timer_wakeup;
    thread_t* timer_next;
    thread_t* timer_prev;
    thread_t** timer_bucket;

    bool IsForkThread;

    uint64_t wait_ticks;
    uint64_t tls_base;
    uint64_t tls_pages;

    uint64_t custom_quantum;

    uint32_t timer_cpu;

    /* ==== EEVDF 公平层 ==== */
    uint64_t vruntime;        // 虚拟运行时间
    uint64_t deadline;        // 虚拟截止时间
    uint64_t last_run_time;   // 上次运行的实际时间
    uint32_t weight;          // 线程权重 (由 priority 转换)
    rb_node_t rb_node;        // 红黑树节点，挂入 CPU 运行队列
    bool on_rq;               // 是否在运行队列红黑树中
    uint64_t vruntime_rem;    // 整型除法余数进位 (长跑精确)
    uint64_t min_vruntime_subtree; // 子树最小 vruntime (Pick 加速增广)
    struct thread_t *zombie_next;

    /* ==== RIP 速率反馈 ====
     * 排序: 7×u64 + 4 + 1 + 3 = 64B 连续块 — 每次采样写的字段挤在
     * 1~2 个 cache line 内. 全部依赖零初始化 (memset 0): base/
     * mult/tag 为 0 时由防御路径接管, 无需显式构造. Fork 整体
     * memcpy 也安全: mult/基线继承父线程是合理初值, tag/tsc/
     * dispatch_rip 在 dispatch 处重置, 陈旧值无影响.
     * rip_last_sample_ms 刻意不在 dispatch 时重置 — 离 CPU 时长
     * 必须进入老化计算. */
    uint64_t rip_base_self;       /* per-thread 慢基线 (1/64 EWMA); 0 = 未建 */
    uint64_t rip_mult_fast;       /* 快通道 1/4;  0 = 视作 1.0x */
    uint64_t rip_mult_slow;       /* 慢通道 1/16; 0 = 视作 1.0x */
    uint64_t rip_signal_tag;      /* pagemap ^ (cs&3); 0 = 未设置 → 首采样重臂 */
    uint64_t rip_last_tsc;        /* 上次采样 TSC (执行期分母基准) */
    uint64_t dispatch_rip;        /* 窗口起点 RIP 快照 (逐采样重臂) */
    uint64_t rip_last_sample_ms;  /* 上次采样时刻 (老化计时) */
    int32_t  rip_quantum_adj;     /* 有符号修正项, ±4 轨道 */
    uint8_t  rip_outlier_streak;  /* 连续离群计数 (≥4 → 基线重置) */
    char     rip_pad[3];
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
    volatile int32_t exiting;
    bool IsTrusted;
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

#define THREAD_QUEUE_CNT 16

extern rb_sharded_root_t res_tree;
cpu_t *get_lw_cpu(cpu_t *ref_cpu = nullptr);

static inline uint64_t irq_save() {
    uint64_t flags;
    asm volatile("pushfq\n\tcli\n\tpop %0" : "=r"(flags) :: "memory");
    return flags;
}

static inline void irq_restore(uint64_t flags) {
    asm volatile("push %0\n\tpopfq" :: "r"(flags) : "memory");
}

static inline uint64_t spin_lock_irqsave(spinlock_t *lock) {
    uint64_t flags = irq_save();
    spinlock_lock(lock);
    return flags;
}

static inline void spin_unlock_irqrestore(spinlock_t *lock, uint64_t flags) {
    spinlock_unlock(lock);
    irq_restore(flags);
}

static inline bool spin_trylock(spinlock_t *lock) {
    return __sync_bool_compare_and_swap(lock, 0, 1);
}
static inline bool spin_trylock_irqsave(spinlock_t *lock, uint64_t *flags) {
    *flags = irq_save();
    if (spin_trylock(lock)) return true;
    irq_restore(*flags);
    return false;
}


namespace Schedule{
    extern uint64_t procl_count;
    extern procl_t *sched_proclist;
    void DrainProcZombieList(cpu_t *cpu);

    namespace Internal{
        void Switch(context_t *ctx);
        void Preempt(context_t *ctx);

        void ProcessAddThread(proc_t *parent, thread_t *thread);
        
        void RemoveFromQueue(cpu_t *cpu, thread_t *thread);
        void InsertToQueue(cpu_t *cpu, thread_t *thread);
        thread_t *Pick(cpu_t *cpu);

        void TimerRemove(thread_t* t);
        void TimerAdd(cpu_t* cpu, thread_t* t, uint64_t expires);
        void TimerCascade(cpu_t* cpu, thread_t** tv, int idx);
    }
        /* 反馈统计导出. out 布局 (计数; [10]/[11] 为 Q10 定点):
     *  [0]samples [1]outliers [2]tag_invalid [3]short_windows
     *  [4]stalled [5]dead_zone [6]sat_hi [7]sat_lo [8]base_resets
     *  [9]tsc_mismatch [10]mean_abs_dev [11]fast_weight
     *  [12]near_reset [13]wf_clamped */
    bool GetRipStats(uint32_t cpu_id, uint64_t out[14]);

    namespace Signal{
        int32_t Raise(proc_t *process, int32_t signal);
        void DefaultHandler(int32_t signal);
    }

    void Init();
    void Install();

    proc_t *NewProcess(bool user,bool Trusted = true);
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
    void WaitForThreadOffCpu(thread_t *thread);
    void KillThread(thread_t *thread);
    void TriggerPreempt(thread_t *woken_thread);
}

}