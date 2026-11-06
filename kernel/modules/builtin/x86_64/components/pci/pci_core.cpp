//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * pci_core.cpp —— 内建组件 kernel.pci.core (Tier1 POLICY)
 * ----------------------------------------------------------------------------
 * 迁移自 x86_64_init() 的 PCI 机制部分（原 init.cpp line 116-123/231-236）：
 *   - 驱动注册表：契约 kernel.pci.v1 的 register_driver / find_device /
 *     device_info / read_bar / config_read32 / config_write32 / enumerate；
 *   - 两阶段模型：驱动组件先登记 match tuple（本组件只包一层 PCI::RegisterDriver，
 *     不带任何枚举副作用）；kernel.pci.enumerate 组件最后调 enumerate() 触发
 *     PCI::EnumeratePCI()/DoPCIWithoutMCFG()，命中 match 的 probe 被即刻调用。
 *
 * 行为保持：probe 时机、match 语义（class/sub/progif 三元组）、无 MCFG 时的
 * legacy 扫描分支都与旧 init.cpp 完全一致；enumerate() 幂等（重复调用直接
 * 返回 OK，不重复枚举）。
 * ==========================================================================*/
#include <kmod/kmod_component.h>
#include <kmod/contracts/pci.h>

#include <acpi/acpi.h>
#include <arch/x86_64/dev/pci/pci.h>

static int32_t pci_core_init(void);
static void pci_core_exit(void);

KCOMPONENT_BEGIN(pci_core, "kernel.pci.core", "1.0.0", KMOD_TIER_POLICY,
                 KMOD_MF_BOOT, (uint64_t)(KMOD_CAP_MMIO | KMOD_CAP_IRQ), 0,
                 "PCI core: driver registry / lookup / BAR+config access / enumerate mechanism",
                 pci_core_init, pci_core_exit, 0, 0, 10000u, 1000u)

KCOMPONENT_IMPORT_FN(pci_core, kprintf_, int, (const char *, ...), "kernel",
                     "kprintf_", "kernel.log.v1")
/* 枚举阶段按 ACPI MCFG 存在性分支；依赖 B1 的 ACPI 组件保证 mcfg 已就绪。 */
KCOMPONENT_DEPENDS(pci_core, "kernel.arch.acpi", acpi)

