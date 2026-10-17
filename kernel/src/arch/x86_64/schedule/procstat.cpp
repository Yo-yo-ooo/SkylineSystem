//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* procstat.cpp — 进程存活查询 + 桌面进程 id (诊断用)
 *
 * 背景: 用户看到的现象是"PROC 7 自己退了, 然后什么都没有了"。
 * 内核里原本没有"某个 pid 现在还在不在"的统一查询入口, 诊断线程
 * (lwIP 每 10 秒的统计节拍) 需要它来周期性地确认窗口管理器还在不在 ——
 * 让"桌面已死"变成日志里一眼可见的事实。
 */
#include <arch/x86_64/schedule/sched.h>
#include <klib/algorithm/art.h>
#include <stdint.h>

int32_t g_desktop_pid = -1;

extern spinlock_t PID2PROC_TREE_LOCK;
extern art_tree *pid2proc_tree;

/* pid 是否仍在进程表里 (exiting 已置位的视为不在) */
bool proc_alive(uint64_t pid) {
    if (!pid2proc_tree) return false;
    uint64_t flags = spin_lock_irqsave(&PID2PROC_TREE_LOCK);
    proc_t *p = (proc_t *)art_search(pid2proc_tree, (const uint8_t *)&pid, 8);
    bool alive = (p != nullptr) &&
                 __atomic_load_n(&p->exiting, __ATOMIC_ACQUIRE) == 0;
    spin_unlock_irqrestore(&PID2PROC_TREE_LOCK, flags);
    return alive;
}
