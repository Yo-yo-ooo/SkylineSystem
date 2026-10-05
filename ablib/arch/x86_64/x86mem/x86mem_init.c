//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT
// It may cause fault to build this if X86MEM_NOT_COMPILE_AVX not use
// (alias)
#include "./x86mem.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __x86_64__
#ifdef X86MEM_NOT_COMPILE_AVX

size_t x86mem_cache_limit = X86MEM_CACHE_LIMIT_DEFAULT;

/* x86-64 下 rbx 不是 PIC 保留寄存器, 可以直接当输出操作数用
 * (与 lib/base/arch/x86_64/init.c 里的 cpuid 包装一致)。 */
static inline void xm_cpuid(uint32_t leaf, uint32_t sub,
                            uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
    uint32_t ea, eb, ec, ed;
    asm volatile("cpuid"
                 : "=a"(ea), "=b"(eb), "=c"(ec), "=d"(ed)
                 : "a"(leaf), "c"(sub));
    *a = ea; *b = eb; *c = ec; *d = ed;
}
///
static uint32_t xm_find_l3_subleaf(uint32_t *ea, uint32_t *eb, uint32_t *ec)
{
    uint32_t a, b, c, d, i;

    xm_cpuid(0x80000000u, 0, &a, &b, &c, &d);
    if (a >= 0x8000001Du) {                         /* AMD / 海光 */
        for (i = 0; i < 64u; i++) {
            xm_cpuid(0x8000001Du, i, &a, &b, &c, &d);
            if ((a & 0x1Fu) == 0u) break;           /* type 0 = 枚举结束 */
            if (((a >> 5) & 0x7u) == 3u) {          /* level == 3 */
                *ea = a; *eb = b; *ec = c;
                return 0x8000001Du;
            }
        }
    }

    xm_cpuid(0u, 0, &a, &b, &c, &d);
    if (a >= 4u) {                                  /* Intel */
        for (i = 0; i < 64u; i++) {
            xm_cpuid(4u, i, &a, &b, &c, &d);
            if ((a & 0x1Fu) == 0u) break;
            if (((a >> 5) & 0x7u) == 3u) {
                *ea = a; *eb = b; *ec = c;
                return 4u;
            }
        }
    }

    return 0u;
}

/* size 改为走 helper —— 对 Intel 行为不变, 对 AMD 从"必失败"变可用 */
size_t x86mem_detect_l3_size(void)
{
    uint32_t a, b, c, d;

    if (xm_find_l3_subleaf(&a, &b, &c) != 0u) {
        size_t line = (size_t)((b & 0xFFFu) + 1u);
        size_t part = (size_t)(((b >> 12) & 0x3FFu) + 1u);
        size_t ways = (size_t)(((b >> 22) & 0x3FFu) + 1u);
        size_t sets = (size_t)(c + 1u);
        size_t sz   = ways * part * line * sets;
        if (sz != 0) return sz;
    }

    /* AMD 老 CPU 回退: Fn8000_0006 EDX[31:18], 单位 512KB (K10~Bulldozer;
     * Zen 必命中 0x8000001D, 走不到这)。 */
    xm_cpuid(0x80000000u, 0, &a, &b, &c, &d);
    if (a >= 0x80000006u) {
        xm_cpuid(0x80000006u, 0, &a, &b, &c, &d);
        size_t sz = (size_t)((d >> 18) & 0x3FFFu) * 512u * 1024u;
        if (sz != 0) return sz;
    }

    return 0;    /* 探测失败 */
}

/* ============================================================================
 * 共享当前核心所在 L3 的**逻辑处理器数** (含 SMT 兄弟线程)。0 = 探测不到。
 *   L3 子叶 EAX[25:14] + 1 = 共享该 L3 实例的逻辑处理器数
 *   - Intel: L3 整 package 共享 → package 内逻辑 CPU 数
 *   - AMD:   L3 归属 CCX     → 本 CCX 内逻辑 CPU 数
 * ==========================================================================*/
unsigned x86mem_detect_l3_shared_threads(void)
{
    uint32_t a, b, c;

    if (xm_find_l3_subleaf(&a, &b, &c) == 0u)
        return 0u;

    return (unsigned)(((a >> 14) & 0xFFFu) + 1u);
}

/* ============================================================================
 * 共享当前核心所在 L3 的**物理核心数** (SMT 已折算)。0 = 探测不到。
 * 老 K8/K10 的 Fn8000_0006 只有容量没有拓扑信息, 只能返回 0。
 * ==========================================================================*/
unsigned x86mem_detect_l3_shared_cores(void)
{
    uint32_t a, b, c, d;
    uint32_t threads, tpc, cores;

    threads = x86mem_detect_l3_shared_threads();
    if (threads == 0u)
        return 0u;

    /* SMT 折算: 每 core 线程数 */
    tpc = 1u;
    xm_cpuid(1u, 0u, &a, &b, &c, &d);
    if ((d & (1u << 28)) != 0u) {                   /* HTT=0 必无 SMT */
        xm_cpuid(0u, 0u, &a, &b, &c, &d);
        if (a >= 0xBu) {                            /* leaf 0xB: 扩展拓扑 */
            xm_cpuid(0xBu, 0u, &a, &b, &c, &d);
            if ((((c >> 8) & 0xFFu) == 1u) &&       /* level type == SMT */
                ((b & 0xFFFFu) != 0u))
                tpc = (b & 0xFFFFu);
        }
    }

    if (tpc <= 1u)
        return threads;                             /* 无 SMT: 逻辑数即核数 */

    cores = threads / tpc;
    return (cores != 0u) ? (unsigned)cores : 1u;
}

void x86mem_init_cache_limit(void)
{
    size_t v = x86mem_detect_l3_size();
    size_t c = x86mem_detect_l3_shared_cores();
    if (v == 0) return;                                  /* 保持默认值 */
    if (c == 0) c = 1;

    v = v * 3 / 4 / c;

    if (v < X86MEM_CACHE_LIMIT_MIN) v = X86MEM_CACHE_LIMIT_MIN;
    if (v > X86MEM_CACHE_LIMIT_MAX) v = X86MEM_CACHE_LIMIT_MAX;

    x86mem_cache_limit = v;
}

#endif /* X86MEM_NOT_COMPILE_AVX */
#endif /* __x86_64__ */
