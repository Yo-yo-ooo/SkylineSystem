//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * fs_vfs.cpp —— 内建组件 kernel.fs.vfs（VFS 核心）
 * ----------------------------------------------------------------------------
 * 迁移自 x86_64_init 的 VFS 全局初始化（原 init.cpp 里没有独立一行：VFS 的
 * 全局状态只有 InitFFMAN() 创建的挂载点 hashmap，而 InitFFMAN 已由 B1 的
 * kernel.dev.core 组件负责，本组件**不重复初始化**）：
 *   - init：校验挂载表白板已由 kernel.dev.core 建好（HMapS_MP != NULL），
 *           发布 kernel.vfs.v1 契约（FS 实现注册表 + 路径解析 + 文件打开最小集）；
 *   - exit：清空 FS 实现注册表（热替换/回滚时 consumers 已 unbind）。
 *   - fd_manager 是 per-process 状态（Schedule::NewProcess 里 fd_manager_init），
 *     不存在全局初始化，故不在此组件 init 内。
 *
 * 行为不变性：本组件**不替换**任何现有 syscall 路径；kernel.vfs.v1 的
 * open/read/write 是给组件/.ko 的旁路 API（v1 直通 FS 实现、不经文件缓存），
 * 内核 sys_fopen/sys_fread 等实现保持原样。
 * ==========================================================================*/
#include <kmod/kmod_component.h>
#include <kmod/contracts/vfs.h>

#include <fs/fd.h>
#include <fs/fc.h>
#include <fs/lwext4/ext4.h>

#include <arch/x86_64/schedule/sched.h>
#include <arch/x86_64/smp/smp.h>
#include <klib/errno.h>
#include <klib/klib.h>
#include <mem/heap.h>

static int32_t comp_fs_vfs_init(void);
static void comp_fs_vfs_exit(void);

KCOMPONENT_BEGIN(fs_vfs, "kernel.fs.vfs", "1.0.0", KMOD_TIER_POLICY,
                 KMOD_MF_BOOT,
                 (uint64_t)(KMOD_CAP_FS | KMOD_CAP_LOG | KMOD_CAP_MEM_ALLOC), 0,
                 "VFS core: fs registry + path resolve + fd file ops",
                 comp_fs_vfs_init, comp_fs_vfs_exit, 0, 0, 1000u, 200u)

KCOMPONENT_IMPORT_FN(fs_vfs, kprintf_, int, (const char *, ...), "kernel",
                     "kprintf_", "kernel.log.v1")
/* 纯拓扑依赖：VFS 的挂载点 hashmap 由 kernel.dev.core 的 InitFFMAN() 建立，
 * 这里不导入符号（InitFFMAN 仍只在 dev.core 里调用一次），只声明先后顺序。 */
KCOMPONENT_DEPENDS(fs_vfs, "kernel.dev.core", dev_core)

