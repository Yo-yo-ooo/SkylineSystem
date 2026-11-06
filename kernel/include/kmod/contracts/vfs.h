//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * kernel/include/kmod/contracts/vfs.h —— 私有契约 kernel.vfs.v1（组件间固定 ABI）
 * ============================================================================
 * 提供者：内建组件 kernel.fs.vfs（kernel/src/arch/x86_64/components/fs_vfs.cpp）
 * 消费者：fs.ext4 / fs.fatfs 内建组件；未来 .ko 形式的 FS 实现/诊断模块。
 *
 * 设计约束（见 docs/kernel-modules.md §4）：
 *   - 本头**只依赖 freestanding <stdint.h>/<stddef.h>**，内建组件与 .ko 共用；
 *   - 所有 ops/描述结构体布局**冻结**：首部固定 { uint32_t version; uint32_t size; }，
 *     后续只用定宽整数与函数指针，并以 static_assert 钉死字节数；
 *   - 语义不进入内核：内核对 CONTRACT 导出只做字符串/版本/尺寸机械校验。
 *
 * 版本策略：任何布局变更必须新增 kernel.vfs.v2（不得原地改 v1 布局）。
 * 错误约定：返回 0 = 成功，负值 = 负 errno（-ENOENT/-EIO/-EINVAL...）。
 * ==========================================================================*/
#ifndef _KMOD_CONTRACTS_VFS_H_
#define _KMOD_CONTRACTS_VFS_H_

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 契约命名空间（导出条目的 symbol 与 contract 字段都等于该字符串：
 * KCOMPONENT_EXPORT_CONTRACT 的线格式约定 —— 消费者 import 时
 * provider="kernel.fs.vfs", symbol="kernel.vfs.v1", contract="kernel.vfs.v1"）。 */
#define KMOD_CONTRACT_VFS_V1        "kernel.vfs.v1"
#define KMOD_CONTRACT_FS_EXT4_V1    "kernel.fs.ext4.v1"
#define KMOD_CONTRACT_FS_FATFS_V1   "kernel.fs.fatfs.v1"
#define KMOD_CONTRACT_FS_ROOTFS_V1  "kernel.fs.rootfs.v1"

#define KMOD_VFS_OPS_VERSION        1u
#define KMOD_VFS_FS_IMPL_VERSION    1u
#define KMOD_VFS_MOUNT_INFO_VERSION 1u
#define KMOD_FS_EXT4_OPS_VERSION    1u
#define KMOD_FS_FATFS_OPS_VERSION   1u
#define KMOD_FS_ROOTFS_OPS_VERSION  1u

#define KMOD_VFS_MAX_IMPL           8u
#define KMOD_VFS_NAME_MAX           16u   /* FS 实现名：ext4 / fatfs */
#define KMOD_VFS_PATH_MAX           512u

/* FSType 数值与 kernel/include/fs/fd.h 的 enum FSType 对齐（内核只透传数值） */
#define KMOD_VFS_FST_UNKNOWN        0u
#define KMOD_VFS_FST_EXT4           1u
#define KMOD_VFS_FST_FATFS          2u

/* 打开 flags（数值与 kernel/include/fs/fd.h 的 O_* 一致，便于内建实现零转换） */
#define KMOD_VFS_O_RDONLY           0x0000u
#define KMOD_VFS_O_WRONLY           0x0001u
#define KMOD_VFS_O_RDWR             0x0002u
#define KMOD_VFS_O_CREAT            0x0040u   /* 0100 */
#define KMOD_VFS_O_TRUNC            0x0200u   /* 01000 */
#define KMOD_VFS_O_APPEND           0x0400u   /* 02000 */

/* seek whence（与 SEEK_SET/CUR/END 一致） */
#define KMOD_VFS_SEEK_SET           0u
#define KMOD_VFS_SEEK_CUR           1u
#define KMOD_VFS_SEEK_END           2u

/* FS 实现能力 flags */
#define KMOD_VFS_IMPL_F_READONLY    (1u << 0)  /* 只读实现 */
#define KMOD_VFS_IMPL_F_MOUNTABLE   (1u << 1)  /* mount() 已接通；未置位仅占位/识别 */
#define KMOD_VFS_IMPL_F_BUILTIN     (1u << 2)  /* 内核内建实现（非 .ko 注册） */

