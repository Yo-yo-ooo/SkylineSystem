//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * block_core.cpp —— 内建组件 kernel.block.core (Tier1 POLICY)
 * ----------------------------------------------------------------------------
 * 迁移/包装现有 Dev 块设备子系统（kernel/src/drivers/dev/dev.cpp），导出
 * 私有契约 kernel.block.v1 给文件系统组件（B4 的 kernel.fs.rootfs）使用：
 *   - find_by_name(name)      <=> Dev::GetSDEV(name)
 *   - find_by_type(type, idx) <=> Dev::GetSDEV((VsDevType)type, idx)
 *   - register_device(desc)   <=> Dev::AddStorageDevice(...) + DevOPS 适配
 *   - read/write              <=> Dev::Read/Dev::Write（扇区，1=OK/0=失败）
 *   - capacity_sectors/sector_size/name/type <= VDL 字段
 *
 * 行为保持：现有驱动（AHCI/NVMe/USB MSC）仍然在 probe 里直接调
 * Dev::AddStorageDevice 注册设备，本组件不改写这条路径；契约只是同一个
 * Dev 注册表的只读视图 + 未来的契约化注册入口。设备不存在不报错。
 * ==========================================================================*/
#include <kmod/kmod_component.h>
#include <kmod/contracts/block.h>

#include <drivers/dev/dev.h>

static int32_t block_core_init(void);
static void block_core_exit(void);

KCOMPONENT_BEGIN(block_core, "kernel.block.core", "1.0.0", KMOD_TIER_POLICY,
                 KMOD_MF_BOOT, 0, 0,
                 "Block device registry over Dev: find/enumerate/read/write (kernel.block.v1)",
                 block_core_init, block_core_exit, 0, 0, 1000u, 200u)

KCOMPONENT_IMPORT_FN(block_core, kprintf_, int, (const char *, ...), "kernel",
                     "kprintf_", "kernel.log.v1")
/* Dev 子系统必须已由 kernel.dev.core 初始化（Dev::Init 建 hashmap）。 */
KCOMPONENT_DEPENDS(block_core, "kernel.dev.core", dev_core)
KCOMPONENT_DEPENDS(block_core, "kernel.pci.core", pci_core)

