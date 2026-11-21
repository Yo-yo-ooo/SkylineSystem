//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * driver_nvme.cpp —— 内建组件 kernel.driver.nvme (Tier1 POLICY)
 * ----------------------------------------------------------------------------
 * 迁移自 init.cpp：pci_probe_nvme() + kNvmeDriver match tuple（line 112/118-119）。
 * match tuple: class=0x01, subclass=0x08, progif=0x02（NVMe mass storage）。
 * 两阶段：init 只登记 match；probe（new NVME(h)）在枚举阶段执行，与旧行为一致。
 * ==========================================================================*/
#include <kmod/kmod_component.h>
#include <kmod/contracts/pci.h>

#include <arch/x86_64/dev/pci/pci.h>
#include <drivers/nvme/nvme.h>

static int32_t driver_nvme_init(void);
static void driver_nvme_exit(void);

KCOMPONENT_BEGIN(driver_nvme, "kernel.driver.nvme", "1.0.0", KMOD_TIER_POLICY,
                 KMOD_MF_BOOT,
                 (uint64_t)(KMOD_CAP_MMIO | KMOD_CAP_IRQ | KMOD_CAP_BLOCK), 0,
                 "NVMe: PCI match registration; probe constructs NVMe controller via Dev",
                 driver_nvme_init, driver_nvme_exit, 0, 0, 10000u, 1000u)

KCOMPONENT_IMPORT_FN(driver_nvme, kprintf_, int, (const char *, ...), "kernel",
                     "kprintf_", "kernel.log.v1")
KCOMPONENT_IMPORT(driver_nvme, pci_ops, kmod_pci_ops_t *, "kernel.pci.core",
                  "kernel.pci.v1", "kernel.pci.v1")
KCOMPONENT_DEPENDS(driver_nvme, "kernel.dev.core", dev_core)

/* 旧 init.cpp pci_probe_nvme 原样迁移（原写法 new class NVME(h)）。 */
static void nvme_pci_probe(void *pci_device)
{
    PCI::PCIHeader0 *h = static_cast<PCI::PCIHeader0 *>(pci_device);
    if (!h)
        return;
    new NVME(h);
}

static const kmod_pci_driver_desc_t nvme_driver_desc = {
    "NVMe", 0u, 0u, 0x01u, 0x08u, 0x02u, 0u, nvme_pci_probe,
};

static int32_t driver_nvme_init(void)
{
    if (!pci_ops || !pci_ops->register_driver)
        return KMOD_ERR_UNRESOLVED;
    if (pci_ops->ops_size != (uint32_t)sizeof(kmod_pci_ops_t))
        return KMOD_ERR_CONTRACT;
    if (pci_ops->abi_version != KMOD_PCI_ABI_V1)
        return KMOD_ERR_VERSION;
    int32_t rc = pci_ops->register_driver(&nvme_driver_desc);
    if (rc != KMOD_OK) {
        kprintf_("[comp.driver.nvme] register_driver failed rc=%d\n", (int)rc);
        return rc;
    }
    kprintf_("[comp.driver.nvme] PCI match registered (01/08/02); "
             "probe deferred to kernel.pci.enumerate\n");
    return KMOD_OK;
}

static void driver_nvme_exit(void)
{
    kprintf_("[comp.driver.nvme] exit: hot-remove unsupported in v1\n");
}

KCOMPONENT_END(driver_nvme)
