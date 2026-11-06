//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * fs_rootfs.cpp —— 内建组件 kernel.fs.rootfs（根文件系统探测与挂载）
 * ----------------------------------------------------------------------------
 * 精确迁移自 x86_64_init 的根设备探测块（原 init.cpp line 242-257）：
 *
 *     static const char *kRootCandidates[] = {"sata0","nvme0","usb0","sata1",0};
 *     for (...) if (ext4_kernel_init(kRootCandidates[i], "/mp/", 0)) break;
 *     if (!root_ok) { kerrorln("init: no usable root filesystem device, halting"); hcf(); }
 *
 * 迁移差异（按 B4 任务要求，失败语义显式化）：
 *   - 每个候选先经 kernel.fs.ext4.v1 的 mount 桥做**只读预检**（设备存在 +
 *     超级块 magic == 0xEF53），非候选/非 ext4 设备只记日志并继续下一个；
 *   - 全部失败返回 KMOD_ERR_NOTFOUND（启动日志可见）。**本组件不 hcf**：
 *     是否把"无根文件系统"变成停机由 Lead 的启动尾部（init.cpp T0 收尾）决定，
 *     这是"组件失败不 panic"硬性要求的落点。
 *   - 日志用导入的 kprintf_（kernel.log.v1），前缀 [fs.rootfs]。
 *
 * 依赖（provider 先序由内建组件拓扑保证）：
 *   kernel.fs.vfs / kernel.fs.ext4   —— VFS 注册表就绪 + ext4 挂载桥
 *   kernel.block.core               —— 块设备抽象层（Dev 注册表）
 *   kernel.pci.enumerate            —— 两阶段 PCI：驱动 match 注册后完成枚举/
 *                                      probe（AHCI/NVMe/xHCI 的块设备只有到这里
 *                                      才真正注册进 Dev 表，rootfs 必须晚于它）
 * ==========================================================================*/
#include <kmod/kmod_component.h>
#include <kmod/contracts/vfs.h>

static int32_t comp_fs_rootfs_init(void);
static void comp_fs_rootfs_exit(void);

KCOMPONENT_BEGIN(fs_rootfs, "kernel.fs.rootfs", "1.0.0", KMOD_TIER_POLICY,
                 KMOD_MF_BOOT,
                 (uint64_t)(KMOD_CAP_FS | KMOD_CAP_BLOCK | KMOD_CAP_DEVICE |
                            KMOD_CAP_LOG | KMOD_CAP_MEM_ALLOC),
                 0, "probe sata0/nvme0/usb0/sata1 and mount ext4 at /mp/",
                 comp_fs_rootfs_init, comp_fs_rootfs_exit, 0, 0, 5000u, 200u)

KCOMPONENT_IMPORT_FN(fs_rootfs, kprintf_, int, (const char *, ...), "kernel",
                     "kprintf_", "kernel.log.v1")
/* ext4 实现契约（provider = kernel.fs.ext4）：mount 桥含只读预检 */
KCOMPONENT_IMPORT(fs_rootfs, ext4_ops_, kmod_fs_ext4_ops_t *, "kernel.fs.ext4",
                  "kernel.fs.ext4.v1", "kernel.fs.ext4.v1")
KCOMPONENT_DEPENDS(fs_rootfs, "kernel.fs.vfs", fs_vfs)
KCOMPONENT_DEPENDS(fs_rootfs, "kernel.block.core", block_core)
KCOMPONENT_DEPENDS(fs_rootfs, "kernel.pci.enumerate", pci_enumerate)

namespace {

constexpr const char *kMountPoint = "/mp/";

/* 与迁移前完全一致的候选顺序（不要改：QEMU 参考镜像 -> sata0；纯 NVMe -> nvme0） */
const char *const kRootCandidates[] = {"sata0", "nvme0", "usb0", "sata1", nullptr};

const char *g_root_device = nullptr;
bool g_root_mounted = false;

int32_t rootfs_ops_mounted(void)
{
    return g_root_mounted ? 1 : 0;
}

const char *rootfs_ops_device(void)
{
    return g_root_device;
}

const char *rootfs_ops_mount_point(void)
{
    return g_root_mounted ? kMountPoint : nullptr;
}

kmod_fs_rootfs_ops_t g_rootfs_ops = {
    KMOD_FS_ROOTFS_OPS_VERSION,
    (uint32_t)sizeof(kmod_fs_rootfs_ops_t),
    rootfs_ops_mounted,
    rootfs_ops_device,
    rootfs_ops_mount_point,
};

} /* namespace */

KCOMPONENT_EXPORT_CONTRACT(fs_rootfs, KMOD_CONTRACT_FS_ROOTFS_V1, g_rootfs_ops, 1)

/* ==================================================================== */
/* 生命周期                                                              */
/* ==================================================================== */
static int32_t comp_fs_rootfs_init(void)
{
    if (g_root_mounted)
        return KMOD_OK; /* 幂等 */

    if (!ext4_ops_ || !ext4_ops_->mount) {
        kprintf_("[fs.rootfs] init FAILED: kernel.fs.ext4.v1 import not bound\n");
        return KMOD_ERR_UNRESOLVED;
    }
    if (ext4_ops_->version != KMOD_FS_EXT4_OPS_VERSION ||
        ext4_ops_->size != (uint32_t)sizeof(kmod_fs_ext4_ops_t)) {
        kprintf_("[fs.rootfs] init FAILED: kernel.fs.ext4.v1 layout mismatch "
                 "(v=%u size=%u)\n",
                 (unsigned)ext4_ops_->version, (unsigned)ext4_ops_->size);
        return KMOD_ERR_CONTRACT;
    }

    for (uint32_t i = 0; kRootCandidates[i]; i++) {
        const char *cand = kRootCandidates[i];
        int32_t rc = ext4_ops_->mount(cand, kMountPoint, 0u);
        if (rc == 0) {
            g_root_device = cand;
            g_root_mounted = true;
            kprintf_("[fs.rootfs] mounted %s at %s (candidate #%u)\n", cand,
                     kMountPoint, (unsigned)i);
            return KMOD_OK;
        }
        kprintf_("[fs.rootfs] candidate %s skipped rc=%d\n", cand, (int)rc);
    }

    kprintf_("[fs.rootfs] NO usable root filesystem (tried sata0/nvme0/usb0/sata1); "
             "returning KMOD_ERR_NOTFOUND (hcf decision belongs to boot tail)\n");
    return KMOD_ERR_NOTFOUND;
}

static void comp_fs_rootfs_exit(void)
{
    /* v1 不主动 umount：根文件系统是全体用户态进程的依赖，卸载只能冷重启。
     * 组件回滚时 exit 只记录审计（内建组件 manager 的失败回滚语义）。
     * 保留 g_root_mounted / g_root_device：若同一镜像随后被重新加载，
     * init 幂等直接返回 OK，不会对已挂载的 /mp/ 做第二次 mount。 */
    if (g_root_mounted)
        kprintf_("[fs.rootfs] exit: %s stays mounted (cold-removal only)\n",
                 g_root_device ? g_root_device : "?");
}

KCOMPONENT_END(fs_rootfs)
