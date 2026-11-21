//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
// SPDX-License-Identifier: GPL-2.0-only
/* demo_xmod.cpp —— 跨模块 ELF 符号解析消费者（BOOT）
 *
 * 不经过 KMOD_IMPORT 函数指针，直接声明 extern "C" hello_fn；加载器在
 * kernel .kout 未命中时扫描已 READY 组件的 .out，把 hello_fn 的绝对地址
 * 回填到本 .ko 的重定位位置。KMOD_DEPENDS 保证 provider 先加载。
 */
#include <kmod/kmod_module.h>

#include <stdint.h>

KMOD_IMPORT_FN(kprintf_, int, (const char *, ...), "kernel", "kprintf_",
               "kernel.log.v1");

/* demo_hello.cpp 以 extern "C" 导出；这是真正的跨 .ko 符号引用。 */
extern "C" int32_t hello_fn(int32_t x);

/* 纯加载顺序依赖：provider 必须先 READY，符号解析才有地址。 */
KMOD_DEPENDS("vendor.demo.hello", dep_hello);

#define XMOD_MAGIC 0x584D4F440001ULL
static uint64_t xmod_magic = 0;

static int32_t xmod_init(void)
{
    int32_t r = hello_fn(1);
    kprintf_("[demo.xmod] cross-ko hello_fn -> %d\n", (int)r);
    if (r != 1001)
        return KMOD_ERR_INVAL;
    xmod_magic = XMOD_MAGIC;
    return KMOD_OK;
}

KMOD_EXPORT_DATA(xmod_magic, "vendor.demo.xmod.v1", 1u, sizeof(xmod_magic));

static void xmod_exit(void)
{
    kprintf_("[demo.xmod] exit\n");
}

KMOD_MANIFEST_FULL("vendor.demo.xmod", "1.0.0", KMOD_TIER_IMPL,
                   (uint32_t)(KMOD_MF_BOOT | KMOD_MF_UNLOADABLE),
                   KMOD_CAP_LOG, 0,
                   "demo: cross-.ko ELF symbol consumer", xmod_init, xmod_exit,
                   0, 0, 0u, 0u);
