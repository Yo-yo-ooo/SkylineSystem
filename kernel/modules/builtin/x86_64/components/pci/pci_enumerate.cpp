//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * pci_enumerate.cpp —— 内建组件 kernel.pci.enumerate (Tier1 POLICY)
 * ----------------------------------------------------------------------------
 * 两阶段 PCI 初始化的第二阶段（迁移自原 init.cpp line 231-236 的枚举动作）：
 *   provider 先序保证本组件 init 时：
 *     kernel.pci.core 已 READY 并导出 kernel.pci.v1；
 *     kernel.driver.{ahci,nvme,xhci,e1000} 已通过 pci.v1 register_driver()
 *     把 match tuple 登记进 PCI 核心（只登记，不访问设备）。
 *   本组件 init 只调用 pci.v1 ops->enumerate()，由 provider 按 ACPI MCFG
 *   分支执行 EnumeratePCI/DoPCIWithoutMCFG，命中 match 的 probe 即刻执行。
 *
 * 依赖边（显式，禁止靠链接顺序碰运气）：
 *   kernel.pci.core         （import pci.v1 -> 自动依赖边）
 *   kernel.driver.ahci / nvme / xhci / e1000 （KCOMPONENT_DEPENDS）
 * ==========================================================================*/
#include <kmod/kmod_component.h>
#include <kmod/contracts/pci.h>

static int32_t pci_enumerate_init(void);
static void pci_enumerate_exit(void);

KCOMPONENT_BEGIN(pci_enumerate, "kernel.pci.enumerate", "1.0.0",
                 KMOD_TIER_POLICY, KMOD_MF_BOOT,
                 (uint64_t)(KMOD_CAP_MMIO | KMOD_CAP_IRQ), 0,
                 "PCI two-phase enumeration trigger (after all PCI drivers registered)",
                 pci_enumerate_init, pci_enumerate_exit, 0, 0, 30000u, 1000u)

KCOMPONENT_IMPORT_FN(pci_enumerate, kprintf_, int, (const char *, ...),
                     "kernel", "kprintf_", "kernel.log.v1")
KCOMPONENT_IMPORT(pci_enumerate, pci_ops, kmod_pci_ops_t *, "kernel.pci.core",
                  "kernel.pci.v1", "kernel.pci.v1")
/* 第二阶段必须晚于全部驱动组件的注册动作。 */
KCOMPONENT_DEPENDS(pci_enumerate, "kernel.driver.ahci", drv_ahci)
KCOMPONENT_DEPENDS(pci_enumerate, "kernel.driver.nvme", drv_nvme)
KCOMPONENT_DEPENDS(pci_enumerate, "kernel.driver.xhci", drv_xhci)
KCOMPONENT_DEPENDS(pci_enumerate, "kernel.driver.e1000", drv_e1000)

static int32_t pci_enumerate_init(void)
{
    if (!pci_ops || !pci_ops->enumerate) {
        kprintf_("[comp.pci.enumerate] FAILED: kernel.pci.v1 not bound\n");
        return KMOD_ERR_UNRESOLVED;
    }
    if (pci_ops->ops_size != (uint32_t)sizeof(kmod_pci_ops_t)) {
        kprintf_("[comp.pci.enumerate] FAILED: ops size %u != %u\n",
                 (unsigned)pci_ops->ops_size,
                 (unsigned)sizeof(kmod_pci_ops_t));
        return KMOD_ERR_CONTRACT;
    }
    if (pci_ops->abi_version != KMOD_PCI_ABI_V1) {
        kprintf_("[comp.pci.enumerate] FAILED: abi %u != %u\n",
                 (unsigned)pci_ops->abi_version, (unsigned)KMOD_PCI_ABI_V1);
        return KMOD_ERR_VERSION;
    }
    int32_t rc = pci_ops->enumerate();
    if (rc != KMOD_OK) {
        kprintf_("[comp.pci.enumerate] enumerate failed rc=%d\n", (int)rc);
        return rc;
    }
    kprintf_("[comp.pci.enumerate] two-phase enumeration complete\n");
    return KMOD_OK;
}

static void pci_enumerate_exit(void)
{
    /* 枚举不可逆（设备/驱动已逐一出 probe），v1 退出只记录审计。 */
    kprintf_("[comp.pci.enumerate] exit: enumeration is one-way in v1\n");
}

KCOMPONENT_END(pci_enumerate)
