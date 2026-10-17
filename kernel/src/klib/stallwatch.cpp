//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* stallwatch.cpp — 停滞看门狗实现 (见 include/klib/stallwatch.h) */
#include <klib/stallwatch.h>
#include <klib/kio.h>
#include <klib/kprintf.h>
#include <stdint.h>

typedef struct context_t context_t;      /* smp.h 里 interrupt_handler_t 需要 */
#include <arch/x86_64/smp/smp.h>         /* cpu_t 完整定义 (只用于取 id) */

volatile stall_slot_t g_stall_slots[STALL_SLOTS];
volatile uint64_t g_stall_magic = 0x5354414C4C574154ULL;   /* "STALLWAT" */

struct cpu_t;                 /* 前向声明, 定义来自上面的 smp.h */
extern "C" cpu_t *this_cpu();

uint32_t stall_cpu_index(void) {
    cpu_t *cpu = this_cpu();
    if (!cpu) return 0;
    uint32_t id = cpu->id;
    return (id < STALL_SLOTS) ? id : 0;
}

/* ---- 看门狗状态 (只在报告路径使用, 无锁近似即可) ---- */
#define STALL_STALE_MS   3000     /* 多久没有心跳算停滞               */
#define STALL_REPORT_MS  4000     /* 同一批停滞的最小重复报告间隔     */
#define STALL_SETTLE_MS  4000     /* 启动后静默期 (SMP 预热)          */
#define STALL_STACK_QWORDS 192    /* 扫描的栈字数 (1.5KB)             */
/* 持锁/等锁超过这个 TSC 时长就报"锁停滞"。取 3e9 ≈ 1 秒 @3GHz:
   正常临界区都是微秒级, 谁持锁一秒以上必然是死锁或病态长临界区。 */
#define STALL_LOCK_TSC_LIMIT 3000000000ULL
/* 槽位新鲜度: 超过这个时间没更新的槽位, 其持锁/等锁字段都是老快照, 不采信 */
#define STALL_FRESH_MS   2000

static uint32_t g_watch_started = 0;
static uint64_t g_watch_t0_ms    = 0;
static uint64_t g_watch_last_ms  = 0;
static uint64_t g_watch_seq[STALL_SLOTS];
static uint64_t g_watch_slot_ms[STALL_SLOTS];   /* 该槽位最后一次更新时刻 */
static uint64_t g_watch_hb_seq[STALL_SLOTS];    /* 心跳判据专用 (与上面分开) */
static uint64_t g_watch_stale_since[STALL_SLOTS];
static uint64_t g_watch_last_report = 0;
static uint32_t g_watch_reports = 0;

static inline uint64_t now_ms(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    uint64_t tsc = ((uint64_t)hi << 32) | lo;
    /* TSC 频率未知时用 3GHz 近似 —— 只用于限流, 不影响判定语义 */
    return tsc / 3000000ULL;
}

/* 判定一个值是否像内核代码地址 (用于从栈里捞返回地址) */
static inline bool looks_like_kernel_text(uint64_t v) {
    return v >= 0xFFFFFFFF80000000ULL && v < 0xFFFFFFFFC0000000ULL;
}

static void dump_stack_heuristic(uint64_t rsp) {
    if (!looks_like_kernel_text(rsp) || (rsp & 0x7)) return;
    kerrorln("  stack scan from %p (kernel frames only):", (void *)rsp);
    uint64_t *sp = (uint64_t *)rsp;
    int printed = 0;
    for (int i = 0; i < STALL_STACK_QWORDS && printed < 12; i++) {
        uint64_t v;
        /* 直接读: 这里是内核栈, 映射必然有效; 越界读由 i 上界兜住 */
        v = sp[i];
        if (looks_like_kernel_text(v)) {
            kerrorln("    [%3d] %p", i, (void *)v);
            printed++;
        }
    }
    if (!printed) kerrorln("    (no kernel return addresses found)");
}

