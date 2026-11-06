//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * driver_xhci.cpp —— 内建组件 kernel.driver.xhci (Tier1 POLICY)
 * ----------------------------------------------------------------------------
 * 迁移自 init.cpp：pci_probe_xhci() + kXhciDriver match tuple（line 113/120-121）。
 * match tuple: class=0x0C, subclass=0x03, progif=0x30（USB xHCI）。
 * 两阶段：init 只登记 match；probe（XHCI::InitXHCIFromPCI(h)）在枚举阶段执行。
 *
 * 说明：xHCI 下游的 USB MSC/HID 枚举发生在 InitXHCIFromPCI 内部（原 init.cpp
 * 注释明确 "USB MSC follows automatically: once the xHCI function probes, its
 * downstream enumeration registers mass-storage LUNs"），因此 B3 不额外拆
 * kernel.driver.usb 组件（拆分会改变行为/重复枚举）。
 * ==========================================================================*/
#include <kmod/kmod_component.h>
#include <kmod/contracts/pci.h>

#include <arch/x86_64/dev/pci/pci.h>
#include <drivers/usb/xhci.h>

static int32_t driver_xhci_init(void);
static void driver_xhci_exit(void);

KCOMPONENT_BEGIN(driver_xhci, "kernel.driver.xhci", "1.0.0", KMOD_TIER_POLICY,
                 KMOD_MF_BOOT,
                 (uint64_t)(KMOD_CAP_MMIO | KMOD_CAP_IRQ | KMOD_CAP_DEVICE), 0,
                 "xHCI: PCI match registration, probe inits controller + downstream USB devices",
                 driver_xhci_init, driver_xhci_exit, 0, 0, 10000u, 1000u)

KCOMPONENT_IMPORT_FN(driver_xhci, kprintf_, int, (const char *, ...), "kernel",
                     "kprintf_", "kernel.log.v1")
KCOMPONENT_IMPORT(driver_xhci, pci_ops, kmod_pci_ops_t *, "kernel.pci.core",
                  "kernel.pci.v1", "kernel.pci.v1")
/* 下游 USB MSC 会经 Dev 注册块设备 -> 需要 kernel.dev.core 先初始化。 */
KCOMPONENT_DEPENDS(driver_xhci, "kernel.dev.core", dev_core)

/* 旧 init.cpp pci_probe_xhci 原样迁移。 */
static void xhci_pci_probe(void *pci_device)
{
    PCI::PCIHeader0 *h = static_cast<PCI::PCIHeader0 *>(pci_device);
    if (!h)
        return;
    XHCI::InitXHCIFromPCI(h);
}

static const kmod_pci_driver_desc_t xhci_driver_desc = {
    "xHCI", 0u, 0u, 0x0Cu, 0x03u, 0x30u, 0u, xhci_pci_probe,
};

static int32_t driver_xhci_init(void)
{
    if (!pci_ops || !pci_ops->register_driver)
        return KMOD_ERR_UNRESOLVED;
    if (pci_ops->ops_size != (uint32_t)sizeof(kmod_pci_ops_t))
        return KMOD_ERR_CONTRACT;
    if (pci_ops->abi_version != KMOD_PCI_ABI_V1)
        return KMOD_ERR_VERSION;
    int32_t rc = pci_ops->register_driver(&xhci_driver_desc);
    if (rc != KMOD_OK) {
        kprintf_("[comp.driver.xhci] register_driver failed rc=%d\n", (int)rc);
        return rc;
    }
    kprintf_("[comp.driver.xhci] PCI match registered (0C/03/30); "
             "probe deferred to kernel.pci.enumerate\n");
    return KMOD_OK;
}

static void driver_xhci_exit(void)
{
    kprintf_("[comp.driver.xhci] exit: hot-remove unsupported in v1\n");
}

KCOMPONENT_END(driver_xhci)
