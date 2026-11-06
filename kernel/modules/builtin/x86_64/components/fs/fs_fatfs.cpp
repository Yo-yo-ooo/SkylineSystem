//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * fs_fatfs.cpp —— 内建组件 kernel.fs.fatfs（FatFs 实现登记，最小组件）
 * ----------------------------------------------------------------------------
 * 现状盘点（不粉饰）：
 *   - FatFs（kernel/src/fs/fatfs/）目前只被**文件系统识别**使用：
 *     kernel/src/partition/identfstype.cpp 调 IdentifyFat()（FAT12/16/32/exFAT）；
 *   - 没有 VFS 挂载路径：FatFs 的磁盘 I/O glue（kernel/src/fs/fatfs/diskio.cpp）
 *     仍是空壳（disk_status=STA_NOINIT、disk_read/write=RES_ERROR），
 *     kernel/src/fs/fatfs/vfsi.cpp 也是空文件；也没有任何进程/内核代码调用
 *     f_mount()。
 *   - FatFs 的同步对象是 f_mount() 时按卷惰性创建（ffsystem.cpp），
 *     没有全局 init 需要迁移，因此本组件 init 只做"实现登记 + 状态日志"。
 *
 * 因此本组件导出 kernel.fs.fatfs.v1（identify + flags）并把 fatfs 以
 * **只读/不可挂载** 方式登记进 kernel.vfs.v1 注册表（KMOD_VFS_IMPL_F_READONLY，
 * 不带 MOUNTABLE）：vfs.mount 自动派发会跳过它，行为与迁移前完全一致。
 * 未来 diskio glue 接通块设备层后，把 impl flags 改成 MOUNTABLE 并实现
 * mount 桥即可（契约布局不变，无需升 abi）。
 * ==========================================================================*/
#include <kmod/kmod_component.h>
#include <kmod/contracts/vfs.h>

#include <fs/fatfs/identfat.h>
#include <klib/errno.h>

static int32_t comp_fs_fatfs_init(void);
static void comp_fs_fatfs_exit(void);

KCOMPONENT_BEGIN(fs_fatfs, "kernel.fs.fatfs", "1.0.0", KMOD_TIER_POLICY,
                 KMOD_MF_BOOT,
                 (uint64_t)(KMOD_CAP_FS | KMOD_CAP_LOG), 0,
                 "FatFs: identify-only registration (VFS disk glue is a stub)",
                 comp_fs_fatfs_init, comp_fs_fatfs_exit, 0, 0, 500u, 100u)

KCOMPONENT_IMPORT_FN(fs_fatfs, kprintf_, int, (const char *, ...), "kernel",
                     "kprintf_", "kernel.log.v1")
KCOMPONENT_IMPORT(fs_fatfs, vfs_ops_, kmod_vfs_ops_t *, "kernel.fs.vfs",
                  "kernel.vfs.v1", "kernel.vfs.v1")

namespace {

/* FatFs 在 VFS 注册表里的占位实现：v1 不提供挂载（diskio glue 未接通）。 */
int32_t fatfs_impl_mount(const char *dev_name, const char *mount_point,
                         uint32_t wpart)
{
    (void)dev_name;
    (void)mount_point;
    (void)wpart;
    return -ENOTSUP;
}

int32_t fatfs_impl_umount(const char *mount_point)
{
    (void)mount_point;
    return -ENOTSUP;
}

kmod_vfs_fs_impl_t g_fatfs_impl = {
    KMOD_VFS_FS_IMPL_VERSION,
    (uint32_t)sizeof(kmod_vfs_fs_impl_t),
    "fatfs",
    KMOD_VFS_FST_FATFS,
    KMOD_VFS_IMPL_F_READONLY | KMOD_VFS_IMPL_F_BUILTIN, /* 无 MOUNTABLE */
    fatfs_impl_mount,
    fatfs_impl_umount,
    {0u, 0u},
};

/* ---- kernel.fs.fatfs.v1 ops ---- */
int32_t fatfs_ops_identify(uint32_t dev_type, uint32_t dev_index,
                           uint32_t partition_id, uint32_t use_virt_image,
                           uint32_t *type_out, uint32_t *error_out)
{
    if (!type_out || !error_out)
        return -EINVAL;
    FS_TYPE fst = IdentifyFat((VsDevType)dev_type, dev_index, partition_id,
                              use_virt_image != 0u);
    *type_out = (uint32_t)fst.TypeID;
    *error_out = (uint32_t)fst.ErrorCode;
    return fst.ErrorCode == 0u ? 0 : -EIO;
}

int32_t fatfs_ops_mount(const char *dev_name, const char *mount_point,
                        uint32_t wpart)
{
    (void)dev_name;
    (void)mount_point;
    (void)wpart;
    return -ENOTSUP;
}

uint32_t fatfs_ops_flags(void)
{
    return KMOD_VFS_IMPL_F_READONLY | KMOD_VFS_IMPL_F_BUILTIN;
}

kmod_fs_fatfs_ops_t g_fatfs_ops = {
    KMOD_FS_FATFS_OPS_VERSION,
    (uint32_t)sizeof(kmod_fs_fatfs_ops_t),
    fatfs_ops_identify,
    fatfs_ops_mount,
    fatfs_ops_flags,
};

bool g_fatfs_registered = false;

} /* namespace */

KCOMPONENT_EXPORT_CONTRACT(fs_fatfs, KMOD_CONTRACT_FS_FATFS_V1, g_fatfs_ops, 1)

/* ==================================================================== */
/* 生命周期                                                              */
/* ==================================================================== */
static int32_t comp_fs_fatfs_init(void)
{
    if (g_fatfs_registered)
        return KMOD_OK; /* 幂等 */

    if (!vfs_ops_) {
        kprintf_("[fs.fatfs] init FAILED: kernel.vfs.v1 import not bound\n");
        return KMOD_ERR_UNRESOLVED;
    }
    if (vfs_ops_->version != KMOD_VFS_OPS_VERSION ||
        vfs_ops_->size != (uint32_t)sizeof(kmod_vfs_ops_t)) {
        kprintf_("[fs.fatfs] init FAILED: kernel.vfs.v1 layout mismatch\n");
        return KMOD_ERR_CONTRACT;
    }
    if (!vfs_ops_->register_fs) {
        kprintf_("[fs.fatfs] init FAILED: register_fs missing\n");
        return KMOD_ERR_CONTRACT;
    }
    int32_t idx = vfs_ops_->register_fs(&g_fatfs_impl);
    if (idx < 0) {
        kprintf_("[fs.fatfs] register_fs(fatfs) failed rc=%d\n", (int)idx);
        return idx;
    }
    g_fatfs_registered = true;
    kprintf_("[fs.fatfs] ready: identify-only registration at vfs slot %d "
             "(VFS mount glue not wired: diskio stub; behavior unchanged)\n",
             (int)idx);
    return KMOD_OK;
}

static void comp_fs_fatfs_exit(void)
{
    if (g_fatfs_registered && vfs_ops_ && vfs_ops_->unregister_fs) {
        int32_t rc = vfs_ops_->unregister_fs("fatfs");
        if (rc != 0)
            kprintf_("[fs.fatfs] exit: unregister_fs rc=%d\n", (int)rc);
    }
    g_fatfs_registered = false;
}

KCOMPONENT_END(fs_fatfs)
