//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * fs_ext4.cpp —— 内建组件 kernel.fs.ext4（lwext4 内核实现）
 * ----------------------------------------------------------------------------
 * 迁移自 x86_64_init 的 ext4 使用路径（原 init.cpp line 248 的
 * ext4_kernel_init(dev, "/mp/", 0)；具体挂载动作由 fs.rootfs 触发）：
 *   - init：把 ext4 实现（mount/umount/stats）登记进 kernel.fs.vfs 的
 *           FS 实现注册表（消费 kernel.vfs.v1 契约），导出 kernel.fs.ext4.v1；
 *   - exit：从 VFS 注册表摘除（回滚/热替换时 consumers 已 unbind）。
 *   - 组件 init 不挂载任何文件系统（没有设备上下文）；挂载由 fs.rootfs 组件做。
 *
 * 行为不变性：mount 桥直接调用既有 ext4_kernel_init()/ext4_umount()，
 * 不改变 lwext4 任何语义。为避免"探测不存在的候选设备"把 lwext4 的
 * ext4_assert 变成内核 Panic，桥接层先做两步只读预检：
 *   1) Dev::GetSDEV(name) + classp 非空（ext4_blockdev_get 的前提）；
 *   2) 读超级块 magic（偏移 1024+0x38 = 1080，值 0xEF53）确认是 ext4。
 * 已知残余风险：预检通过但 ext4_mount 仍失败时 ext4_kernel_init() 内部会
 * hcf()（既有行为，本组件写作用域不允许改 kernel/src/fs 源码；已向 Lead
 * 报最小 patch 建议，见 B4 报告"残余风险"一节）。
 * ==========================================================================*/
#include <kmod/kmod_component.h>
#include <kmod/contracts/vfs.h>

#include <drivers/dev/dev.h>
#include <fs/lwext4/ext4.h>
#include <klib/errno.h>
#include <partition/mgr.h>

static int32_t comp_fs_ext4_init(void);
static void comp_fs_ext4_exit(void);

KCOMPONENT_BEGIN(fs_ext4, "kernel.fs.ext4", "1.0.0", KMOD_TIER_POLICY,
                 KMOD_MF_BOOT,
                 (uint64_t)(KMOD_CAP_FS | KMOD_CAP_BLOCK | KMOD_CAP_LOG |
                            KMOD_CAP_MEM_ALLOC),
                 0, "lwext4 implementation registered into kernel.vfs.v1",
                 comp_fs_ext4_init, comp_fs_ext4_exit, 0, 0, 1000u, 200u)

KCOMPONENT_IMPORT_FN(fs_ext4, kprintf_, int, (const char *, ...), "kernel",
                     "kprintf_", "kernel.log.v1")
/* 消费 VFS 契约（provider = kernel.fs.vfs；导出 symbol == 契约名，见宏约定） */
KCOMPONENT_IMPORT(fs_ext4, vfs_ops_, kmod_vfs_ops_t *, "kernel.fs.vfs",
                  "kernel.vfs.v1", "kernel.vfs.v1")
/* 块设备层必须先就绪（设备注册/枚举由 kernel.block.core + pci.enumerate 负责） */
KCOMPONENT_DEPENDS(fs_ext4, "kernel.block.core", block_core)

