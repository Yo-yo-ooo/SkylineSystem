/*
 * SPDX-License-Identifier: MIT
 * File: atomic.h
 * Copyright (C) 2026 Yo-yo-ooo
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
/* atomic.h - 跨架构原子操作库，覆盖 GCC __atomic_xxx 内建语义 */
#ifndef ATOMIC_H
#define ATOMIC_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 内存序常量，与 GCC __ATOMIC_* 一致 */
#define ATOMIC_RELAXED  0
#define ATOMIC_CONSUME  1
#define ATOMIC_ACQUIRE  2
#define ATOMIC_RELEASE  3
#define ATOMIC_ACQ_REL  4
#define ATOMIC_SEQ_CST  5

/* atomic_flag */
typedef uint8_t atomic_flag;
#define ATOMIC_FLAG_INIT 0

/* lock-free 查询：1/2/4/8 一定无锁；x86_64 +cmpxchg16b 支持 16 */
#define ATOMIC_ALWAYS_LOCK_FREE(n) ((n) <= 8)
#define ATOMIC_IS_LOCK_FREE(n)     ((n) <= 8)

#if defined(__x86_64__)
#  include "atomic_x86_64.h"
#elif defined(__aarch64__)
#  include "atomic_aarch64.h"
#elif defined(__riscv) && (__riscv_xlen == 64)
#  include "atomic_riscv64.h"
#else
#  error "Unsupported architecture"
#endif

/* ------------------------------------------------------------------ */
/* 内部通用调度宏：利用 ## 拼接消除重复的 switch-case 样板            */
/* ------------------------------------------------------------------ */

/* 用于 load 类操作 (无 v 参数) */
#define _ATOMIC_LOAD_OP(op, ptr, mo) ({ \
    uint64_t _i; \
    switch(sizeof(*(ptr))) { \
    case 1: _i = atomic_##op##_1((ptr), (mo)); break; \
    case 2: _i = atomic_##op##_2((ptr), (mo)); break; \
    case 4: _i = atomic_##op##_4((ptr), (mo)); break; \
    case 8: _i = atomic_##op##_8((ptr), (mo)); break; \
    default: _i = 0; break; \
    } \
    (typeof(*(ptr)))_i; \
})

/* 用于 fetch_xxx / xxx_fetch / exchange 类操作 (带 v 参数) */
#define _ATOMIC_FETCH_OP(op, ptr, v, mo) ({ \
    uint64_t _i; \
    switch(sizeof(*(ptr))) { \
    case 1: _i = atomic_##op##_1((ptr), (uint8_t)(uintptr_t)(v), (mo)); break; \
    case 2: _i = atomic_##op##_2((ptr), (uint16_t)(uintptr_t)(v), (mo)); break; \
    case 4: _i = atomic_##op##_4((ptr), (uint32_t)(uintptr_t)(v), (mo)); break; \
    case 8: _i = atomic_##op##_8((ptr), (uint64_t)(uintptr_t)(v), (mo)); break; \
    default: _i = 0; break; \
    } \
    (typeof(*(ptr)))_i; \
})

/* ------------------------------------------------------------------ */
/* 公共 API                                                            */
/* ------------------------------------------------------------------ */

#define atomic_load_n(ptr, mo)             _ATOMIC_LOAD_OP(load, (ptr), (mo))

#define atomic_store_n(ptr, v, mo) do { \
    switch(sizeof(*(ptr))) { \
    case 1: atomic_store_1((ptr), (uint8_t)(uintptr_t)(v), (mo)); break; \
    case 2: atomic_store_2((ptr), (uint16_t)(uintptr_t)(v), (mo)); break; \
    case 4: atomic_store_4((ptr), (uint32_t)(uintptr_t)(v), (mo)); break; \
    case 8: atomic_store_8((ptr), (uint64_t)(uintptr_t)(v), (mo)); break; \
    default: break; \
    } \
} while(0)

#define atomic_exchange_n(ptr, v, mo)      _ATOMIC_FETCH_OP(exchange, (ptr), (v), (mo))

#define atomic_compare_exchange_n(ptr, exp, des, weak, succ, fail) ({ \
    bool _ok; \
    switch(sizeof(*(ptr))) { \
    case 1: _ok = atomic_compare_exchange_1((ptr),(exp),(uint8_t)(uintptr_t)(des),weak,succ,fail); break; \
    case 2: _ok = atomic_compare_exchange_2((ptr),(exp),(uint16_t)(uintptr_t)(des),weak,succ,fail); break; \
    case 4: _ok = atomic_compare_exchange_4((ptr),(exp),(uint32_t)(uintptr_t)(des),weak,succ,fail); break; \
    case 8: _ok = atomic_compare_exchange_8((ptr),(exp),(uint64_t)(uintptr_t)(des),weak,succ,fail); break; \
    default: _ok = false; break; \
    } \
    _ok; \
})

#define atomic_fetch_add_n(ptr, v, mo)     _ATOMIC_FETCH_OP(fetch_add, (ptr), (v), (mo))
#define atomic_fetch_sub_n(ptr, v, mo)     _ATOMIC_FETCH_OP(fetch_sub, (ptr), (v), (mo))
#define atomic_fetch_and_n(ptr, v, mo)     _ATOMIC_FETCH_OP(fetch_and, (ptr), (v), (mo))
#define atomic_fetch_or_n(ptr, v, mo)      _ATOMIC_FETCH_OP(fetch_or,  (ptr), (v), (mo))
#define atomic_fetch_xor_n(ptr, v, mo)     _ATOMIC_FETCH_OP(fetch_xor, (ptr), (v), (mo))
#define atomic_fetch_nand_n(ptr, v, mo)    _ATOMIC_FETCH_OP(fetch_nand,(ptr), (v), (mo))

#define atomic_add_fetch_n(ptr, v, mo)     _ATOMIC_FETCH_OP(add_fetch, (ptr), (v), (mo))
#define atomic_sub_fetch_n(ptr, v, mo)     _ATOMIC_FETCH_OP(sub_fetch, (ptr), (v), (mo))
#define atomic_and_fetch_n(ptr, v, mo)     _ATOMIC_FETCH_OP(and_fetch, (ptr), (v), (mo))
#define atomic_or_fetch_n(ptr, v, mo)      _ATOMIC_FETCH_OP(or_fetch,  (ptr), (v), (mo))
#define atomic_xor_fetch_n(ptr, v, mo)     _ATOMIC_FETCH_OP(xor_fetch, (ptr), (v), (mo))
#define atomic_nand_fetch_n(ptr, v, mo)    _ATOMIC_FETCH_OP(nand_fetch,(ptr), (v), (mo))

#ifdef __cplusplus
}
#endif
#endif /* ATOMIC_H */