namespace {

constexpr uint32_t kMaxRegistered = KMOD_BLOCK_REGISTERED_MAX;
constexpr uint32_t kMaxScanIndex = 64u;

/* 通过契约 register_device() 注册的设备：保存 desc 副本，DevOPS 适配器以
 * 本结构地址作为 Dev 层的 classp（Dev::Read/Write 会把 classp 原样传回）。 */
struct RegBlock {
    kmod_block_device_desc_t desc;
    bool used;
};

RegBlock g_regs[kMaxRegistered];
uint32_t g_registered = 0;

uint8_t adapt_read(void *classp, uint64_t lba, uint32_t sector_count, void *buf)
{
    RegBlock *r = static_cast<RegBlock *>(classp);
    if (!r || !r->used || !r->desc.read)
        return 0;
    return r->desc.read(r->desc.ctx, lba, sector_count, buf);
}

uint8_t adapt_write(void *classp, uint64_t lba, uint32_t sector_count, void *buf)
{
    RegBlock *r = static_cast<RegBlock *>(classp);
    if (!r || !r->used || !r->desc.write)
        return 0;
    return r->desc.write(r->desc.ctx, lba, sector_count, static_cast<const void *>(buf));
}

void *blk_find_by_name(const char *name)
{
    if (!name || name[0] == '\0')
        return nullptr;
    return static_cast<void *>(Dev::GetSDEV(name));
}

void *blk_find_by_type(uint32_t dev_type, uint32_t index)
{
    if (dev_type > (uint32_t)MAX_TYPE_C)
        return nullptr;
    return static_cast<void *>(Dev::GetSDEV(static_cast<VsDevType>(dev_type), index));
}

/* Dev 按 type 维护连续 index（AddStorageDevice 取 max+1），用于契约化注册后
 * 反查生成的设备名/句柄。 */
uint32_t next_index_of_type(uint32_t dev_type)
{
    uint32_t n = 0;
    while (n < kMaxScanIndex &&
           Dev::GetSDEV(static_cast<VsDevType>(dev_type), n) != nullptr)
        n++;
    return n;
}

void *blk_register_device(const kmod_block_device_desc_t *desc)
{
    if (!desc || !desc->read)
        return nullptr;
    if (desc->type > (uint32_t)MAX_TYPE_C) /* 与 Dev::AddStorageDevice 的接受范围一致 */
        return nullptr;
    if (g_registered >= kMaxRegistered)
        return nullptr;
    RegBlock *slot = nullptr;
    for (uint32_t i = 0; i < kMaxRegistered; i++) {
        if (!g_regs[i].used) {
            slot = &g_regs[i];
            break;
        }
    }
    if (!slot)
        return nullptr;

    slot->desc = *desc;
    slot->used = true;
    g_registered++;

    uint32_t index = next_index_of_type(desc->type);
    DevOPS ops = {};
    ops.Read = adapt_read;
    if (desc->write)
        ops.Write = adapt_write;
    Dev::AddStorageDevice(static_cast<VsDevType>(desc->type), ops,
                          desc->sector_count, slot);
    VDL *dev = Dev::GetSDEV(static_cast<VsDevType>(desc->type), index);
    if (!dev) {
        /* 注册失败（Dev 表满/OOM）：回收槽位，契约返回失败。 */
        slot->used = false;
        g_registered--;
        return nullptr;
    }
    return static_cast<void *>(dev);
}

uint8_t blk_read(void *dev, uint64_t lba, uint32_t sector_count, void *buf)
{
    if (!dev)
        return 0;
    return Dev::Read(static_cast<VDL *>(dev), lba, sector_count, buf);
}

uint8_t blk_write(void *dev, uint64_t lba, uint32_t sector_count, const void *buf)
{
    if (!dev)
        return 0;
    return Dev::Write(static_cast<VDL *>(dev), lba, sector_count,
                      const_cast<void *>(buf));
}

uint64_t blk_capacity_sectors(void *dev)
{
    if (!dev)
        return 0;
    return static_cast<VDL *>(dev)->MaxSectorCount;
}

uint32_t blk_sector_size(void *dev)
{
    (void)dev;
    /* Dev 层历史约定按 512B 扇区计算（Dev::ReadBytes/WriteBytes 等），
       且 VDL.SectorSize 未初始化，故契约 v1 固定返回 512。 */
    return KMOD_BLOCK_SECTOR_SIZE_DEFAULT;
}

const char *blk_name(void *dev)
{
    if (!dev)
        return nullptr;
    return static_cast<VDL *>(dev)->Name;
}

uint32_t blk_type(void *dev)
{
    if (!dev)
        return 0xFFFFFFFFu;
    return static_cast<uint32_t>(static_cast<VDL *>(dev)->type);
}

} /* namespace */

static kmod_block_ops_t g_block_ops = {
    KMOD_BLOCK_ABI_V1,
    (uint32_t)sizeof(kmod_block_ops_t),
    blk_find_by_name,
    blk_find_by_type,
    blk_register_device,
    blk_read,
    blk_write,
    blk_capacity_sectors,
    blk_sector_size,
    blk_name,
    blk_type,
};

static int32_t block_core_init(void)
{
    kprintf_("[comp.block.core] init: contract=%s abi=%u registered_slots=%u\n",
             KMOD_CONTRACT_BLOCK_V1, (unsigned)KMOD_BLOCK_ABI_V1,
             (unsigned)kMaxRegistered);
    /* 只提供契约视图；设备注册仍由驱动 probe（Dev::AddStorageDevice）完成。 */
    return KMOD_OK;
}

static void block_core_exit(void)
{
    kprintf_("[comp.block.core] exit: Dev registry stays owned by kernel.dev.core\n");
}

KCOMPONENT_EXPORT_CONTRACT(block_core, KMOD_CONTRACT_BLOCK_V1, g_block_ops,
                           KMOD_BLOCK_ABI_V1)

KCOMPONENT_END(block_core)
