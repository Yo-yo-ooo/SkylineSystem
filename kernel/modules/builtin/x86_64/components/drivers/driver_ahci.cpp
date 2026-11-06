//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * driver_ahci.cpp —— 内建组件 kernel.driver.ahci (Tier1 POLICY)
 * ----------------------------------------------------------------------------
 * 迁移自 init.cpp：pci_probe_ahci() + kAhciDriver match tuple（line 111/116-117）。
 * 本组件 init 只通过 kernel.pci.v1 register_driver() 登记 match：
 *   {"AHCI", vendor=any, device=any, class=0x01, subclass=0x06, progif=0x01}
 * 真正的设备 probe 在 kernel.pci.enumerate 触发枚举时执行（两阶段），
 * probe 代码与旧实现逐字一致：new AHCI::AHCIDriver(&h->Header)。
 * ==========================================================================*/
#include <kmod/kmod_component.h>
#include <kmod/contracts/pci.h>

#include <arch/x86_64/dev/pci/pci.h>
#include <drivers/ahci/ahci.h>

static int32_t driver_ahci_init(void);
static void driver_ahci_exit(void);

KCOMPONENT_BEGIN(driver_ahci, "kernel.driver.ahci", "1.0.0", KMOD_TIER_POLICY,
                 KMOD_MF_BOOT,
                 (uint64_t)(KMOD_CAP_MMIO | KMOD_CAP_IRQ | KMOD_CAP_BLOCK), 0,
                 "AHCI/SATA: PCI match registration; probe creates AHCIDriver via Dev",
                 driver_ahci_init, driver_ahci_exit, 0, 0, 10000u, 1000u)

KCOMPONENT_IMPORT_FN(driver_ahci, kprintf_, int, (const char *, ...), "kernel",
                     "kprintf_", "kernel.log.v1")
KCOMPONENT_IMPORT(driver_ahci, pci_ops, kmod_pci_ops_t *, "kernel.pci.core",
                  "kernel.pci.v1", "kernel.pci.v1")
/* probe 会调用 Dev::AddStorageDevice 注册块设备 -> 必须先有 kernel.dev.core。 */
KCOMPONENT_DEPENDS(driver_ahci, "kernel.dev.core", dev_core)

/* 旧 init.cpp pci_probe_ahci 原样迁移（适配契约的 void* 句柄）。 */
static void ahci_pci_probe(void *pci_device)
{
    PCI::PCIHeader0 *h = static_cast<PCI::PCIHeader0 *>(pci_device);
    if (!h)
        return;
    new AHCI::AHCIDriver(&h->Header);
}

static const kmod_pci_driver_desc_t ahci_driver_desc = {
    "AHCI", 0u, 0u, 0x01u, 0x06u, 0x01u, 0u, ahci_pci_probe,
};

static int32_t driver_ahci_init(void)
{
    if (!pci_ops || !pci_ops->register_driver)
        return KMOD_ERR_UNRESOLVED;
    if (pci_ops->ops_size != (uint32_t)sizeof(kmod_pci_ops_t))
        return KMOD_ERR_CONTRACT;
    if (pci_ops->abi_version != KMOD_PCI_ABI_V1)
        return KMOD_ERR_VERSION;
    int32_t rc = pci_ops->register_driver(&ahci_driver_desc);
    if (rc != KMOD_OK) {
        kprintf_("[comp.driver.ahci] register_driver failed rc=%d\n", (int)rc);
        return rc;
    }
    kprintf_("[comp.driver.ahci] PCI match registered (01/06/01); "
             "probe deferred to kernel.pci.enumerate\n");
    return KMOD_OK;
}

static void driver_ahci_exit(void)
{
    kprintf_("[comp.driver.ahci] exit: hot-remove unsupported in v1\n");
}

KCOMPONENT_END(driver_ahci)
