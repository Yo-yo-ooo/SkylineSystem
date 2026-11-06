//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * net_lwip.cpp —— 内建组件 kernel.net.lwip（lwIP 协议栈生命周期）
 * ----------------------------------------------------------------------------
 * 精确迁移自 x86_64_init 的 deferred 网络上线块（原 init.cpp line 276-287，
 * SCHED_BENCH_AUTO=0 分支）：
 *
 *     proc_t *nproc = Schedule::NewProcess(false);
 *     if (nproc)
 *         Schedule::NewKernelThreadEx(nproc, 0, 8, (void *)NetStackInitDeferred, 8);
 *
 * 语义保持：
 *   - **不阻塞启动**：组件 init 只创建进程 + 内核线程，立即返回；真正的
 *     lwIP 初始化（lwip_init/netif_add/DHCP 线程）由该线程在调度器运行后
 *     通过既有 NetStackInitDeferred() -> NetStackInit() 执行（kernel/src/
 *     drivers/net/e1000_lwip.cpp，本组件不改动它）；
 *   - e1000 的 PCI probe（E1000::Init）由 B3 的 kernel.pci.enumerate 在更早的
 *     拓扑位置完成；本组件依赖 kernel.driver.e1000 形成显式边，不靠链接顺序。
 *
 * 失败语义：进程/线程创建失败 = 可恢复（网络不可用但系统可启动）-> 记日志并
 * 返回 KMOD_OK；E1000 不存在同理（QEMU 可无网卡）。
 * 已知限制：lwIP 线程一旦创建不可停止，组件不支持热替换/卸载（无 quiesce）。
 * ==========================================================================*/
#include <kmod/kmod_component.h>
#include <kmod/contracts/netdev.h>

#include <arch/x86_64/schedule/sched.h>
#include <drivers/net/e1000.h>
#include <klib/errno.h>

static int32_t comp_net_lwip_init(void);
static void comp_net_lwip_exit(void);

KCOMPONENT_BEGIN(net_lwip, "kernel.net.lwip", "1.0.0", KMOD_TIER_POLICY,
                 KMOD_MF_BOOT,
                 (uint64_t)(KMOD_CAP_NET | KMOD_CAP_LOG | KMOD_CAP_MEM_ALLOC),
                 0, "lwIP bring-up as deferred kernel thread (non-blocking boot)",
                 comp_net_lwip_init, comp_net_lwip_exit, 0, 0, 500u, 100u)

KCOMPONENT_IMPORT_FN(net_lwip, kprintf_, int, (const char *, ...), "kernel",
                     "kprintf_", "kernel.log.v1")
/* 消费 netdev 注册表契约（当前 lwIP 胶水仍硬编码 e1000，见 netdev.h 现状；
 * 这里导入同时形成 provider 先序边，未来 attach_device 实现可直接用）。 */
KCOMPONENT_IMPORT(net_lwip, netdev_ops_, kmod_netdev_ops_t *,
                  "kernel.net.netdev", "kernel.netdev.v1", "kernel.netdev.v1")
/* 拓扑依赖：网卡驱动组件 / 调度器安装完成 / 根文件系统（可能需要 /mp 配置） */
KCOMPONENT_DEPENDS(net_lwip, "kernel.driver.e1000", driver_e1000)
KCOMPONENT_DEPENDS(net_lwip, "kernel.sched.install", sched_install)
KCOMPONENT_DEPENDS(net_lwip, "kernel.fs.rootfs", fs_rootfs)

namespace {

/* 创建 "deferred 网络初始化" 内核线程（不等待、不睡眠）。
 * 返回 0 = 已排队，负值 = 可恢复失败。 */
int32_t spawn_deferred_thread(void)
{
    proc_t *nproc = Schedule::NewProcess(false);
    if (!nproc) {
        kprintf_("[net.lwip] cannot create process for deferred bring-up\n");
        return -ENOMEM;
    }
    /* 栈 8 页 = 32 KiB：lwIP 输入路径 + DHCP 深调用链（既有注释结论） */
    thread_t *t = Schedule::NewKernelThreadEx(nproc, 0, 8,
                                              (void *)NetStackInitDeferred, 8);
    if (!t) {
        kprintf_("[net.lwip] cannot create deferred kernel thread\n");
        return -EAGAIN;
    }
    return 0;
}

/* ---- kernel.net.stack.v1 ops ---- */
int32_t netstack_ops_up(void)
{
    /* 同步拉起（幂等：e1000_lwip.cpp 内 g_started 保证）。引导期不调用。 */
    NetStackInit();
    return 0;
}

int32_t netstack_ops_deferred_up(void)
{
    int32_t rc = spawn_deferred_thread();
    return rc;
}

int32_t netstack_ops_attach_device(void *dev)
{
    (void)dev;
    /* v1：lwIP netif 接入仍固定在 NetStackInit() 内部（e1000 专用），
     * 需要改 kernel/src/drivers/net/e1000_lwip.cpp 才能通用化；
     * 本组件写作用域不允许改既有 net 源码，故诚实返回 -ENOTSUP。 */
    return -ENOTSUP;
}

kmod_net_stack_ops_t g_net_stack_ops = {
    KMOD_NET_STACK_OPS_VERSION,
    (uint32_t)sizeof(kmod_net_stack_ops_t),
    netstack_ops_up,
    netstack_ops_deferred_up,
    netstack_ops_attach_device,
};

bool g_lwip_queued = false;

} /* namespace */

KCOMPONENT_EXPORT_CONTRACT(net_lwip, KMOD_CONTRACT_NET_STACK_V1, g_net_stack_ops, 1)

/* ==================================================================== */
/* 生命周期                                                              */
/* ==================================================================== */
static int32_t comp_net_lwip_init(void)
{
    if (g_lwip_queued)
        return KMOD_OK; /* 幂等 */

    if (netdev_ops_ &&
        (netdev_ops_->version != KMOD_NETDEV_OPS_VERSION ||
         netdev_ops_->size != (uint32_t)sizeof(kmod_netdev_ops_t))) {
        kprintf_("[net.lwip] init FAILED: kernel.netdev.v1 layout mismatch\n");
        return KMOD_ERR_CONTRACT;
    }

    int32_t rc = spawn_deferred_thread();
    if (rc != 0) {
        /* 可恢复：网络不可用不阻断启动（与原 init.cpp 的 if (nproc) 静默跳过同义） */
        kprintf_("[net.lwip] deferred bring-up unavailable rc=%d; "
                 "net stack stays offline\n", (int)rc);
        return KMOD_OK;
    }
    g_lwip_queued = true;
    kprintf_("[net.lwip] deferred lwIP bring-up thread queued (non-blocking; "
             "starts after sti/scheduler kick)\n");
    return KMOD_OK;
}

static void comp_net_lwip_exit(void)
{
    /* 线程不可回收：v1 不支持热替换/卸载，exit 只记录审计。 */
    if (g_lwip_queued)
        kprintf_("[net.lwip] exit: deferred thread cannot be stopped "
                 "(cold-removal only)\n");
}

KCOMPONENT_END(net_lwip)
