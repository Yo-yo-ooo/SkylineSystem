/*
* SPDX-License-Identifier: GPL-2.0-only
* File: 0aarch64mem.S
* Copyright (C) 2026 Yo-yo-ooo
*
* This file is part of SkylineSystem.
*
* SkylineSystem is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
*/
#ifdef __aarch64__

#include <stdint.h>
#include <stddef.h>
#include <pdef.h>

__extension__ extern __inline __Uint8x16_t
__attribute__ ((__always_inline__, __gnu_inline__, __artificial__))
vld1q_u8 (const uint8_t *__a)
{
  return __builtin_aarch64_ld1v16qi_us (
				(const __builtin_aarch64_simd_qi *) __a);
}

__extension__ extern __inline __Uint8x8_t
__attribute__ ((__always_inline__, __gnu_inline__, __artificial__))
vdup_n_u8 (uint8_t __a)
{
  return (__Uint8x8_t) {__a, __a, __a, __a, __a, __a, __a, __a};
}

__extension__ extern __inline __Uint8x8_t
__attribute__ ((__always_inline__, __gnu_inline__, __artificial__))
vld1_u8 (const uint8_t *__a)
{
  return __builtin_aarch64_ld1v8qi_us (
				(const __builtin_aarch64_simd_qi *) __a);
}

__extension__ extern __inline void
__attribute__ ((__always_inline__, __gnu_inline__, __artificial__))
vst1q_u8 (uint8_t *__a, __Uint8x16_t __b)
{
  __builtin_aarch64_st1v16qi_su ((__builtin_aarch64_simd_qi *) __a, __b);
}

__extension__ extern __inline void
__attribute__ ((__always_inline__, __gnu_inline__, __artificial__))
vst1_u8 (uint8_t *__a, __Uint8x8_t __b)
{
  __builtin_aarch64_st1v8qi_su ((__builtin_aarch64_simd_qi *) __a, __b);
}

__extension__ extern __inline __Uint8x16_t
__attribute__ ((__always_inline__, __gnu_inline__, __artificial__))
vdupq_n_u8 (uint8_t __a)
{
  return (__Uint8x16_t) {__a, __a, __a, __a, __a, __a, __a, __a,
		       __a, __a, __a, __a, __a, __a, __a, __a};
}

func_optimize(3)
void NEON_MEMCPY(void* dst, const void* src, size_t size) {

    uint8_t* neon_dst = (uint8_t*)dst;
    const uint8_t* neon_src = (const uint8_t*)src;

    int32_t nn = size / 64;
    size -= nn * 64;
    while (nn--) {
        __builtin_prefetch(neon_src + 64);
        __Uint8x16_t _p0 = vld1q_u8(neon_src);
        __Uint8x16_t _p1 = vld1q_u8(neon_src + 16);
        __Uint8x16_t _p2 = vld1q_u8(neon_src + 32);
        __Uint8x16_t _p3 = vld1q_u8(neon_src + 48);
        vst1q_u8(neon_dst, _p0);
        vst1q_u8(neon_dst + 16, _p1);
        vst1q_u8(neon_dst + 32, _p2);
        vst1q_u8(neon_dst + 48, _p3);
        neon_src += 64;
        neon_dst += 64;
    }
    if (size > 16) {
        __Uint8x16_t _p0 = vld1q_u8(neon_src);
        vst1q_u8(neon_dst, _p0);
        neon_src += 16;
        neon_dst += 16;
        size -= 16;
    }
    if (size > 8) {
        __Uint8x8_t _p0 = vld1_u8(neon_src);
        vst1_u8(neon_dst, _p0);
        neon_src += 8;
        neon_dst += 8;
        size -= 8;
    }
    while (size--) {
        *neon_dst++ = *neon_src++;
    }
}

func_optimize(3)
void NEON_MEMSET(void* dst, uint8_t value, size_t size) {

    // NEON 优化部分
    uint8_t* neon_dst = (uint8_t*)dst;
    int32_t nn = size / 64;
    size -= nn * 64;

    // 生成包含 16 个相同字节的 NEON 向量
    __Uint8x16_t neon_value = vdupq_n_u8(value);

    // 使用 NEON 指令进行批量内存填充
    while (nn--) {
        // 预取下一块数据，尽管在 memset 中这个可能影响较小
        __builtin_prefetch(neon_dst + 64);
        vst1q_u8(neon_dst, neon_value);
        vst1q_u8(neon_dst + 16, neon_value);
        vst1q_u8(neon_dst + 32, neon_value);
        vst1q_u8(neon_dst + 48, neon_value);
        neon_dst += 64;
    }

    // 处理剩余的字节
    while (size >= 16) {
        vst1q_u8(neon_dst, neon_value);
        neon_dst += 16;
        size -= 16;
    }
    while (size >= 8) {
        __Uint8x8_t neon_value8 = vdup_n_u8(value);
        vst1_u8(neon_dst, neon_value8);
        neon_dst += 8;
        size -= 8;
    }
    // 处理剩余不足 8 个字节的情况
    while (size--) {
        *neon_dst++ = value;
    }
}



#endif