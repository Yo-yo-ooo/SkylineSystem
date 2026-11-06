//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * driver_e1000.cpp —— 内建组件 kernel.driver.e1000 (Tier1 POLICY)
 * ----------------------------------------------------------------------------
 * 迁移自 init.cpp：pci_probe_e1000() + kE1000Driver match tuple（line 114/122-123）。
 * match tuple: class=0x02, subclass=0x00, progif=0x00（网络控制器）。
 * 两阶段：init 只登记 match；probe（E1000::Init(h)）在枚举阶段执行。
 *
 * netdev 契约：kernel.netdev.v1 由 B4 的 comp-fsnet 定义（provider 组件
 * "kernel.net.netdev"），B3 不重复定义/导出同名契约（全局命名空间会冲突）。
 * 本组件只注册 PCI 驱动；E1000 的收发抽象仍由既有 E1000::Send/SetInputCallback
 * 等直接 API 提供，后续如需接入 netdev 契约，本组件作为**消费者**导入即可。
 * ==========================================================================*/
#include <kmod/kmod_component.h>
#include <kmod/contracts/pci.h>

#include <arch/x86_64/dev/pci/pci.h>
#include <drivers/net/e1000.h>

static int32_t driver_e1000_init(void);
static void driver_e1000_exit(void);

KCOMPONENT_BEGIN(driver_e1000, "kernel.driver.e1000", "1.0.0", KMOD_TIER_POLICY,
                 KMOD_MF_BOOT,
                 (uint64_t)(KMOD_CAP_MMIO | KMOD_CAP_IRQ | KMOD_CAP_NET), 0,
                 "Intel e1000 NIC: PCI match registration, probe inits ring/interrupts",
                 driver_e1000_init, driver_e1000_exit, 0, 0, 10000u, 1000u)

KCOMPONENT_IMPORT_FN(driver_e1000, kprintf_, int, (const char *, ...), "kernel",
                     "kprintf_", "kernel.log.v1")
KCOMPONENT_IMPORT(driver_e1000, pci_ops, kmod_pci_ops_t *, "kernel.pci.core",
                  "kernel.pci.v1", "kernel.pci.v1")

/* 旧 init.cpp pci_probe_e1000 原样迁移。 */
static void e1000_pci_probe(void *pci_device)
{
    PCI::PCIHeader0 *h = static_cast<PCI::PCIHeader0 *>(pci_device);
    if (!h)
        return;
    E1000::Init(h);
}

static const kmod_pci_driver_desc_t e1000_driver_desc = {
    "e1000", 0u, 0u, 0x02u, 0x00u, 0x00u, 0u, e1000_pci_probe,
};

static int32_t driver_e1000_init(void)
{
    if (!pci_ops || !pci_ops->register_driver)
        return KMOD_ERR_UNRESOLVED;
    if (pci_ops->ops_size != (uint32_t)sizeof(kmod_pci_ops_t))
        return KMOD_ERR_CONTRACT;
    if (pci_ops->abi_version != KMOD_PCI_ABI_V1)
        return KMOD_ERR_VERSION;
    int32_t rc = pci_ops->register_driver(&e1000_driver_desc);
    if (rc != KMOD_OK) {
        kprintf_("[comp.driver.e1000] register_driver failed rc=%d\n", (int)rc);
        return rc;
    }
    kprintf_("[comp.driver.e1000] PCI match registered (02/00/00); "
             "probe deferred to kernel.pci.enumerate\n");
    return KMOD_OK;
}

static void driver_e1000_exit(void)
{
    kprintf_("[comp.driver.e1000] exit: hot-remove unsupported in v1\n");
}

KCOMPONENT_END(driver_e1000)
