//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * 宿主测试 shim —— MEMOPS_SupportVn 的强定义
 *
 * MEMOPS_SupportV1/V2/V3 在 x86mem 的四个 .c 里都是 weak 定义 (每个 tier 各
 * 出一份; 编译时未启用的 tier 定义为 0)。ELF 下 ld -r / 最终链接都能正确合并
 * 多个 weak 定义; 但 PE-COFF (MinGW) 下 ld -r 会把重复的 weak 定义改名成
 * .weak.<sym>.<file>, 不留全局定义, 最终链接会报 undefined reference (连
 * .debug_info 里的重定位都指向它), 即使没有引用也一样。
 *
 * 因此这里单独用一个不 include x86mem.h 的 TU 给出强定义: 只要 TU 里出现过
 * 一次 weak 声明, GCC 就会把随后的定义也降级为 weak (无论先后顺序), 所以
 * 这个定义必须放在独立文件中。
 *
 * ELF 下强定义覆盖 weak (行为不变); PE-COFF 下补上缺失的定义。
 * 测试二进制总是把 base/avx/avx2/avx512 四个 tier 全部编译进来, 所以恒为 1;
 * 真正挑选哪个 tier 可跑由 CPUID 决定。
 * ==========================================================================*/
#include <stdint.h>

uint8_t MEMOPS_SupportV1 = 1;
uint8_t MEMOPS_SupportV2 = 1;
uint8_t MEMOPS_SupportV3 = 1;
