//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * kernel/include/kmod/contracts/block.h —— 私有契约 kernel.block.v1 (块设备)
 * ============================================================================
 * 提供者组件: kernel.block.core （内建组件，Tier1 POLICY，导出 KMOD_SYM_CONTRACT）
 * 消费者:     kernel.fs.rootfs（B4）等文件系统组件；未来的块设备 .ko
 * 线格式: 纯 C 结构 + 版本/尺寸头字段。
 *
 * 语义（v1，与内建组件 manager / 现有 Dev 子系统对齐）：
 *   - 设备句柄 void* 是 provider 私有不透明句柄（内建实现里 = Dev::VDL*），
 *     只能回传给本 ops 的 read/write/capacity/sector_size/name/type；
 *   - read/write 扇区语义与现有 Dev::Read/Dev::Write 完全一致：
 *     返回 1 = 成功，0 = 失败（不是负错误码；沿用既有块层约定）；
 *   - sector_size 恒为 KMOD_BLOCK_SECTOR_SIZE_DEFAULT(512)（Dev 层历史约定，
 *     Dev::ReadBytes 等按 512 计算；provider 不读取未初始化的 VDL.SectorSize）；
 *   - 设备注册目前仍由各驱动 probe 直接调 Dev::AddStorageDevice（行为不变），
 *     register_device 供未来驱动/模块按契约注册；provider 负责生成设备名。
 * ==========================================================================*/
#ifndef _KMOD_CONTRACT_BLOCK_H_
#define _KMOD_CONTRACT_BLOCK_H_

#include <stdint.h>
#include <stddef.h>
#include <kmod/kmod_abi.h>   /* KMOD_ABI_STATIC_ASSERT */

#define KMOD_CONTRACT_BLOCK_V1         "kernel.block.v1"
#define KMOD_BLOCK_ABI_V1              1u
#define KMOD_BLOCK_REGISTERED_MAX      8u
#define KMOD_BLOCK_SECTOR_SIZE_DEFAULT 512u

/* 设备类型数值与内核 VsDevType 对齐（provider/consumer 都不解释，仅分类/枚举） */
#define KMOD_BLOCK_TYPE_SATA        0u
#define KMOD_BLOCK_TYPE_IDE         1u
#define KMOD_BLOCK_TYPE_NVME        2u
#define KMOD_BLOCK_TYPE_SAS         3u
#define KMOD_BLOCK_TYPE_USB_STORAGE 8u

/* 驱动实现的扇区回调：返回 1 = 成功，0 = 失败（沿用 Dev 块层约定）。 */
typedef uint8_t (*kmod_block_read_fn)(void *ctx, uint64_t lba,
                                      uint32_t sector_count, void *buf);
typedef uint8_t (*kmod_block_write_fn)(void *ctx, uint64_t lba,
                                       uint32_t sector_count, const void *buf);

/* 块设备注册描述（register_device 用）。 */
typedef struct kmod_block_device_desc {
    uint32_t type;             /* KMOD_BLOCK_TYPE_* */
    uint32_t reserved;         /* 必须 0 */
    uint64_t sector_count;     /* 容量（按 sector_size 计） */
    void *ctx;                 /* 透传给 read/write 回调 */
    kmod_block_read_fn read;   /* 必须非 NULL */
    kmod_block_write_fn write; /* 可 NULL（只读设备） */
} kmod_block_device_desc_t;

/* 契约 ops。头两字段是机械校验的版本/尺寸，不得移动。 */
typedef struct kmod_block_ops {
    uint32_t abi_version;      /* = KMOD_BLOCK_ABI_V1 */
    uint32_t ops_size;         /* = sizeof(kmod_block_ops_t) */
    /* 按名查找（"sata0"/"nvme0"/"usb0"...，等价 Dev::GetSDEV）；NULL = 不存在。 */
    void *(*find_by_name)(const char *name);
    /* 按类型+序号枚举（等价 Dev::GetSDEV(type, index)）；NULL = 不存在。 */
    void *(*find_by_type)(uint32_t dev_type, uint32_t index);
    /* 注册块设备；返回设备句柄（可再用于 name/read/...），失败返回 NULL。 */
    void *(*register_device)(const kmod_block_device_desc_t *desc);
    /* 扇区读/写：1 = 成功，0 = 失败。 */
    uint8_t (*read)(void *dev, uint64_t lba, uint32_t sector_count, void *buf);
    uint8_t (*write)(void *dev, uint64_t lba, uint32_t sector_count,
                     const void *buf);
    uint64_t (*capacity_sectors)(void *dev);
    uint32_t (*sector_size)(void *dev);
    const char *(*name)(void *dev);
    uint32_t (*type)(void *dev);
} kmod_block_ops_t;

/* ABI 布局冻结：任何字段变更必须提升 KMOD_BLOCK_ABI_V1 并保留旧结构。 */
KMOD_ABI_STATIC_ASSERT(sizeof(kmod_block_device_desc_t) == 40u,
                       "kmod_block_device_desc_t ABI freeze");
KMOD_ABI_STATIC_ASSERT(sizeof(kmod_block_ops_t) == 80u,
                       "kmod_block_ops_t ABI freeze");

#endif /* _KMOD_CONTRACT_BLOCK_H_ */
