//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* stallwatch.h — 停滞看门狗 (per-CPU 心跳 + 最后现场)
 *
 * 为解决"整机看似随机挂死, 串口在某一行中间停住, 却看不到任何线索"的问题:
 * 每个 CPU 在每次进入中断/异常/系统调用时把自己的现场记进 diag 槽, 并在
 * 持有自旋锁时记录锁地址与加锁点。任意一个 CPU 只要还在收中断/系统调用,
 * 就能发现"某个 CPU 已经 N 秒没有任何进出记录", 把它的最后现场打到串口。
 *
 * 设计约束:
 *  - 记录路径无锁、无分支预测负担 (只有几条 store), 可在关中断自旋里调用;
 *  - 报告路径只由"还活着的 CPU"执行, 用全局标志保证同一时刻只有一个报告;
 *  - 不做任何分配, 不依赖堆 (可在启动早期使用)。
 */
#pragma once

#ifndef _STALLWATCH_H
#define _STALLWATCH_H

#include <stdint.h>

#define STALL_SLOTS 8             /* 最多跟踪 8 个 CPU */
#define STALL_NONE  0xFFFFFFFFu

typedef struct stall_slot {
    volatile uint64_t seq;        /* 心跳: 每次中断/异常/系统调用入口 +1   */
    volatile uint64_t rip;        /* 进入时的 RIP (内核或用户)             */
    volatile uint64_t rsp;        /* 进入时的 RSP                          */
    volatile uint64_t rflags;     /* 进入时的 RFLAGS (看 IF)               */
    volatile uint64_t int_no;     /* 进入的中断/异常向量                   */
    volatile uint64_t is_syscall; /* 1 = 系统调用路径                      */
    volatile uint64_t lock;       /* 正在等待/持有的自旋锁地址, 0 = 无      */
    volatile uint64_t lock_site;  /* 加锁点 (调用者 RIP)                   */
    volatile uint64_t holder;     /* 持锁 CPU (最后一次成功加锁者)          */
    volatile uint64_t holder_rip; /* 持锁者加锁点                          */
    volatile uint64_t held_since; /* 本核持锁起始 (TSC 原始值, 0 = 未持锁) */
    volatile uint64_t wait_since; /* 本核开始等锁 (TSC 原始值, 0 = 未等锁) */
    volatile uint64_t wait_lock;  /* 正在等的那把锁                        */
    volatile uint64_t wait_site;  /* 等锁点                                */
    volatile uint64_t kernel_busy;/* 1 = 当前处在本核的内核上下文里         */
} stall_slot_t;

/* TSC 原始值 (看门狗用它做"持锁/等锁多久"的判据, 不依赖任何时钟校准) */
static inline uint64_t stall_tsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* 全局可见 (供 GDB/监视器直接读取) */
extern volatile stall_slot_t g_stall_slots[STALL_SLOTS];
extern volatile uint64_t g_stall_magic;

/* 当前 CPU 槽位索引 (0..STALL_SLOTS-1; SMP 未起或 GS 未就绪时退化为 0) */
uint32_t stall_cpu_index(void);

/* 中断/异常/系统调用入口: 记现场 (RIP/RSP/RFLAGS/向量)。
   注意这里**不**推进心跳 —— 心跳由调度器每次调度 (stall_note_beat) 推进。
   原因: 一个核可能长时间在用户态跑 (既不进系统调用也不被抢占到), 若把
   心跳绑在入口上, 它会误报"这个核 3 秒没动"。 */
static inline void stall_note_entry(uint64_t rip, uint64_t rsp,
                                    uint64_t rflags, uint64_t int_no,
                                    uint64_t syscall) {
    uint32_t me = stall_cpu_index();
    stall_slot_t *s = (stall_slot_t *)&g_stall_slots[me];
    s->rip = rip;
    s->rsp = rsp;
    s->rflags = rflags;
    s->int_no = int_no;
    s->is_syscall = syscall;
}

/* 心跳: 每个核只要还在被调度 (timer tick / 让出 CPU) 就推进。
   看门狗据此判断"这个核是不是真的停了"。 */
static inline void stall_note_beat(void) {
    stall_slot_t *s = (stall_slot_t *)&g_stall_slots[stall_cpu_index()];
    __atomic_add_fetch(&s->seq, 1, __ATOMIC_RELEASE);
}

/* 内核上下文标记: 进入系统调用/异常处理时置 1, 退出时清 0。
   看门狗只对"卡在内核里"的核报警 —— 一个空闲的 AP 心跳也不动, 但它是
   正常的, 不该被点名。 */
static inline void stall_set_kernel_busy(uint64_t busy) {
    stall_slot_t *s = (stall_slot_t *)&g_stall_slots[stall_cpu_index()];
    s->kernel_busy = busy;
}

/* 自旋锁: 记录待锁现场 (加锁前), 成功后清空。
   每个函数只查一次 cpu 下标, 同一段记录的字段落在同一个槽 —— 否则
   "记录等待"与"记录获得"会写进不同槽位, 留下永久为真的 hold 字段,
   看门狗就会打出跨核拼接的垃圾现场 (实测踩过)。 */
static inline void stall_note_lock_wait(uint64_t lock_addr, uint64_t site) {
    uint32_t me = stall_cpu_index();
    stall_slot_t *s = (stall_slot_t *)&g_stall_slots[me];
    s->lock = lock_addr;
    s->lock_site = site;
    s->wait_lock = lock_addr;
    s->wait_site = site;
    if (s->wait_since == 0) s->wait_since = stall_tsc();
}

static inline void stall_note_lock_acquired(uint64_t lock_addr, uint64_t site) {
    uint32_t me = stall_cpu_index();
    stall_slot_t *s = (stall_slot_t *)&g_stall_slots[me];
    s->lock = 0;
    s->lock_site = 0;
    s->wait_since = 0;
    s->wait_lock = 0;
    s->wait_site = 0;
    s->holder = me;
    s->holder_rip = site;
    s->held_since = stall_tsc();
    (void)lock_addr;
}

static inline void stall_note_lock_released(void) {
    stall_slot_t *s = (stall_slot_t *)&g_stall_slots[stall_cpu_index()];
    s->lock = 0;
    s->lock_site = 0;
    s->wait_since = 0;
    s->wait_lock = 0;
    s->wait_site = 0;
    s->holder = STALL_NONE;
    s->holder_rip = 0;
    s->held_since = 0;
}

/* 看门狗: 由时钟中断/调度向量定期调用 (见 stallwatch.cpp) */
void stall_watch_tick(void);

#endif
