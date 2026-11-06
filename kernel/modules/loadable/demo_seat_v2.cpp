//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * demo_seat_v2.cpp —— kmod 示例：随机数座位(seat)提供者 v2（非 BOOT，池内）
 * ----------------------------------------------------------------------------
 * 与 v1 同模块名 "vendor.demo.prng"，版本 2.0.0，用于验证热替换：
 *   加载 v1 → 调用座位得到高 4 位 = 1 → hotreplace v2 → 高 4 位 = 2。
 * v2 固定序列：0x2222222222222222 起，每次 +1。
 * ==========================================================================*/
#include <kmod/kmod_module.h>
#include <kmod/kmod_seats.h>

#include <stdint.h>

KMOD_IMPORT_FN(kprintf_, int, (const char *, ...), "kernel", "kprintf_",
               "kernel.log.v1");

/* v2 固定序列：0x2222222222222222 + n。 */
static uint64_t prng_state = 0;

static uint64_t prng_u64(void)
{
    return 0x2222222222222222ull + prng_state++;
}

static void prng_fill(uint8_t *buf, uint64_t len)
{
    if (!buf)
        return;
    for (uint64_t i = 0; i < len; i++)
        buf[i] = (uint8_t)(prng_u64() >> 8);
}

static kmod_seat_random_ops_t prng_ops = {
    KMOD_SEAT_RANDOM_ABI,
    (uint32_t)sizeof(kmod_seat_random_ops_t),
    prng_u64,
    prng_fill,
};

static int32_t prng_init(void)
{
    kprintf_("[demo.prng] v2 active\n");
    return KMOD_OK;
}

static void prng_quiesce(void)
{
    /* v2 无 in-flight 状态：静默即完成。 */
}

static void prng_exit(void)
{
    kprintf_("[demo.prng] exit\n");
}

KMOD_EXPORT_SEAT(KMOD_SEAT_RANDOM, prng_ops, KMOD_SEAT_RANDOM_ABI);

KMOD_MANIFEST_FULL("vendor.demo.prng", "2.0.0", KMOD_TIER_POLICY,
                   (uint32_t)(KMOD_MF_HOTREPLACE | KMOD_MF_UNLOADABLE),
                   (uint64_t)(KMOD_CAP_SEAT_PROVIDER | KMOD_CAP_HOT_REPLACE |
                              KMOD_CAP_LOG),
                   0, "demo: random seat provider v2", prng_init, prng_exit,
                   prng_quiesce, 0, 500u, 200u);