/* ------------------------------------------------------------------ */
/* FS 实现描述：注册到 VFS 注册表，供 vfs.mount 按注册顺序自动挂载。   */
/* 布局冻结：48 字节。                                                */
/* ------------------------------------------------------------------ */
typedef struct kmod_vfs_fs_impl {
    uint32_t version;                       /* = KMOD_VFS_FS_IMPL_VERSION */
    uint32_t size;                          /* = sizeof(kmod_vfs_fs_impl_t) */
    const char *name;                       /* "ext4"/"fatfs"（注册表唯一键） */
    uint32_t fstype;                        /* KMOD_VFS_FST_* */
    uint32_t flags;                         /* KMOD_VFS_IMPL_F_* */
    int32_t (*mount)(const char *dev_name, const char *mount_point,
                     uint32_t wpart);       /* 0 = 成功，负 = 错误 */
    int32_t (*umount)(const char *mount_point);
    uint32_t reserved[2];                   /* 必须为 0 */
} kmod_vfs_fs_impl_t;

/* ------------------------------------------------------------------ */
/* 路径解析结果（vfs.resolve 填充）。布局冻结：48 字节。               */
/* ------------------------------------------------------------------ */
typedef struct kmod_vfs_mount_info {
    uint32_t version;                       /* = KMOD_VFS_MOUNT_INFO_VERSION */
    uint32_t size;                          /* = sizeof(kmod_vfs_mount_info_t) */
    uint32_t fstype;                        /* KMOD_VFS_FST_* */
    uint32_t fs_impl_index;                 /* 注册表下标；0xFFFFFFFFu = 未登记 */
    uint64_t mount_id;                      /* 已挂载文件系统实例的不透明身份 */
    const char *mount_point;                /* 内核持久字符串（只读，如 "/mp/"） */
    void *mp;                               /* FS 私有 mountpoint 指针（不透明） */
    uint32_t flags;
    uint32_t reserved;                      /* 必须为 0 */
} kmod_vfs_mount_info_t;

/* ------------------------------------------------------------------ */
/* VFS 固定契约 ops。布局冻结：96 字节（8 + 11 个函数指针）。          */
/* open/read/write 语义：对当前进程（Schedule::this_proc）的 FD 表操作；*/
/* v1 的 read/write 直通 FS 实现、不经文件缓存（内核 syscall 路径不变）。*/
/* ------------------------------------------------------------------ */
typedef struct kmod_vfs_ops {
    uint32_t version;                       /* = KMOD_VFS_OPS_VERSION */
    uint32_t size;                          /* = sizeof(kmod_vfs_ops_t) */
    int32_t (*register_fs)(const kmod_vfs_fs_impl_t *impl);
    int32_t (*unregister_fs)(const char *name);
    int32_t (*mount)(const char *dev_name, const char *mount_point,
                     uint32_t wpart);       /* 按注册顺序尝试 MOUNTABLE 实现 */
    int32_t (*umount)(const char *mount_point);
    int32_t (*resolve)(const char *path, kmod_vfs_mount_info_t *out);
    int32_t (*open)(const char *path, uint32_t flags, int32_t *fd_out);
    int32_t (*close)(int32_t fd);
    int32_t (*read)(int32_t fd, void *buf, uint64_t count, uint64_t *read_out);
    int32_t (*write)(int32_t fd, const void *buf, uint64_t count,
                     uint64_t *written_out);
    int32_t (*seek)(int32_t fd, uint64_t offset, uint32_t whence,
                    uint64_t *pos_out);
    int32_t (*file_size)(int32_t fd, uint64_t *size_out);
} kmod_vfs_ops_t;