/* 单个核的现场 */
static void dump_one_cpu(uint32_t i, stall_slot_t *s) {
    kerrorln("  last entry : rip=%p rsp=%p rflags=%p vec=%llu %s",
             (void *)s->rip, (void *)s->rsp, (void *)s->rflags,
             (unsigned long long)s->int_no,
             s->is_syscall ? "(syscall)" : "(interrupt/exception)");
    kerrorln("  interrupts : %s", (s->rflags & 0x200) ? "ENABLED" : "DISABLED");
    if (s->wait_lock) {
        kerrorln("  waiting on : lock=%p (acquired at %p)",
                 (void *)s->wait_lock, (void *)s->wait_site);
    } else if (s->held_since) {
        kerrorln("  holding    : lock acquired at %p", (void *)s->holder_rip);
    } else {
        kerrorln("  waiting on : no lock recorded (not blocked in spinlock)");
    }
    if (s->holder != STALL_NONE && s->holder != i) {
        kerrorln("  last lock  : cpu %llu holds it (site %p)",
                 (unsigned long long)s->holder, (void *)s->holder_rip);
    }
    dump_stack_heuristic(s->rsp);
}

/* 整机全景: 一次把每个核的状态都打出来 —— 锁死锁的分析必须看到"所有"核
   在等什么/持什么, 只看一个核永远拼不出环。 */
static void dump_all_cpus(uint32_t focus) {
    uint64_t tsc = stall_tsc();
    (void)focus;
    for (uint32_t i = 0; i < STALL_SLOTS; i++) {
        stall_slot_t *s = (stall_slot_t *)&g_stall_slots[i];
        if (s->seq == 0) continue;
        kerrorln("  --- CPU %u ---", i);
        if (s->held_since)
            kerrorln("  holds      : site %p for %llu TSC ticks",
                     (void *)s->holder_rip,
                     (unsigned long long)(tsc - s->held_since));
        if (s->wait_since)
            kerrorln("  waits for  : lock %p (site %p) for %llu TSC ticks",
                     (void *)s->wait_lock, (void *)s->wait_site,
                     (unsigned long long)(tsc - s->wait_since));
        dump_one_cpu(i, s);
    }
    kerrorln("================================================================");
}

/* 是否有别的核正卡在某把锁上 (等锁记录新鲜且确实在等) —— 配合"持锁太久"
   判据用: 没人等的长临界区不值得报警。 */
static bool anyone_waiting_for(uint32_t holder) {
    for (uint32_t i = 0; i < STALL_SLOTS; i++) {
        if (i == holder) continue;
        stall_slot_t *s = (stall_slot_t *)&g_stall_slots[i];
        if (s->seq == 0 || !s->wait_since) continue;
        if (g_watch_slot_ms[i] == 0) continue;
        if (s->holder == holder) return true;
    }
    return false;
}

