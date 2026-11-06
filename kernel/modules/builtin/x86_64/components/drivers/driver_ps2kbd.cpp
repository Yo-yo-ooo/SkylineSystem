//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * driver_ps2kbd.cpp —— 内建组件 kernel.input.ps2.kbd (Tier1 POLICY)
 * ----------------------------------------------------------------------------
 * 迁移自 init.cpp line 239：InitFunc("KEYBOARD(x86)", keyboard_init())。
 * PS/2 不支持热插拔探测（ISA 风格控制器），因此 init 即执行原键盘初始化：
 * 控制器配置 + IRQ33 路由 + Kbd 输入核心注册（Kbd::Init -> Dev::AddDevice）。
 * 组件依赖 kernel.arch.apic（IOAPIC::RemapIRQ 需要 APIC 就绪）与
 * kernel.dev.core（Kbd::Init 往 Dev 注册输入设备）。
 * 失败策略：PS/2 控制器/键盘缺失时保持 KMOD_OK（QEMU 环境差异），仅结构错误
 * （契约/依赖未绑定）才返回错误。
 * ==========================================================================*/
#include <kmod/kmod_component.h>

#include <drivers/keyboard/x86/keyboard.h>

static int32_t driver_ps2kbd_init(void);
static void driver_ps2kbd_exit(void);

KCOMPONENT_BEGIN(driver_ps2kbd, "kernel.input.ps2.kbd", "1.0.0",
                 KMOD_TIER_POLICY, KMOD_MF_BOOT,
                 (uint64_t)(KMOD_CAP_IRQ | KMOD_CAP_DEVICE), 0,
                 "PS/2 keyboard: controller init + IRQ33 + shared input core (Kbd::Init)",
                 driver_ps2kbd_init, driver_ps2kbd_exit, 0, 0, 5000u, 1000u)

KCOMPONENT_IMPORT_FN(driver_ps2kbd, kprintf_, int, (const char *, ...),
                     "kernel", "kprintf_", "kernel.log.v1")
KCOMPONENT_DEPENDS(driver_ps2kbd, "kernel.arch.apic", apic)
KCOMPONENT_DEPENDS(driver_ps2kbd, "kernel.dev.core", dev_core)

static int32_t driver_ps2kbd_init(void)
{
    kprintf_("[comp.ps2.kbd] init: PS/2 keyboard controller + IRQ33 (legacy path)\n");
    /* 与旧 init.cpp 完全相同的一次性初始化；函数无返回值。 */
    keyboard_init();
    kprintf_("[comp.ps2.kbd] init done: legacy PS/2 keyboard path armed\n");
    return KMOD_OK;
}

static void driver_ps2kbd_exit(void)
{
    kprintf_("[comp.ps2.kbd] exit: PS/2 controller left as-is (no hot-remove)\n");
}

KCOMPONENT_END(driver_ps2kbd)