/* ==================================================================== */
/* FS 实现注册表（组件内状态；ext4/fatfs 组件在各自 init 里登记）        */
/* ==================================================================== */
namespace {

struct VfsRegEntry {
    bool used;
    kmod_vfs_fs_impl_t impl;    /* 结构拷贝：调用方栈对象生命周期不受限 */
};

VfsRegEntry g_vfs_reg[KMOD_VFS_MAX_IMPL];
spinlock_t g_vfs_reg_lock = 0;
bool g_vfs_ready = false;

constexpr uint32_t kInvalidIndex = 0xFFFFFFFFu;

bool valid_impl(const kmod_vfs_fs_impl_t *impl)
{
    if (!impl)
        return false;
    if (impl->version != KMOD_VFS_FS_IMPL_VERSION ||
        impl->size != (uint32_t)sizeof(kmod_vfs_fs_impl_t))
        return false;
    if (!impl->name || !impl->name[0] || !impl->mount)
        return false;
    size_t n = 0;
    while (n < KMOD_VFS_NAME_MAX && impl->name[n])
        n++;
    return n > 0 && n < KMOD_VFS_NAME_MAX;
}

bool same_name(const char *a, const char *b)
{
    if (!a || !b)
        return false;
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

/* 返回注册表下标（>=0）或负 errno。 */
int32_t vfs_register_fs(const kmod_vfs_fs_impl_t *impl)
{
    if (!valid_impl(impl))
        return -EINVAL;
    int32_t rc = -ENOSPC;
    spinlock_lock(&g_vfs_reg_lock);
    for (uint32_t i = 0; i < KMOD_VFS_MAX_IMPL; i++) {
        if (g_vfs_reg[i].used && same_name(g_vfs_reg[i].impl.name, impl->name)) {
            rc = -EEXIST;
            goto out;
        }
    }
    for (uint32_t i = 0; i < KMOD_VFS_MAX_IMPL; i++) {
        if (!g_vfs_reg[i].used) {
            g_vfs_reg[i].used = true;
            g_vfs_reg[i].impl = *impl;
            rc = (int32_t)i;
            break;
        }
    }
out:
    spinlock_unlock(&g_vfs_reg_lock);
    return rc;
}

int32_t vfs_unregister_fs(const char *name)
{
    if (!name || !name[0])
        return -EINVAL;
    int32_t rc = -ENOENT;
    spinlock_lock(&g_vfs_reg_lock);
    for (uint32_t i = 0; i < KMOD_VFS_MAX_IMPL; i++) {
        if (g_vfs_reg[i].used && same_name(g_vfs_reg[i].impl.name, name)) {
            g_vfs_reg[i].used = false;
            _memset(&g_vfs_reg[i].impl, 0, sizeof(g_vfs_reg[i].impl));
            rc = 0;
            break;
        }
    }
    spinlock_unlock(&g_vfs_reg_lock);
    return rc;
}

int32_t vfs_find_impl_index(const char *name)
{
    int32_t rc = -ENOENT;
    spinlock_lock(&g_vfs_reg_lock);
    for (uint32_t i = 0; i < KMOD_VFS_MAX_IMPL; i++) {
        if (g_vfs_reg[i].used && same_name(g_vfs_reg[i].impl.name, name)) {
            rc = (int32_t)i;
            break;
        }
    }
    spinlock_unlock(&g_vfs_reg_lock);
    return rc;
}

/* 按注册顺序尝试所有 MOUNTABLE 实现；返回 0 = 挂载成功。 */
int32_t vfs_mount(const char *dev_name, const char *mount_point, uint32_t wpart)
{
    if (!dev_name || !mount_point)
        return -EINVAL;
    int32_t last = -ENODEV;
    for (uint32_t i = 0; i < KMOD_VFS_MAX_IMPL; i++) {
        kmod_vfs_fs_impl_t impl;
        bool used = false;
        spinlock_lock(&g_vfs_reg_lock);
        used = g_vfs_reg[i].used;
        if (used)
            impl = g_vfs_reg[i].impl;
        spinlock_unlock(&g_vfs_reg_lock);
        if (!used || !(impl.flags & KMOD_VFS_IMPL_F_MOUNTABLE) || !impl.mount)
            continue;
        last = impl.mount(dev_name, mount_point, wpart);
        if (last == 0)
            return 0;
    }
    return last;
}

int32_t vfs_umount(const char *mount_point)
{
    if (!mount_point)
        return -EINVAL;
    /* GetMount 内部按 "/<mp>/" 前缀查挂载点表；未挂载路径返回 NULL。 */
    __hmap_s_mp *MP = GetMount(mount_point);
    if (!MP)
        return -ENOENT;
    uint32_t fstype = (uint32_t)MP->FST;

    int32_t rc = -ENODEV;
    for (uint32_t i = 0; i < KMOD_VFS_MAX_IMPL; i++) {
        kmod_vfs_fs_impl_t impl;
        bool used = false;
        spinlock_lock(&g_vfs_reg_lock);
        used = g_vfs_reg[i].used;
        if (used)
            impl = g_vfs_reg[i].impl;
        spinlock_unlock(&g_vfs_reg_lock);
        if (!used || impl.fstype != fstype || !(impl.flags & KMOD_VFS_IMPL_F_MOUNTABLE) ||
            !impl.umount)
            continue;
        rc = impl.umount(mount_point);
        if (rc == 0)
            return 0;
    }
    return rc;
}

int32_t vfs_resolve(const char *path, kmod_vfs_mount_info_t *out)
{
    if (!path || !out)
        return -EINVAL;
    if (out->version != KMOD_VFS_MOUNT_INFO_VERSION ||
        out->size != (uint32_t)sizeof(kmod_vfs_mount_info_t))
        return -EINVAL;

    __hmap_s_mp *MP = GetMount(path);
    if (!MP)
        return -ENOENT;

    uint32_t fs_index = kInvalidIndex;
    for (uint32_t i = 0; i < KMOD_VFS_MAX_IMPL; i++) {
        bool hit = false;
        spinlock_lock(&g_vfs_reg_lock);
        hit = g_vfs_reg[i].used && g_vfs_reg[i].impl.fstype == (uint32_t)MP->FST;
        spinlock_unlock(&g_vfs_reg_lock);
        if (hit) {
            fs_index = i;
            break;
        }
    }

    out->fstype = (uint32_t)MP->FST;
    out->fs_impl_index = fs_index;
    out->mount_id = (uint64_t)(uintptr_t)MP;
    out->mount_point = MP->MPName;
    out->mp = MP->MP;
    out->flags = 0u;
    out->reserved = 0u;
    return 0;
}

/* ---- fd 文件打开最小集（对应 kernel/src/arch/x86_64/schedule/syscall/fops.cpp
 *      的 sys_fopen/sys_fread/... 语义；v1 旁路文件缓存） ---- */

int32_t vfs_open(const char *path, uint32_t flags, int32_t *fd_out)
{
    if (!path || !path[0] || !fd_out)
        return -EINVAL;
    proc_t *proc = Schedule::this_proc();
    if (!proc || !proc->FDMan)
        return -EINVAL; /* bootstrap/中断上下文没有进程 FD 表 */

    __hmap_s_mp *MP = GetMount(path);
    if (!MP || !MP->FSOPS || !MP->FSOPS->open)
        return -ENOENT;

    size_t plen = strlen(path);
    if (plen >= (size_t)PATH_MAX)
        return -ENAMETOOLONG;

    fd_t *fd_struct = nullptr;
    int32_t fd_idx = fd_alloc(proc->FDMan, &fd_struct);
    if (fd_idx < 0 || !fd_struct)
        return -EMFILE;

    fd_struct->FSOPS = MP->FSOPS;
    fd_struct->MP = MP;
    fd_struct->offset = 0;
    fd_struct->file_uid = fc_next_file_uid();
    fd_struct->filedesc = kmalloc(MP->FSOPS->SIZEOF_FILE_DESC);
    fd_struct->path = nullptr;
    if (!fd_struct->filedesc) {
        fd_free(proc->FDMan, fd_idx);
        return -ENOMEM;
    }
    _memset(fd_struct->filedesc, 0, MP->FSOPS->SIZEOF_FILE_DESC);

    fd_struct->path = (char *)kmalloc(plen + 1);
    if (!fd_struct->path) {
        kfree(fd_struct->filedesc);
        fd_free(proc->FDMan, fd_idx);
        return -ENOMEM;
    }
    __memcpy(fd_struct->path, path, plen);
    fd_struct->path[plen] = '\0';
    fd_struct->path_len = (uint32_t)plen;

    int32_t err = MP->FSOPS->open(fd_struct->filedesc, path, (int32_t)flags);
    if (err != 0) { /* FS 返回 0 成功 / 正 errno，syscall ABI 用负 errno */
        kfree(fd_struct->path);
        kfree(fd_struct->filedesc);
        fd_free(proc->FDMan, fd_idx);
        return err > 0 ? -err : err;
    }
    fd_struct->file_size = MP->FSOPS->fsize(fd_struct->filedesc);
    *fd_out = fd_idx;
    return 0;
}

int32_t vfs_close(int32_t fd)
{
    proc_t *proc = Schedule::this_proc();
    if (!proc || !proc->FDMan)
        return -EINVAL;
    fd_t *FD = fd_get(proc->FDMan, fd);
    if (!FD)
        return -EBADF;
    int32_t res = 0;
    if (FD->FSOPS && FD->FSOPS->close)
        res = FD->FSOPS->close(FD->filedesc);
    if (FD->path) {
        kfree(FD->path);
        FD->path = nullptr;
    }
    fd_free(proc->FDMan, fd);
    return res > 0 ? -res : res;
}

int32_t vfs_read(int32_t fd, void *buf, uint64_t count, uint64_t *read_out)
{
    if (!read_out)
        return -EINVAL;
    *read_out = 0;
    if (count == 0)
        return 0;
    if (!buf)
        return -EINVAL;
    proc_t *proc = Schedule::this_proc();
    if (!proc || !proc->FDMan)
        return -EINVAL;
    fd_t *FD = fd_get(proc->FDMan, fd);
    if (!FD || !FD->FSOPS || !FD->filedesc)
        return -EBADF;

    FD->FSOPS->lseek(FD->filedesc, FD->offset, KMOD_VFS_SEEK_SET);
    size_t n = 0;
    int32_t rc = FD->FSOPS->read(FD->filedesc, buf, (size_t)count, &n);
    if (rc != 0)
        return rc > 0 ? -rc : rc;
    FD->offset += n;
    *read_out = (uint64_t)n;
    return 0;
}

int32_t vfs_write(int32_t fd, const void *buf, uint64_t count,
                  uint64_t *written_out)
{
    if (!written_out)
        return -EINVAL;
    *written_out = 0;
    if (count == 0)
        return 0;
    if (!buf)
        return -EINVAL;
    proc_t *proc = Schedule::this_proc();
    if (!proc || !proc->FDMan)
        return -EINVAL;
    fd_t *FD = fd_get(proc->FDMan, fd);
    if (!FD || !FD->FSOPS || !FD->filedesc)
        return -EBADF;

    FD->FSOPS->lseek(FD->filedesc, FD->offset, KMOD_VFS_SEEK_SET);
    size_t n = 0;
    int32_t rc = FD->FSOPS->write(FD->filedesc, buf, (size_t)count, &n);
    if (rc != 0)
        return rc > 0 ? -rc : rc;
    FD->offset += n;
    FD->file_size = FD->FSOPS->fsize(FD->filedesc);
    *written_out = (uint64_t)n;
    return 0;
}

int32_t vfs_seek(int32_t fd, uint64_t offset, uint32_t whence, uint64_t *pos_out)
{
    if (!pos_out || whence > KMOD_VFS_SEEK_END)
        return -EINVAL;
    proc_t *proc = Schedule::this_proc();
    if (!proc || !proc->FDMan)
        return -EINVAL;
    fd_t *FD = fd_get(proc->FDMan, fd);
    if (!FD || !FD->FSOPS || !FD->filedesc)
        return -EBADF;

    int32_t lr = FD->FSOPS->lseek(FD->filedesc, offset, whence);
    if (lr != 0)
        return lr > 0 ? -lr : lr;
    uint64_t base = (whence == KMOD_VFS_SEEK_SET)   ? 0u
                    : (whence == KMOD_VFS_SEEK_CUR) ? FD->offset
                                                    : FD->file_size;
    uint64_t np = base + offset;
    if (np > FD->file_size)
        np = FD->file_size;
    FD->offset = np;
    *pos_out = np;
    return 0;
}

int32_t vfs_file_size(int32_t fd, uint64_t *size_out)
{
    if (!size_out)
        return -EINVAL;
    proc_t *proc = Schedule::this_proc();
    if (!proc || !proc->FDMan)
        return -EINVAL;
    fd_t *FD = fd_get(proc->FDMan, fd);
    if (!FD || !FD->FSOPS || !FD->filedesc)
        return -EBADF;
    *size_out = FD->FSOPS->fsize(FD->filedesc);
    return 0;
}

/* 契约 ops（.data 内固定布局；static_assert 在契约头里钉死尺寸） */
kmod_vfs_ops_t g_vfs_ops = {
    KMOD_VFS_OPS_VERSION,
    (uint32_t)sizeof(kmod_vfs_ops_t),
    vfs_register_fs,
    vfs_unregister_fs,
    vfs_mount,
    vfs_umount,
    vfs_resolve,
    vfs_open,
    vfs_close,
    vfs_read,
    vfs_write,
    vfs_seek,
    vfs_file_size,
};

} /* namespace */

KCOMPONENT_EXPORT_CONTRACT(fs_vfs, KMOD_CONTRACT_VFS_V1, g_vfs_ops, 1)

/* ==================================================================== */
/* 生命周期                                                              */
/* ==================================================================== */
static int32_t comp_fs_vfs_init(void)
{
    if (g_vfs_ready)
        return KMOD_OK; /* 幂等 */

    if (HMapS_MP == nullptr) {
        /* VFS 的挂载点表由 kernel.dev.core 的 InitFFMAN() 建立；这里不重复
         * （重复 new 一个 hashmap 会泄漏且让既有挂载点不可见）。 */
        kprintf_("[fs.vfs] init FAILED: mount table (HMapS_MP) is NULL; "
                 "kernel.dev.core must run InitFFMAN() first\n");
        return KMOD_ERR_STATE;
    }
    g_vfs_ready = true;
    kprintf_("[fs.vfs] ready: fs registry %u slots, resolve/open/read/write "
             "contract published (%s)\n",
             (unsigned)KMOD_VFS_MAX_IMPL, KMOD_CONTRACT_VFS_V1);
    return KMOD_OK;
}

static void comp_fs_vfs_exit(void)
{
    spinlock_lock(&g_vfs_reg_lock);
    for (uint32_t i = 0; i < KMOD_VFS_MAX_IMPL; i++) {
        g_vfs_reg[i].used = false;
        _memset(&g_vfs_reg[i].impl, 0, sizeof(g_vfs_reg[i].impl));
    }
    spinlock_unlock(&g_vfs_reg_lock);
    g_vfs_ready = false;
    kprintf_("[fs.vfs] exit: fs registry cleared\n");
}

KCOMPONENT_END(fs_vfs)