namespace {

constexpr uint32_t kMaxDrivers = KMOD_PCI_DRIVERS_MAX;

/* 每个注册槽：契约描述符 + 交给旧 PCI 核心的 PCIDriver（地址必须稳定）。 */
struct DrvSlot {
    const kmod_pci_driver_desc_t *desc;
    PCI::PCIDriver pci;
};

DrvSlot g_slots[kMaxDrivers];
uint32_t g_driver_count = 0;
bool g_enumerated = false;

void dispatch_probe(uint32_t index, PCI::PCIHeader0 *h)
{
    const kmod_pci_driver_desc_t *d = g_slots[index].desc;
    if (!d || !d->probe || !h)
        return;
    /* 0 = 任意（现有驱动全部为 0，匹配语义与旧 PCI::ProbeFunction 一致）。 */
    if (d->vendor_id != 0u && h->Header.Vendor_ID != d->vendor_id)
        return;
    if (d->device_id != 0u && h->Header.Device_ID != d->device_id)
        return;
    d->probe(reinterpret_cast<void *>(h));
}

/* 旧 PCI::PCIDriver.probe 是 PCIHeader0*，契约 probe 是 void*；用槽位索引做
 * 适配 thunk（与旧 init.cpp 的 pci_probe_* 包装层等价，只是多一层分派）。 */
#define KMOD_PCI_THUNK(N)                                      \
    void probe_thunk_##N(PCI::PCIHeader0 *h)                   \
    {                                                          \
        dispatch_probe(N, h);                                  \
    }
KMOD_PCI_THUNK(0)
KMOD_PCI_THUNK(1)
KMOD_PCI_THUNK(2)
KMOD_PCI_THUNK(3)
KMOD_PCI_THUNK(4)
KMOD_PCI_THUNK(5)
KMOD_PCI_THUNK(6)
KMOD_PCI_THUNK(7)
KMOD_PCI_THUNK(8)
KMOD_PCI_THUNK(9)
KMOD_PCI_THUNK(10)
KMOD_PCI_THUNK(11)
KMOD_PCI_THUNK(12)
KMOD_PCI_THUNK(13)
KMOD_PCI_THUNK(14)
KMOD_PCI_THUNK(15)
#undef KMOD_PCI_THUNK

static_assert(kMaxDrivers == 16u, "thunk table must match driver slots");

typedef void (*PciProbeThunk)(PCI::PCIHeader0 *);
PciProbeThunk const g_thunks[kMaxDrivers] = {
    probe_thunk_0,  probe_thunk_1,  probe_thunk_2,  probe_thunk_3,
    probe_thunk_4,  probe_thunk_5,  probe_thunk_6,  probe_thunk_7,
    probe_thunk_8,  probe_thunk_9,  probe_thunk_10, probe_thunk_11,
    probe_thunk_12, probe_thunk_13, probe_thunk_14, probe_thunk_15,
};

void zero_bytes(void *p, size_t n)
{
    uint8_t *b = reinterpret_cast<uint8_t *>(p);
    for (size_t i = 0; i < n; i++)
        b[i] = 0;
}

int32_t core_register_driver(const kmod_pci_driver_desc_t *desc)
{
    if (!desc || !desc->name || desc->name[0] == '\0' || !desc->probe)
        return KMOD_ERR_INVAL;
    for (uint32_t i = 0; i < g_driver_count; i++) {
        if (g_slots[i].desc == desc)
            return KMOD_OK; /* 幂等：同一描述符重复登记 */
    }
    if (g_driver_count >= kMaxDrivers)
        return KMOD_ERR_NOMEM;

    uint32_t i = g_driver_count;
    g_slots[i].desc = desc;
    g_slots[i].pci.name = desc->name;
    g_slots[i].pci.cls = desc->class_code;
    g_slots[i].pci.sub = desc->subclass;
    g_slots[i].pci.progif = desc->prog_if;
    g_slots[i].pci.probe = g_thunks[i];
    /* 只登记到旧 PCI 核心的匹配表；不访问设备（两阶段的第一阶段）。 */
    PCI::RegisterDriver(&g_slots[i].pci);
    g_driver_count++;
    kprintf_("[comp.pci.core] driver registered: %s match=%02x/%02x/%02x\n",
             desc->name, (unsigned)desc->class_code, (unsigned)desc->subclass,
             (unsigned)desc->prog_if);
    return KMOD_OK;
}

void *core_find_device(uint8_t class_code, uint8_t subclass, uint8_t prog_if)
{
    return reinterpret_cast<void *>(PCI::FindPCIDev(class_code, subclass, prog_if));
}

int32_t core_device_info(void *pci_device, kmod_pci_device_info_t *out)
{
    if (!pci_device || !out)
        return KMOD_ERR_INVAL;
    PCI::PCIHeader0 *h = static_cast<PCI::PCIHeader0 *>(pci_device);
    zero_bytes(out, sizeof(*out));
    out->vendor_id = h->Header.Vendor_ID;
    out->device_id = h->Header.Device_ID;
    out->revision = h->Header.Revision_ID;
    out->class_code = h->Header.Class;
    out->subclass = h->Header.SubClass;
    out->prog_if = h->Header.Prog_IF;
    out->subsystem_vendor_id = h->SubsystemVendorID;
    out->subsystem_id = h->SubsystemID;
    /* ECAM/MMIO 枚举下句柄地址低位即 bus/slot/function（见 PCI::get_address）。 */
    uint64_t addr = reinterpret_cast<uint64_t>(pci_device);
    out->bus = (uint8_t)((addr >> 20) & 0xFFu);
    out->slot = (uint8_t)((addr >> 15) & 0x1Fu);
    out->function = (uint8_t)((addr >> 12) & 0x07u);
    return KMOD_OK;
}

int32_t core_read_bar(void *pci_device, uint32_t bar_index,
                      kmod_pci_bar_info_t *out)
{
    if (!pci_device || !out || bar_index >= KMOD_PCI_BARS_MAX)
        return KMOD_ERR_INVAL;
    PCI::PCI_BAR_TYPE bar =
        PCI::pci_get_bar(static_cast<PCI::PCIHeader0 *>(pci_device),
                         static_cast<int32_t>(bar_index));
    zero_bytes(out, sizeof(*out));
    out->index = bar_index;
    out->size = bar.size;
    switch (bar.type) {
    case PCI::MMIO32:
        out->type = KMOD_PCI_BAR_MMIO32;
        out->address = bar.mem_address;
        break;
    case PCI::MMIO64:
        out->type = KMOD_PCI_BAR_MMIO64;
        out->address = bar.mem_address;
        break;
    case PCI::IO:
        out->type = KMOD_PCI_BAR_IO;
        out->address = (uint64_t)bar.io_address;
        break;
    default:
        out->type = KMOD_PCI_BAR_NONE;
        break;
    }
    return KMOD_OK;
}

uint32_t core_config_read32(void *pci_device, uint32_t reg)
{
    if (!pci_device)
        return 0xFFFFFFFFu;
    /* PCI::io_read_dword 内部按 &0xFC 对齐并走 CF8/CFC 配置空间访问。 */
    return PCI::io_read_dword(reinterpret_cast<uint64_t>(pci_device),
                              (uint8_t)(reg & 0xFCu));
}

void core_config_write32(void *pci_device, uint32_t reg, uint32_t value)
{
    if (!pci_device)
        return;
    PCI::io_write_dword(reinterpret_cast<uint64_t>(pci_device),
                        (uint8_t)(reg & 0xFCu), value);
}

int32_t core_enumerate(void)
{
    if (g_enumerated) {
        kprintf_("[comp.pci.core] enumerate: already done (idempotent)\n");
        return KMOD_OK;
    }
    g_enumerated = true;
    /* 与旧 init.cpp line 235-236 完全一致的分支与调用。 */
    if (ACPI::mcfg == nullptr) {
        PCI::DoPCIWithoutMCFG();
        kprintf_("[comp.pci.core] enumerate done: legacy CF8/CFC (no MCFG)\n");
        return KMOD_OK;
    }
    PCI::EnumeratePCI(ACPI::mcfg);
    kprintf_("[comp.pci.core] enumerate done: MCFG/ECAM\n");
    return KMOD_OK;
}

} /* namespace */

static kmod_pci_ops_t g_pci_ops = {
    KMOD_PCI_ABI_V1,
    (uint32_t)sizeof(kmod_pci_ops_t),
    core_register_driver,
    core_find_device,
    core_device_info,
    core_read_bar,
    core_config_read32,
    core_config_write32,
    core_enumerate,
};

static int32_t pci_core_init(void)
{
    kprintf_("[comp.pci.core] init: contract=%s abi=%u slots=%u\n",
             KMOD_CONTRACT_PCI_V1, (unsigned)KMOD_PCI_ABI_V1,
             (unsigned)kMaxDrivers);
    /* 本组件只提供机制与契约；枚举由 kernel.pci.enumerate 在两阶段就绪后触发。 */
    return KMOD_OK;
}

static void pci_core_exit(void)
{
    /* v1 不支持 PCI 核心热摘除：PCI 总线/设备表与 Tier0 中断分发共存亡。 */
    kprintf_("[comp.pci.core] exit: PCI core is not removable in v1\n");
}

KCOMPONENT_EXPORT_CONTRACT(pci_core, KMOD_CONTRACT_PCI_V1, g_pci_ops,
                           KMOD_PCI_ABI_V1)

KCOMPONENT_END(pci_core)