namespace {

constexpr uint32_t kExt4SuperSector = 2u;       /* 超级块字节偏移 1024 / 512 */
constexpr uint32_t kExt4MagicInSector = 0x38u;  /* 扇区内 magic 偏移 (1024+56) */
constexpr uint16_t kExt4Magic = 0xEF53u;

/* 只读预检：ext4 实现能否接管这个块设备。返回 0 = 可能是 ext4。
 * 读超级块所在的第 2 个 512B 扇区（magic 在扇区内偏移 0x38 = 56），
 * 走 PartitionManager::Read —— 与 lwext4 blockdev_bread 完全同一条路径，
 * 因此自动获得 USE_VIRT_IMAGE / 分区起始偏移两种模式下一致的行为。 */
int32_t ext4_device_probe(const char *dev_name, uint32_t wpart)
{
    if (!dev_name || !dev_name[0])
        return -EINVAL;
    VDL *dev = Dev::GetSDEV(dev_name);
    if (!dev || !dev->classp)
        return -ENODEV;
    if (!dev->ops.Read && !dev->ops.ReadBytes)
        return -ENODEV;
    alignas(16) uint8_t sb[512];
    _memset(sb, 0, sizeof(sb));
    if (!PartitionManager::Read(dev->type, dev->idx, (uint8_t)wpart,
                                kExt4SuperSector, 1u, sb))
        return -EIO;
    uint16_t m = (uint16_t)((uint16_t)sb[kExt4MagicInSector] |
                            ((uint16_t)sb[kExt4MagicInSector + 1u] << 8));
    if (m != kExt4Magic)
        return -ENODEV;
    return 0;
}

/* ---- VFS FS 实现桥（kmod_vfs_fs_impl_t） ---- */
int32_t ext4_impl_mount(const char *dev_name, const char *mount_point,
                        uint32_t wpart)
{
    int32_t rc = ext4_device_probe(dev_name, wpart);
    if (rc != 0)
        return rc;
    if (!ext4_kernel_init(dev_name, mount_point, wpart))
        return -EIO;
    return 0;
}

int32_t ext4_impl_umount(const char *mount_point)
{
    int32_t r = ext4_umount(mount_point);
    return r > 0 ? -r : r;
}

/* ---- kernel.fs.ext4.v1 扩展契约 ops ---- */
int32_t ext4_ops_mount(const char *dev_name, const char *mount_point,
                       uint32_t wpart)
{
    return ext4_impl_mount(dev_name, mount_point, wpart);
}

int32_t ext4_ops_umount(const char *mount_point)
{
    return ext4_impl_umount(mount_point);
}

int32_t ext4_ops_stats(const char *mount_point, uint64_t *total_bytes,
                       uint64_t *free_bytes, uint32_t *block_size)
{
    if (!mount_point || !total_bytes || !free_bytes || !block_size)
        return -EINVAL;
    struct ext4_mount_stats st;
    _memset(&st, 0, sizeof(st));
    int32_t r = ext4_mount_point_stats(mount_point, &st);
    if (r != EOK)
        return r > 0 ? -r : r;
    uint64_t bs = (uint64_t)st.block_size;
    *total_bytes = (uint64_t)st.blocks_count * bs;
    *free_bytes = (uint64_t)st.free_blocks_count * bs;
    *block_size = st.block_size;
    return 0;
}

/* FS 实现描述（vfs 注册表项） */
kmod_vfs_fs_impl_t g_ext4_impl = {
    KMOD_VFS_FS_IMPL_VERSION,
    (uint32_t)sizeof(kmod_vfs_fs_impl_t),
    "ext4",
    KMOD_VFS_FST_EXT4,
    KMOD_VFS_IMPL_F_MOUNTABLE | KMOD_VFS_IMPL_F_BUILTIN,
    ext4_impl_mount,
    ext4_impl_umount,
    {0u, 0u},
};

/* kernel.fs.ext4.v1 ops */
kmod_fs_ext4_ops_t g_ext4_ops = {
    KMOD_FS_EXT4_OPS_VERSION,
    (uint32_t)sizeof(kmod_fs_ext4_ops_t),
    ext4_ops_mount,
    ext4_ops_umount,
    ext4_ops_stats,
};

bool g_ext4_registered = false;

} /* namespace */

KCOMPONENT_EXPORT_CONTRACT(fs_ext4, KMOD_CONTRACT_FS_EXT4_V1, g_ext4_ops, 1)

/* ==================================================================== */
/* 生命周期                                                              */
/* ==================================================================== */
static int32_t comp_fs_ext4_init(void)
{
    if (g_ext4_registered)
        return KMOD_OK; /* 幂等 */

    if (!vfs_ops_) {
        kprintf_("[fs.ext4] init FAILED: kernel.vfs.v1 import not bound\n");
        return KMOD_ERR_UNRESOLVED;
    }
    if (vfs_ops_->version != KMOD_VFS_OPS_VERSION ||
        vfs_ops_->size != (uint32_t)sizeof(kmod_vfs_ops_t)) {
        kprintf_("[fs.ext4] init FAILED: kernel.vfs.v1 layout mismatch "
                 "(v=%u size=%u, expected v=%u size=%u)\n",
                 (unsigned)vfs_ops_->version, (unsigned)vfs_ops_->size,
                 (unsigned)KMOD_VFS_OPS_VERSION,
                 (unsigned)sizeof(kmod_vfs_ops_t));
        return KMOD_ERR_CONTRACT;
    }
    if (!vfs_ops_->register_fs) {
        kprintf_("[fs.ext4] init FAILED: register_fs missing\n");
        return KMOD_ERR_CONTRACT;
    }
    int32_t idx = vfs_ops_->register_fs(&g_ext4_impl);
    if (idx < 0) {
        kprintf_("[fs.ext4] register_fs(ext4) failed rc=%d\n", (int)idx);
        return idx;
    }
    g_ext4_registered = true;
    kprintf_("[fs.ext4] ready: lwext4 registered into kernel.vfs.v1 at slot %d\n",
             (int)idx);
    return KMOD_OK;
}

static void comp_fs_ext4_exit(void)
{
    if (g_ext4_registered && vfs_ops_ && vfs_ops_->unregister_fs) {
        int32_t rc = vfs_ops_->unregister_fs("ext4");
        if (rc != 0)
            kprintf_("[fs.ext4] exit: unregister_fs rc=%d\n", (int)rc);
    }
    g_ext4_registered = false;
}

KCOMPONENT_END(fs_ext4)
