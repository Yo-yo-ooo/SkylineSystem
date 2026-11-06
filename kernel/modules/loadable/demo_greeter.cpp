//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * demo_greeter.cpp —— kmod 示例：私有契约消费者（BOOT）
 * ----------------------------------------------------------------------------
 * 通过 KMOD_IMPORT 以**函数指针 typedef** 导入 hello 模块的 hello_fn：
 *   provider = "vendor.demo.hello", symbol = "hello_fn",
 *   contract = "vendor.demo.hello.v1"（模块间私下约定，内核只做机械回填）。
 * 内核只按依赖图先加载 provider（hello），再把 hello_fn 地址写进本模块的
 * 导入槽；本模块 init 里调用 hello_fn(1) 断言 == 1001。
 *
 * 能力声明 required = LOG | MEM_ALLOC（LOG 用于打印；MEM_ALLOC 是演示
 * 治理层审计的声明，本文件并未真正调用分配器）。
 * ==========================================================================*/
#include <kmod/kmod_module.h>

#include <stdint.h>

/* 模块间私有契约的消费侧类型（必须与提供者 demo_hello.cpp 的 hello_fn 匹配）。 */
typedef int32_t (*hello_fn_t)(int32_t);

KMOD_IMPORT_FN(kprintf_, int, (const char *, ...), "kernel", "kprintf_",
               "kernel.log.v1");

KMOD_IMPORT(hello_fn, hello_fn_t, "vendor.demo.hello", "hello_fn",
            "vendor.demo.hello.v1");

static int32_t greeter_init(void);
static void greeter_exit(void);

static int32_t greeter_init(void)
{
    if (!KMOD_IMPORT_READY(hello_fn))
        return KMOD_ERR_INVAL;

    int32_t r = hello_fn(1);
    kprintf_("[demo.greeter] hello_fn -> %d\n", (int)r);
    if (r != 1001)
        return KMOD_ERR_INVAL;
    return KMOD_OK;
}

static void greeter_exit(void)
{
    kprintf_("[demo.greeter] exit\n");
}

KMOD_MANIFEST_FULL("vendor.demo.greeter", "1.0.0", KMOD_TIER_IMPL,
                   (uint32_t)(KMOD_MF_BOOT | KMOD_MF_UNLOADABLE),
                   (uint64_t)(KMOD_CAP_LOG | KMOD_CAP_MEM_ALLOC), 0,
                   "demo: hello consumer (boot)", greeter_init, greeter_exit,
                   0, 0, 0u, 0u);
