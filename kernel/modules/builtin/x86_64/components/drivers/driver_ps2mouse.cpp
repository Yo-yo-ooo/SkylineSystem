//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * driver_ps2mouse.cpp —— 内建组件 kernel.input.ps2.mouse (Tier1 POLICY)
 * ----------------------------------------------------------------------------
 * 迁移自 init.cpp line 238：InitFunc("PS/2 MOUSE(x86)", ps2_mouse_init())。
 * PS/2 无热插拔探测：init 即执行原鼠标初始化（控制器配置 + IRQ12 路由 +
 * PS/2 设备注册）。依赖 kernel.arch.apic（IOAPIC 路由）与 kernel.dev.core
 * （mouse.cpp -> Dev::AddDevice(PS2MouseDev, ...)）。
 * 设备缺失：ps2_mouse_init() 返回 false（如 QEMU 无 PS/2 鼠标）时按任务约定
 * 返回 KMOD_OK，只记录 present=0；仅结构错误才失败。
 * ==========================================================================*/
#include <kmod/kmod_component.h>

#include <drivers/mouse/x86/ps2mouse.h>

static int32_t driver_ps2mouse_init(void);
static void driver_ps2mouse_exit(void);

KCOMPONENT_BEGIN(driver_ps2mouse, "kernel.input.ps2.mouse", "1.0.0",
                 KMOD_TIER_POLICY, KMOD_MF_BOOT,
                 (uint64_t)(KMOD_CAP_IRQ | KMOD_CAP_DEVICE), 0,
                 "PS/2 mouse: controller init + IRQ12 route + PS/2 device registration",
                 driver_ps2mouse_init, driver_ps2mouse_exit, 0, 0, 5000u, 1000u)

KCOMPONENT_IMPORT_FN(driver_ps2mouse, kprintf_, int, (const char *, ...),
                     "kernel", "kprintf_", "kernel.log.v1")
KCOMPONENT_DEPENDS(driver_ps2mouse, "kernel.arch.apic", apic)
KCOMPONENT_DEPENDS(driver_ps2mouse, "kernel.dev.core", dev_core)

static int32_t driver_ps2mouse_init(void)
{
    /* 与旧 init.cpp 同一次调用；返回值 = 设备是否存在/初始化是否成功。 */
    bool present = ps2_mouse_init();
    kprintf_("[comp.ps2.mouse] init done: present=%d (missing device is not an error)\n",
             (int)present);
    return KMOD_OK;
}

static void driver_ps2mouse_exit(void)
{
    kprintf_("[comp.ps2.mouse] exit: PS/2 controller left as-is (no hot-remove)\n");
}

KCOMPONENT_END(driver_ps2mouse)
