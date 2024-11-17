#pragma once

#include "../../../klib/klib.h"
#include "../../../mem/vmm.h"
#include "../../../mem/heap.h"
#include "../../../fs/vfs.h"
#include "../interrupt/idt.h"
#include "../smp/smp.h"

enum {
  SCHED_STARTING,
  SCHED_RUNNING,
  SCHED_SLEEPING,
  SCHED_BLOCKED,
  SCHED_DEAD
};

enum {
  SCHED_IDLE,
  SCHED_KERNEL,
  SCHED_USER
};

typedef void(*signal_handler)(int);

struct process;
struct registers__;
typedef struct registers__ registers;

struct cpu_info;
typedef struct cpu_info cpu_info;

typedef struct thread{
    u64 stack_base;   // gs:0
    u64 kernel_stack; // gs:8

    u64 stack_bottom;
    u64 kstack_bottom;

    struct process* parent;
    u64 idx; // idx in proc thread list

    u64 gs;

    registers ctx;

    u64 sleeping_time;

    u8 state;
    char fxsave[512] __attribute__((aligned(16)));

    pagemap* pm;
    heap* heap_area;

    signal_handler sigs[32];
} thread;

typedef struct process {
    u64 pid;
    u64 cpu;
    u64 idx; // index inside the cpu proc list

    pagemap* pm;

    u8 type; // Idle, Kernel or User?

    vfs_node* current_dir;
    file_descriptor fds[256];
    u16 fd_idx;

    atomic_lock lock;

    char* name;

    list* threads;  // thread

    struct process* parent;

    u64 tidx; // thread index
    bool scheduled;
} process;

namespace Schedule{
    void Schedule(registers* r);

    thread* GetNextThread(process* proc);
    process* GetNextProc(cpu_info* c);

    process* NewProc(char* name, u8 type, u64 cpu, bool child);
    thread* ProcAddThread(process* proc, void* entry, bool fork);
}