/* ------------------------------------------------------------------ */
/* kernel.fs.ext4.v1：ext4 实现扩展契约（fs.ext4 组件导出）。          */
/* 布局冻结：32 字节。                                                */
/* ------------------------------------------------------------------ */
typedef struct kmod_fs_ext4_ops {
    uint32_t version;                       /* = KMOD_FS_EXT4_OPS_VERSION */
    uint32_t size;                          /* = sizeof(kmod_fs_ext4_ops_t) */
    int32_t (*mount)(const char *dev_name, const char *mount_point,
                     uint32_t wpart);       /* 0 = 成功（lwext4 EOK） */
    int32_t (*umount)(const char *mount_point);
    int32_t (*stats)(const char *mount_point, uint64_t *total_bytes,
                     uint64_t *free_bytes, uint32_t *block_size);
} kmod_fs_ext4_ops_t;

/* ------------------------------------------------------------------ */
/* kernel.fs.fatfs.v1：FatFs 组件契约。                               */
/* v1 现状（诚实声明）：FatFs 只提供分区识别（IdentifyFat）与挂载寄存器*/
/* 登记，VFS mount 未接通（kernel/src/fs/fatfs/diskio.cpp 的盘 I/O glue */
/* 仍是 STA_NOINIT/RES_ERROR 空壳），因此 flags() 不含 MOUNTABLE。      */
/* 布局冻结：32 字节。                                                */
/* ------------------------------------------------------------------ */
typedef struct kmod_fs_fatfs_ops {
    uint32_t version;                       /* = KMOD_FS_FATFS_OPS_VERSION */
    uint32_t size;                          /* = sizeof(kmod_fs_fatfs_ops_t) */
    /* 等价 IdentifyFat(): 成功返回 0，并写 type/error（见 partition/identfstype.h） */
    int32_t (*identify)(uint32_t dev_type, uint32_t dev_index,
                        uint32_t partition_id, uint32_t use_virt_image,
                        uint32_t *type_out, uint32_t *error_out);
    int32_t (*mount)(const char *dev_name, const char *mount_point,
                     uint32_t wpart);       /* v1 恒返回 -ENOTSUP */
    uint32_t (*flags)(void);                /* KMOD_VFS_IMPL_F_* */
} kmod_fs_fatfs_ops_t;

/* ------------------------------------------------------------------ */
/* kernel.fs.rootfs.v1：根文件系统状态（fs.rootfs 组件导出）。        */
/* 布局冻结：32 字节。                                                */
/* ------------------------------------------------------------------ */
typedef struct kmod_fs_rootfs_ops {
    uint32_t version;                       /* = KMOD_FS_ROOTFS_OPS_VERSION */
    uint32_t size;                          /* = sizeof(kmod_fs_rootfs_ops_t) */
    int32_t (*mounted)(void);               /* 1 = 已挂载，0 = 未挂载 */
    const char *(*device)(void);            /* 根设备名（静态字符串）或 NULL */
    const char *(*mount_point)(void);       /* 根挂载点（静态字符串）或 NULL */
} kmod_fs_rootfs_ops_t;

/* ---------------- 布局冻结自检（内建组件与 .ko 共用同一头） ---------------- */
#if defined(__cplusplus)
static_assert(sizeof(kmod_vfs_fs_impl_t) == 48u,
              "kmod_vfs_fs_impl_t layout must stay 48 bytes (kernel.vfs.v1)");
static_assert(sizeof(kmod_vfs_mount_info_t) == 48u,
              "kmod_vfs_mount_info_t layout must stay 48 bytes (kernel.vfs.v1)");
static_assert(sizeof(kmod_vfs_ops_t) == 96u,
              "kmod_vfs_ops_t layout must stay 96 bytes (kernel.vfs.v1)");
static_assert(sizeof(kmod_fs_ext4_ops_t) == 32u,
              "kmod_fs_ext4_ops_t layout must stay 32 bytes (kernel.fs.ext4.v1)");
static_assert(sizeof(kmod_fs_fatfs_ops_t) == 32u,
              "kmod_fs_fatfs_ops_t layout must stay 32 bytes (kernel.fs.fatfs.v1)");
static_assert(sizeof(kmod_fs_rootfs_ops_t) == 32u,
              "kmod_fs_rootfs_ops_t layout must stay 32 bytes (kernel.fs.rootfs.v1)");
#endif

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* _KMOD_CONTRACTS_VFS_H_ */
