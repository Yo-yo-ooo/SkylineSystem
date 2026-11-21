//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * user_init.cpp —— 内建组件 kernel.user.init（用户态进程创建）
 * ----------------------------------------------------------------------------
 * 精确迁移自 x86_64_init 的桌面/硬件进程创建块（原 init.cpp line 300-318）：
 *
 *     proc_t *proc = Schedule::NewProcess(true);
 *     kinfoln("Creating desktop process...");
 *     char *argv[] = {(char*)"Test Main Thread"}; char *envp[] = {nullptr};
 *     thread_t *desktop  = Schedule::NewThread(proc,  0, 0, "/mp/desktop.elf", 1, argv, envp);
 *     proc_t *proc2      = Schedule::NewProcess(true);
 *     thread_t *desktop2 = Schedule::NewThread(proc2, 0, 1, "/mp/hw.elf", 1, argv, envp);
 *     kinfoln("desktop PROCESS: %d", proc->id);
 *     extern int32_t g_desktop_pid; g_desktop_pid = (int32_t)proc->id;
 *
 * **本组件只做进程/线程创建**：
 *   - 原 line 319-321 的 `sti` + `LAPIC::IPI(SCHED_VEC)` + IPIOthers() 留在
 *     Lead 的 T0 启动尾部（组件 init 处于 bootstrap 上下文，不能开中断/
 *     触发调度；进程线程在 sti+kick 后才会真正运行 —— 与原顺序完全一致）；
 *   - 原 line 297-298 的 PrintFSERIAL/sys_sysinfo_init() 同样属 T0 收尾。
 *
 * 失败语义：NewProcess/NewThread 失败 = 可恢复（系统能起来但没有桌面）——
 * 记 kerror 日志并返回 KMOD_OK，不 panic、不回滚整个组件图（用户态缺失不应
 * 让 FS/网络组件一起被回滚）。原实现假定成功并直接解引用 proc->id，若失败会
 * #PF；这里是行为改进且不改变成功路径。
 * ==========================================================================*/
#include <kmod/kmod_component.h>

#include <arch/x86_64/schedule/sched.h>
#include <klib/errno.h>

static int32_t comp_user_init_init(void);
static void comp_user_init_exit(void);

KCOMPONENT_BEGIN(user_init, "kernel.user.init", "1.0.0", KMOD_TIER_POLICY,
                 KMOD_MF_BOOT,
                 (uint64_t)(KMOD_CAP_FS | KMOD_CAP_LOG | KMOD_CAP_MEM_ALLOC),
                 0, "create desktop + hw user processes (sti/IPI stays in boot tail)",
                 comp_user_init_init, comp_user_init_exit, 0, 0, 5000u, 200u)

KCOMPONENT_IMPORT_FN(user_init, kprintf_, int, (const char *, ...), "kernel",
                     "kprintf_", "kernel.log.v1")
KCOMPONENT_DEPENDS(user_init, "kernel.fs.rootfs", fs_rootfs)
KCOMPONENT_DEPENDS(user_init, "kernel.sched.install", sched_install)
KCOMPONENT_DEPENDS(user_init, "kernel.display.fb", display_fb)

namespace {

bool g_desktop_created = false;
int32_t g_desktop_pid_cached = -1;

} /* namespace */

/* ==================================================================== */
/* 生命周期                                                              */
/* ==================================================================== */
static int32_t comp_user_init_init(void)
{
    if (g_desktop_created)
        return KMOD_OK; /* 幂等 */

    kprintf_("[user.init] creating desktop process...\n");
    proc_t *proc = Schedule::NewProcess(true);
    if (!proc) {
        kprintf_("[user.init] ERROR: NewProcess(desktop) failed; "
                 "no userland (recoverable, component stays OK)\n");
        return KMOD_OK;
    }

    char *argv[] = {(char *)"Test Main Thread"};
    char *envp[] = {nullptr};
    thread_t *desktop =
        Schedule::NewThread(proc, 0, 0, "/mp/desktop.elf", 1, argv, envp);
    if (!desktop)
        kprintf_("[user.init] WARN: desktop.elf thread creation failed (PID %d)\n",
                 (int)proc->id);

    proc_t *proc2 = Schedule::NewProcess(true);
    thread_t *desktop2 = nullptr;
    if (proc2)
        desktop2 = Schedule::NewThread(proc2, 0, 1, "/mp/hw.elf", 1, argv, envp);
    else
        kprintf_("[user.init] WARN: NewProcess(hw) failed\n");
    (void)desktop2;

    kprintf_("[user.init] desktop PROCESS: %d\n", (int)proc->id);
    /* 桌面进程 id 记入全局：诊断线程据此报告"窗口管理器已经不在" */
    extern int32_t g_desktop_pid;
    g_desktop_pid = (int32_t)proc->id;
    g_desktop_pid_cached = g_desktop_pid;
    g_desktop_created = true;
    kprintf_("[user.init] ready: desktop + hw processes queued; "
             "sti/IPI kick remains in T0 boot tail\n");
    return KMOD_OK;
}

static void comp_user_init_exit(void)
{
    /* 进程创建不可逆：v1 不支持热替换/卸载，exit 只记录审计。 */
    if (g_desktop_created)
        kprintf_("[user.init] exit: desktop PID %d keeps running "
                 "(cold-removal only)\n", (int)g_desktop_pid_cached);
}

KCOMPONENT_END(user_init)
