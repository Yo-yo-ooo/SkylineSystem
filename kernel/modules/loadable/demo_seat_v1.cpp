//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * demo_seat_v1.cpp —— kmod 示例：随机数座位(seat)提供者 v1（非 BOOT，池内）
 * ----------------------------------------------------------------------------
 * 座位 ABI 由 Tier0 内核定义（<kmod/kmod_seats.h>），模块逐字段匹配：
 *   abi_version = KMOD_SEAT_RANDOM_ABI(1)
 *   ops_size    = sizeof(kmod_seat_random_ops_t)（内核硬校验）
 *   u64()       = 固定序列：0x1111111111111111 起，每次 +1
 *   fill()      = 用 u64() 的字节填充
 *
 * 本模块不设 KMOD_MF_BOOT：由 selftest 按名加载，用于验证座位安装/热替换。
 * v1/v2 同模块名 "vendor.demo.prng" 不同版本；selftest 用返回值高 4 位
 * 判断当前生效实现（v1 高 4 位 = 1，v2 = 2）。
 * ==========================================================================*/
#include <kmod/kmod_module.h>
#include <kmod/kmod_seats.h>

#include <stdint.h>

KMOD_IMPORT_FN(kprintf_, int, (const char *, ...), "kernel", "kprintf_",
               "kernel.log.v1");

/* v1 固定序列：0x1111111111111111 + n。 */
static uint64_t prng_state = 0;

static uint64_t prng_u64(void)
{
    return 0x1111111111111111ull + prng_state++;
}

static void prng_fill(uint8_t *buf, uint64_t len)
{
    if (!buf)
        return;
    for (uint64_t i = 0; i < len; i++)
        buf[i] = (uint8_t)(prng_u64() >> 8);
}

/* 座位 ops（非 const：.out 段地址回填后由内核对 ops 做只读读取，
 * 但模块侧声明保持普通 static 对象，避免把 reloc 目标放进只读段）。 */
static kmod_seat_random_ops_t prng_ops = {
    KMOD_SEAT_RANDOM_ABI,
    (uint32_t)sizeof(kmod_seat_random_ops_t),
    prng_u64,
    prng_fill,
};

static void prng_quiesce(void);
static void prng_exit(void);

static int32_t prng_init(void)
{
    kprintf_("[demo.prng] v1 active\n");
    return KMOD_OK;
}

static void prng_quiesce(void)
{
    /* v1 无 in-flight 状态：静默即完成。 */
}

static void prng_exit(void)
{
    kprintf_("[demo.prng] exit\n");
}

KMOD_EXPORT_SEAT(KMOD_SEAT_RANDOM, prng_ops, KMOD_SEAT_RANDOM_ABI);

KMOD_MANIFEST_FULL("vendor.demo.prng", "1.0.0", KMOD_TIER_POLICY,
                   (uint32_t)(KMOD_MF_HOTREPLACE | KMOD_MF_UNLOADABLE),
                   (uint64_t)(KMOD_CAP_SEAT_PROVIDER | KMOD_CAP_HOT_REPLACE |
                              KMOD_CAP_LOG),
                   0, "demo: random seat provider v1", prng_init, prng_exit,
                   prng_quiesce, 0, 500u, 200u);