void stall_watch_tick(void) {
    uint64_t now = now_ms();
    if (!g_watch_started) {
        g_watch_started = 1;
        g_watch_t0_ms = now;
        for (uint32_t i = 0; i < STALL_SLOTS; i++) {
            g_watch_seq[i] = g_stall_slots[i].seq;
            g_watch_hb_seq[i] = g_stall_slots[i].seq;
            g_watch_slot_ms[i] = now;
            g_watch_stale_since[i] = 0;
        }
        return;
    }
    if (now - g_watch_t0_ms < STALL_SETTLE_MS) return;
    /* 限流: 每 250ms 检查一次足够 */
    if (now - g_watch_last_ms < 250) return;
    g_watch_last_ms = now;
    if (now - g_watch_last_report < STALL_REPORT_MS) return;
    if (g_watch_reports >= 20) return;          /* 别把串口刷爆 */

    /* ---- (1) 锁停滞: 谁持锁/等锁太久 -------------------------------------
       这是实测最常见、也最致命的形态: 一个核在 spinlock_lock 里永旋
       (例如等 ptf_lock 打印锁), 而持锁者又卡在别处 ->
       整机输出停在一行中间, 但所有核的心跳都还在, 老的"心跳停滞"判据
       完全看不到。判据用 TSC 原始值, 不依赖任何时钟校准。

       新鲜度门槛: 只采信最近 STALL_FRESH_MS 内更新过的槽位 —— 一个核如果
       长时间不再进中断/系统调用, 它槽里的持锁字段就是老快照, 拿它当
       "有人持锁 N 秒"会误报 (实测踩过)。 */
    uint64_t tsc = stall_tsc();
    for (uint32_t i = 0; i < STALL_SLOTS; i++) {
        stall_slot_t *s = (stall_slot_t *)&g_stall_slots[i];
        if (s->seq == 0) continue;
        uint64_t seq = s->seq;
        if (seq != g_watch_seq[i]) {
            g_watch_seq[i] = seq;
            g_watch_slot_ms[i] = now;
        }
        if (now - g_watch_slot_ms[i] > STALL_FRESH_MS) continue;  /* 老快照 */

        if (s->held_since && tsc - s->held_since > STALL_LOCK_TSC_LIMIT) {
            /* 持锁太久: 只有当**有别的核正在等这把锁**时才算真问题 ——
               否则只是某段长临界区, 报出来反而误导。 */
            if (!anyone_waiting_for(i)) continue;
            g_watch_last_report = now;
            g_watch_reports++;
            kerrorln("");
            kerrorln("========== LOCK STALL WATCHDOG: CPU %u has held a lock "
                     "for %llu TSC ticks (acquired at site %p) while another "
                     "CPU waits ==========",
                     i, (unsigned long long)(tsc - s->held_since),
                     (void *)s->holder_rip);
            dump_all_cpus(i);
            return;
        }
        if (s->wait_since && tsc - s->wait_since > STALL_LOCK_TSC_LIMIT) {
            /* 等锁太久: 必须能指出这把锁被谁持着, 且那个核的槽位也是新鲜的
               (否则是启动早期/长期不进中断留下的老快照, 不报)。 */
            uint64_t h = s->holder;
            if (h >= STALL_SLOTS || h == i) continue;
            stall_slot_t *hs = (stall_slot_t *)&g_stall_slots[h];
            if (hs->seq == 0) continue;
            if (now - g_watch_slot_ms[h] > STALL_FRESH_MS) continue;
            if (!hs->held_since) continue;

            g_watch_last_report = now;
            g_watch_reports++;
            kerrorln("");
            kerrorln("========== LOCK STALL WATCHDOG: CPU %u has been waiting "
                     "for lock %p (site %p) for %llu TSC ticks; CPU %llu has "
                     "held it for %llu ticks (site %p) ==========",
                     i, (void *)s->wait_lock, (void *)s->wait_site,
                     (unsigned long long)(tsc - s->wait_since),
                     (unsigned long long)h,
                     (unsigned long long)(tsc - hs->held_since),
                     (void *)hs->holder_rip);
            dump_all_cpus(i);
            return;
        }
    }

    /* ---- (2) 心跳停滞: 某个核很久没有进调度 -----------------------------
       前提: 该槽位必须是"最近还在用"的 —— 一个从启动后不久就再没进过
       调度的核 (空闲 AP), 它槽里的现场是几十分钟前的老快照, 报出来只会
       误导。判据: 槽位首见时间在最近 STALL_FRESH_MS*2 内。 */
    uint32_t me = stall_cpu_index();
    for (uint32_t i = 0; i < STALL_SLOTS; i++) {
        if (i == me) continue;
        uint64_t seq = g_stall_slots[i].seq;
        if (seq == 0) continue;                 /* 该槽位从未用过 (无此核) */
        if (seq != g_watch_hb_seq[i]) {         /* 有进展 */
            g_watch_hb_seq[i] = seq;
            g_watch_stale_since[i] = 0;
            continue;
        }
        /* 只关心"卡在内核里"的核: 空闲 AP 心跳同样不动, 但它没问题 */
        if (!g_stall_slots[i].kernel_busy) {
            g_watch_stale_since[i] = 0;
            continue;
        }
        if (g_watch_stale_since[i] == 0) {      /* 第一次发现停住 */
            /* 只有当该核在我们开始观察之后还在更新时, 才把"停住"当回事 */
            if (now - g_watch_slot_ms[i] > STALL_FRESH_MS) continue;
            g_watch_stale_since[i] = now;
            continue;
        }
        if (now - g_watch_stale_since[i] < STALL_STALE_MS) continue;

        g_watch_last_report = now;
        g_watch_reports++;

        stall_slot_t *s = (stall_slot_t *)&g_stall_slots[i];
        kerrorln("");
        kerrorln("========== STALL WATCHDOG: CPU %u has not run for %llu ms "
                 "(reported by CPU %u) ==========",
                 i, (unsigned long long)(now - g_watch_stale_since[i]), me);
        dump_one_cpu(i, s);
        kerrorln("================================================================");
    }
}

/* 供其它子系统查询 (诊断用) */
uint64_t stall_waited_lock(void) {
    stall_slot_t *s = (stall_slot_t *)&g_stall_slots[stall_cpu_index()];
    return s->wait_lock;
}
