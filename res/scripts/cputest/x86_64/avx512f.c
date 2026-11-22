// SPDX-FileCopyrightText: 2026 Yo-yo-ooo
// SPDX-License-Identifier: MIT

#include <immintrin.h>

// x86mem 的 AVX-512 tier 依赖 GCC 14+ intrinsics 头里的
// `#pragma GCC target("avx512f,evex512")`（见 avx512fintrin.h）。
// GCC 13 及更早会报 "attribute 'target' argument 'evex512' is unknown"。
// 在这里显式复现同一要求：探测失败时 gdef.mk 会关闭 AVX-512 tier，
// x86mem 回退到 avx2/base，功能不受影响。
#pragma GCC target("avx512f,evex512")

// 强制要求编译器生成一条真正的 512 位指令
void test(void) {
    __m512i v = _mm512_setzero_si512();
    asm volatile("" : : "x"(v) : "memory");
}
int main(void) { return 0; }
