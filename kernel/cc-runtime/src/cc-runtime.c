#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#pragma GCC diagnostic ignored "-Wunused-function"

//===-- assembly.h - compiler-rt assembler support macros -----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// 2024/01/21 - Modified by mintsuki for use inside cc-runtime
//
//===----------------------------------------------------------------------===//
//
// This file defines macros for use in compiler-rt assembler source.
// This file is not part of the interface of this library.
//
//===----------------------------------------------------------------------===//

#ifndef COMPILERRT_ASSEMBLY_H
#define COMPILERRT_ASSEMBLY_H

#define SEPARATOR ;

#define HIDDEN(name) .hidden name
#define LOCAL_LABEL(name) .L_##name
#define FILE_LEVEL_DIRECTIVE
#if defined(__arm__) || defined(__aarch64__)
#define SYMBOL_IS_FUNC(name) .type name,%function
#else
#define SYMBOL_IS_FUNC(name) .type name,@function
#endif
#define CONST_SECTION .section .rodata

#define NO_EXEC_STACK_DIRECTIVE .section .note.GNU-stack,"",%progbits

#if defined(__arm__) || defined(__aarch64__)
#define FUNC_ALIGN                                                             \
  .text SEPARATOR                                                              \
  .balign 16 SEPARATOR
#else
#define FUNC_ALIGN
#endif

// BTI and PAC gnu property note
#define NT_GNU_PROPERTY_TYPE_0 5
#define GNU_PROPERTY_AARCH64_FEATURE_1_AND 0xc0000000
#define GNU_PROPERTY_AARCH64_FEATURE_1_BTI 1
#define GNU_PROPERTY_AARCH64_FEATURE_1_PAC 2

#if defined(__ARM_FEATURE_BTI_DEFAULT)
#define BTI_FLAG GNU_PROPERTY_AARCH64_FEATURE_1_BTI
#else
#define BTI_FLAG 0
#endif

#if __ARM_FEATURE_PAC_DEFAULT & 3
#define PAC_FLAG GNU_PROPERTY_AARCH64_FEATURE_1_PAC
#else
#define PAC_FLAG 0
#endif

#define GNU_PROPERTY(type, value)                                              \
  .pushsection .note.gnu.property, "a" SEPARATOR                               \
  .p2align 3 SEPARATOR                                                         \
  .word 4 SEPARATOR                                                            \
  .word 16 SEPARATOR                                                           \
  .word NT_GNU_PROPERTY_TYPE_0 SEPARATOR                                       \
  .asciz "GNU" SEPARATOR                                                       \
  .word type SEPARATOR                                                         \
  .word 4 SEPARATOR                                                            \
  .word value SEPARATOR                                                        \
  .word 0 SEPARATOR                                                            \
  .popsection

#if BTI_FLAG != 0
#define BTI_C hint #34
#define BTI_J hint #36
#else
#define BTI_C
#define BTI_J
#endif

#if (BTI_FLAG | PAC_FLAG) != 0
#define GNU_PROPERTY_BTI_PAC                                                   \
  GNU_PROPERTY(GNU_PROPERTY_AARCH64_FEATURE_1_AND, BTI_FLAG | PAC_FLAG)
#else
#define GNU_PROPERTY_BTI_PAC
#endif

#if defined(__clang__) || defined(__GCC_HAVE_DWARF2_CFI_ASM)
#define CFI_START .cfi_startproc
#define CFI_END .cfi_endproc
#else
#define CFI_START
#define CFI_END
#endif

#if defined(__arm__)

// Determine actual [ARM][THUMB[1][2]] ISA using compiler predefined macros:
// - for '-mthumb -march=armv6' compiler defines '__thumb__'
// - for '-mthumb -march=armv7' compiler defines '__thumb__' and '__thumb2__'
#if defined(__thumb2__) || defined(__thumb__)
#define DEFINE_CODE_STATE .thumb SEPARATOR
#define DECLARE_FUNC_ENCODING    .thumb_func SEPARATOR
#if defined(__thumb2__)
#define USE_THUMB_2
#define IT(cond)  it cond
#define ITT(cond) itt cond
#define ITE(cond) ite cond
#else
#define USE_THUMB_1
#define IT(cond)
#define ITT(cond)
#define ITE(cond)
#endif // defined(__thumb__2)
#else // !defined(__thumb2__) && !defined(__thumb__)
#define DEFINE_CODE_STATE .arm SEPARATOR
#define DECLARE_FUNC_ENCODING
#define IT(cond)
#define ITT(cond)
#define ITE(cond)
#endif

#if defined(USE_THUMB_1) && defined(USE_THUMB_2)
#error "USE_THUMB_1 and USE_THUMB_2 can't be defined together."
#endif

#if defined(__ARM_ARCH_4T__) || __ARM_ARCH >= 5
#define ARM_HAS_BX
#endif
#if !defined(__ARM_FEATURE_CLZ) && !defined(USE_THUMB_1) &&  \
    (__ARM_ARCH >= 6 || (__ARM_ARCH == 5 && !defined(__ARM_ARCH_5__)))
#define __ARM_FEATURE_CLZ
#endif

#ifdef ARM_HAS_BX
#define JMP(r) bx r
#define JMPc(r, c) bx##c r
#else
#define JMP(r) mov pc, r
#define JMPc(r, c) mov##c pc, r
#endif

// pop {pc} can't switch Thumb mode on ARMv4T
#if __ARM_ARCH >= 5
#define POP_PC() pop {pc}
#else
#define POP_PC()                                                               \
  pop {ip};                                                                    \
  JMP(ip)
#endif

#if defined(USE_THUMB_2)
#define WIDE(op) op.w
#else
#define WIDE(op) op
#endif
#else // !defined(__arm)
#define DECLARE_FUNC_ENCODING
#define DEFINE_CODE_STATE
#endif

#define GLUE2_(a, b) a##b
#define GLUE(a, b) GLUE2_(a, b)
#define GLUE2(a, b) GLUE2_(a, b)
#define GLUE3_(a, b, c) a##b##c
#define GLUE3(a, b, c) GLUE3_(a, b, c)
#define GLUE4_(a, b, c, d) a##b##c##d
#define GLUE4(a, b, c, d) GLUE4_(a, b, c, d)

#ifndef SYMBOL_NAME
#define SYMBOL_NAME(name) GLUE(__USER_LABEL_PREFIX__, name)
#endif

#ifdef VISIBILITY_HIDDEN
#define DECLARE_SYMBOL_VISIBILITY(name)                                        \
  HIDDEN(SYMBOL_NAME(name)) SEPARATOR
#define DECLARE_SYMBOL_VISIBILITY_UNMANGLED(name) \
  HIDDEN(name) SEPARATOR
#else
#define DECLARE_SYMBOL_VISIBILITY(name)
#define DECLARE_SYMBOL_VISIBILITY_UNMANGLED(name)
#endif

#define DEFINE_COMPILERRT_FUNCTION(name)                                       \
  DEFINE_CODE_STATE                                                            \
  FILE_LEVEL_DIRECTIVE SEPARATOR                                               \
  .globl SYMBOL_NAME(name) SEPARATOR                                           \
  SYMBOL_IS_FUNC(SYMBOL_NAME(name)) SEPARATOR                                  \
  DECLARE_SYMBOL_VISIBILITY(name)                                              \
  DECLARE_FUNC_ENCODING                                                        \
  SYMBOL_NAME(name):

#define DEFINE_COMPILERRT_THUMB_FUNCTION(name)                                 \
  DEFINE_CODE_STATE                                                            \
  FILE_LEVEL_DIRECTIVE SEPARATOR                                               \
  .globl SYMBOL_NAME(name) SEPARATOR                                           \
  SYMBOL_IS_FUNC(SYMBOL_NAME(name)) SEPARATOR                                  \
  DECLARE_SYMBOL_VISIBILITY(name) SEPARATOR                                    \
  .thumb_func SEPARATOR                                                        \
  SYMBOL_NAME(name):

#define DEFINE_COMPILERRT_PRIVATE_FUNCTION(name)                               \
  DEFINE_CODE_STATE                                                            \
  FILE_LEVEL_DIRECTIVE SEPARATOR                                               \
  .globl SYMBOL_NAME(name) SEPARATOR                                           \
  SYMBOL_IS_FUNC(SYMBOL_NAME(name)) SEPARATOR                                  \
  HIDDEN(SYMBOL_NAME(name)) SEPARATOR                                          \
  DECLARE_FUNC_ENCODING                                                        \
  SYMBOL_NAME(name):

#define DEFINE_COMPILERRT_PRIVATE_FUNCTION_UNMANGLED(name)                     \
  DEFINE_CODE_STATE                                                            \
  .globl name SEPARATOR                                                        \
  SYMBOL_IS_FUNC(name) SEPARATOR                                               \
  HIDDEN(name) SEPARATOR                                                       \
  DECLARE_FUNC_ENCODING                                                        \
  name:

#define DEFINE_COMPILERRT_OUTLINE_FUNCTION_UNMANGLED(name)                     \
  DEFINE_CODE_STATE                                                            \
  FUNC_ALIGN                                                                   \
  .globl name SEPARATOR                                                        \
  SYMBOL_IS_FUNC(name) SEPARATOR                                               \
  DECLARE_SYMBOL_VISIBILITY_UNMANGLED(name) SEPARATOR                          \
  DECLARE_FUNC_ENCODING                                                        \
  name:                                                                        \
  SEPARATOR CFI_START                                                          \
  SEPARATOR BTI_C

#define DEFINE_COMPILERRT_FUNCTION_ALIAS(name, target)                         \
  .globl SYMBOL_NAME(name) SEPARATOR                                           \
  SYMBOL_IS_FUNC(SYMBOL_NAME(name)) SEPARATOR                                  \
  DECLARE_SYMBOL_VISIBILITY(name) SEPARATOR                                    \
  .set SYMBOL_NAME(name), SYMBOL_NAME(target) SEPARATOR

#if defined(__ARM_EABI__)
#define DEFINE_AEABI_FUNCTION_ALIAS(aeabi_name, name)                          \
  DEFINE_COMPILERRT_FUNCTION_ALIAS(aeabi_name, name)
#else
#define DEFINE_AEABI_FUNCTION_ALIAS(aeabi_name, name)
#endif

#define END_COMPILERRT_FUNCTION(name)                                          \
  .size SYMBOL_NAME(name), . - SYMBOL_NAME(name)
#define END_COMPILERRT_OUTLINE_FUNCTION(name)                                  \
  CFI_END SEPARATOR                                                            \
  .size SYMBOL_NAME(name), . - SYMBOL_NAME(name)

#endif // COMPILERRT_ASSEMBLY_H
//===-- int_endianness.h - configuration header for compiler-rt -----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// 2024/01/21 - Modified by mintsuki for use inside cc-runtime
//
//===----------------------------------------------------------------------===//
//
// This file is a configuration header for compiler-rt.
// This file is not part of the interface of this library.
//
//===----------------------------------------------------------------------===//

#ifndef INT_ENDIANNESS_H
#define INT_ENDIANNESS_H

// Clang and GCC provide built-in endianness definitions.
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define _YUGA_LITTLE_ENDIAN 0
#define _YUGA_BIG_ENDIAN 1
#elif __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define _YUGA_LITTLE_ENDIAN 1
#define _YUGA_BIG_ENDIAN 0
#endif // __BYTE_ORDER__

#endif // INT_ENDIANNESS_H
//===-- int_lib.h - configuration header for compiler-rt  -----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// 2024/01/21 - Modified by mintsuki for use inside cc-runtime
//
//===----------------------------------------------------------------------===//
//
// This file is not part of the interface of this library.
//
// This file defines various standard types, most importantly a number of unions
// used to access parts of larger types.
//
//===----------------------------------------------------------------------===//

#ifndef INT_TYPES_H
#define INT_TYPES_H


// si_int is defined in Linux sysroot's asm-generic/siginfo.h
#ifdef si_int
#undef si_int
#endif
typedef int32_t si_int;
typedef uint32_t su_int;
#if UINT_MAX == 0xFFFFFFFF
#define clzsi __builtin_clz
#define ctzsi __builtin_ctz
#elif ULONG_MAX == 0xFFFFFFFF
#define clzsi __builtin_clzl
#define ctzsi __builtin_ctzl
#else
#error could not determine appropriate clzsi macro for this system
#endif

typedef int64_t di_int;
typedef uint64_t du_int;

typedef union {
  di_int all;
  struct {
#if _YUGA_LITTLE_ENDIAN
    su_int low;
    si_int high;
#else
    si_int high;
    su_int low;
#endif // _YUGA_LITTLE_ENDIAN
  } s;
} dwords;

typedef union {
  du_int all;
  struct {
#if _YUGA_LITTLE_ENDIAN
    su_int low;
    su_int high;
#else
    su_int high;
    su_int low;
#endif // _YUGA_LITTLE_ENDIAN
  } s;
} udwords;

#if defined(__LP64__) || defined(__wasm__) || defined(__mips64) ||             \
    defined(__SIZEOF_INT128__)
#define CRT_HAS_128BIT
#endif

#ifdef CRT_HAS_128BIT
typedef int ti_int __attribute__((mode(TI)));
typedef unsigned tu_int __attribute__((mode(TI)));

typedef union {
  ti_int all;
  struct {
#if _YUGA_LITTLE_ENDIAN
    du_int low;
    di_int high;
#else
    di_int high;
    du_int low;
#endif // _YUGA_LITTLE_ENDIAN
  } s;
} twords;

typedef union {
  tu_int all;
  struct {
#if _YUGA_LITTLE_ENDIAN
    du_int low;
    du_int high;
#else
    du_int high;
    du_int low;
#endif // _YUGA_LITTLE_ENDIAN
  } s;
} utwords;

#endif // CRT_HAS_128BIT

#endif // INT_TYPES_H
//===-- int_lib.h - configuration header for compiler-rt  -----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// 2024/01/21 - Modified by mintsuki for use inside cc-runtime
//
//===----------------------------------------------------------------------===//
//
// This file is a configuration header for compiler-rt.
// This file is not part of the interface of this library.
//
//===----------------------------------------------------------------------===//

#ifndef INT_LIB_H
#define INT_LIB_H

// Assumption: Signed integral is 2's complement.
// Assumption: Right shift of signed negative is arithmetic shift.
// Assumption: Endianness is little or big (not mixed).

// ABI macro definitions

#if __ARM_EABI__
#ifdef COMPILER_RT_ARMHF_TARGET
#define COMPILER_RT_ABI
#else
#define COMPILER_RT_ABI __attribute__((__pcs__("aapcs")))
#endif
#else
#define COMPILER_RT_ABI
#endif

#define AEABI_RTABI __attribute__((__pcs__("aapcs")))

#define ALWAYS_INLINE __attribute__((always_inline))
#define NOINLINE __attribute__((noinline))
#define NORETURN __attribute__((noreturn))
#define UNUSED __attribute__((unused))

#ifndef SYMBOL_NAME
#define STR(a) #a
#define XSTR(a) STR(a)
#define SYMBOL_NAME(name) XSTR(__USER_LABEL_PREFIX__) #name
#endif

#define COMPILER_RT_ALIAS(name, aliasname) \
  COMPILER_RT_ABI __typeof(name) aliasname __attribute__((__alias__(#name)));

// Include the standard compiler builtin headers we use functionality from.

// Include the commonly used internal type definitions.

// Include internal utility function declarations.

COMPILER_RT_ABI int __paritysi2(si_int a);
COMPILER_RT_ABI int __paritydi2(di_int a);

COMPILER_RT_ABI di_int __divdi3(di_int a, di_int b);
COMPILER_RT_ABI si_int __divsi3(si_int a, si_int b);
COMPILER_RT_ABI su_int __udivsi3(su_int n, su_int d);

COMPILER_RT_ABI su_int __udivmodsi4(su_int a, su_int b, su_int *rem);
COMPILER_RT_ABI du_int __udivmoddi4(du_int a, du_int b, du_int *rem);
#ifdef CRT_HAS_128BIT
COMPILER_RT_ABI int __clzti2(ti_int a);
COMPILER_RT_ABI tu_int __udivmodti4(tu_int a, tu_int b, tu_int *rem);
#endif

#endif // INT_LIB_H
//===-- int_util.h - internal utility functions ---------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file is not part of the interface of this library.
//
// This file defines non-inline utilities which are available for use in the
// library. The function definitions themselves are all contained in int_util.c
// which will always be compiled into any compiler-rt library.
//
//===----------------------------------------------------------------------===//

#ifndef INT_UTIL_H
#define INT_UTIL_H

/// \brief Trigger a program abort (or panic for kernel code).
#define compilerrt_abort() __compilerrt_abort_impl(__FILE__, __LINE__, __func__)

NORETURN void __compilerrt_abort_impl(const char *file, int line,
                                      const char *function);

#define COMPILE_TIME_ASSERT(expr) COMPILE_TIME_ASSERT1(expr, __COUNTER__)
#define COMPILE_TIME_ASSERT1(expr, cnt) COMPILE_TIME_ASSERT2(expr, cnt)
#define COMPILE_TIME_ASSERT2(expr, cnt)                                        \
  typedef char ct_assert_##cnt[(expr) ? 1 : -1] UNUSED

// Force unrolling the code specified to be repeated N times.
#define REPEAT_0_TIMES(code_to_repeat) /* do nothing */
#define REPEAT_1_TIMES(code_to_repeat) code_to_repeat
#define REPEAT_2_TIMES(code_to_repeat)                                         \
  REPEAT_1_TIMES(code_to_repeat)                                               \
  code_to_repeat
#define REPEAT_3_TIMES(code_to_repeat)                                         \
  REPEAT_2_TIMES(code_to_repeat)                                               \
  code_to_repeat
#define REPEAT_4_TIMES(code_to_repeat)                                         \
  REPEAT_3_TIMES(code_to_repeat)                                               \
  code_to_repeat

#define REPEAT_N_TIMES_(N, code_to_repeat) REPEAT_##N##_TIMES(code_to_repeat)
#define REPEAT_N_TIMES(N, code_to_repeat) REPEAT_N_TIMES_(N, code_to_repeat)

#endif // INT_UTIL_H
//===-- absvdi2.c - Implement __absvdi2 -----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements __absvdi2 for the compiler_rt library.
//
//===----------------------------------------------------------------------===//


// Returns: absolute value

// Effects: aborts if abs(x) < 0

COMPILER_RT_ABI di_int __absvdi2(di_int a) {
  const int N = (int)(sizeof(di_int) * CHAR_BIT);
  if (a == ((di_int)((du_int)1 << (N - 1))))
    compilerrt_abort();
  const di_int t = a >> (N - 1);
  return (a ^ t) - t;
}
//===-- absvsi2.c - Implement __absvsi2 -----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements __absvsi2 for the compiler_rt library.
//
//===----------------------------------------------------------------------===//


// Returns: absolute value

// Effects: aborts if abs(x) < 0

COMPILER_RT_ABI si_int __absvsi2(si_int a) {
  const int N = (int)(sizeof(si_int) * CHAR_BIT);
  if (a == ((si_int)((su_int)1 << (N - 1))))
    compilerrt_abort();
  const si_int t = a >> (N - 1);
  return (a ^ t) - t;
}
//===-- absvti2.c - Implement __absvdi2 -----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements __absvti2 for the compiler_rt library.
//
//===----------------------------------------------------------------------===//


#ifdef CRT_HAS_128BIT

// Returns: absolute value

// Effects: aborts if abs(x) < 0

COMPILER_RT_ABI ti_int __absvti2(ti_int a) {
  const int N = (int)(sizeof(ti_int) * CHAR_BIT);
  if (a == (ti_int)((tu_int)1 << (N - 1)))
    compilerrt_abort();
  const ti_int s = a >> (N - 1);
  return (a ^ s) - s;
}

#endif // CRT_HAS_128BIT
//===-- addvdi3.c - Implement __addvdi3 -----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements __addvdi3 for the compiler_rt library.
//
//===----------------------------------------------------------------------===//


// Returns: a + b

// Effects: aborts if a + b overflows

COMPILER_RT_ABI di_int __addvdi3(di_int a, di_int b) {
  di_int s = (du_int)a + (du_int)b;
  if (b >= 0) {
    if (s < a)
      compilerrt_abort();
  } else {
    if (s >= a)
      compilerrt_abort();
  }
  return s;
}
//===-- addvsi3.c - Implement __addvsi3 -----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements __addvsi3 for the compiler_rt library.
//
//===----------------------------------------------------------------------===//


// Returns: a + b

// Effects: aborts if a + b overflows

COMPILER_RT_ABI si_int __addvsi3(si_int a, si_int b) {
  si_int s = (su_int)a + (su_int)b;
  if (b >= 0) {
    if (s < a)
      compilerrt_abort();
  } else {
    if (s >= a)
      compilerrt_abort();
  }
  return s;
}
//===-- addvti3.c - Implement __addvti3 -----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements __addvti3 for the compiler_rt library.
//
//===----------------------------------------------------------------------===//


#ifdef CRT_HAS_128BIT

// Returns: a + b

// Effects: aborts if a + b overflows

COMPILER_RT_ABI ti_int __addvti3(ti_int a, ti_int b) {
  ti_int s = (tu_int)a + (tu_int)b;
  if (b >= 0) {
    if (s < a)
      compilerrt_abort();
  } else {
    if (s >= a)
      compilerrt_abort();
  }
  return s;
}

#endif // CRT_HAS_128BIT
// ====-- ashldi3.c - Implement __ashldi3 ---------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements __ashldi3 for the compiler_rt library.
//
//===----------------------------------------------------------------------===//


// Returns: a << b

// Precondition:  0 <= b < bits_in_dword

COMPILER_RT_ABI di_int __ashldi3(di_int a, int b) {
  const int bits_in_word = (int)(sizeof(si_int) * CHAR_BIT);
  dwords input;
  dwords result;
  input.all = a;
  if (b & bits_in_word) /* bits_in_word <= b < bits_in_dword */ {
    result.s.low = 0;
    result.s.high = input.s.low << (b - bits_in_word);
  } else /* 0 <= b < bits_in_word */ {
    if (b == 0)
      return a;
    result.s.low = input.s.low << b;
    result.s.high =
        ((su_int)input.s.high << b) | (input.s.low >> (bits_in_word - b));
  }
  return result.all;
}

#if defined(__ARM_EABI__)
COMPILER_RT_ALIAS(__ashldi3, __aeabi_llsl)
#endif
//===-- ashlti3.c - Implement __ashlti3 -----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements __ashlti3 for the compiler_rt library.
//
//===----------------------------------------------------------------------===//


#ifdef CRT_HAS_128BIT

// Returns: a << b

// Precondition:  0 <= b < bits_in_tword

COMPILER_RT_ABI ti_int __ashlti3(ti_int a, int b) {
  const int bits_in_dword = (int)(sizeof(di_int) * CHAR_BIT);
  twords input;
  twords result;
  input.all = a;
  if (b & bits_in_dword) /* bits_in_dword <= b < bits_in_tword */ {
    result.s.low = 0;
    result.s.high = input.s.low << (b - bits_in_dword);
  } else /* 0 <= b < bits_in_dword */ {
    if (b == 0)
      return a;
    result.s.low = input.s.low << b;
    result.s.high =
        ((du_int)input.s.high << b) | (input.s.low >> (bits_in_dword - b));
  }
  return result.all;
}

#endif // CRT_HAS_128BIT
//===-- ashrdi3.c - Implement __ashrdi3 -----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements __ashrdi3 for the compiler_rt library.
//
//===----------------------------------------------------------------------===//


// Returns: arithmetic a >> b

// Precondition:  0 <= b < bits_in_dword

COMPILER_RT_ABI di_int __ashrdi3(di_int a, int b) {
  const int bits_in_word = (int)(sizeof(si_int) * CHAR_BIT);
  dwords input;
  dwords result;
  input.all = a;
  if (b & bits_in_word) /* bits_in_word <= b < bits_in_dword */ {
    // result.s.high = input.s.high < 0 ? -1 : 0
    result.s.high = input.s.high >> (bits_in_word - 1);
    result.s.low = input.s.high >> (b - bits_in_word);
  } else /* 0 <= b < bits_in_word */ {
    if (b == 0)
      return a;
    result.s.high = input.s.high >> b;
    result.s.low =
        ((su_int)input.s.high << (bits_in_word - b)) | (input.s.low >> b);
  }
  return result.all;
}

#if defined(__ARM_EABI__)
COMPILER_RT_ALIAS(__ashrdi3, __aeabi_lasr)
#endif
//===-- ashrti3.c - Implement __ashrti3 -----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements __ashrti3 for the compiler_rt library.
//
//===----------------------------------------------------------------------===//


#ifdef CRT_HAS_128BIT

// Returns: arithmetic a >> b

// Precondition:  0 <= b < bits_in_tword

COMPILER_RT_ABI ti_int __ashrti3(ti_int a, int b) {
  const int bits_in_dword = (int)(sizeof(di_int) * CHAR_BIT);
  twords input;
  twords result;
  input.all = a;
  if (b & bits_in_dword) /* bits_in_dword <= b < bits_in_tword */ {
    // result.s.high = input.s.high < 0 ? -1 : 0
    result.s.high = input.s.high >> (bits_in_dword - 1);
    result.s.low = input.s.high >> (b - bits_in_dword);
  } else /* 0 <= b < bits_in_dword */ {
    if (b == 0)
      return a;
    result.s.high = input.s.high >> b;
    result.s.low =
        ((du_int)input.s.high << (bits_in_dword - b)) | (input.s.low >> b);
  }
  return result.all;
}

#endif // CRT_HAS_128BIT
//===-- bswapdi2.c - Implement __bswapdi2 ---------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements __bswapdi2 for the compiler_rt library.
//
//===----------------------------------------------------------------------===//


COMPILER_RT_ABI uint64_t __bswapdi2(uint64_t u) {
  return (
      (((u)&0xff00000000000000ULL) >> 56) |
      (((u)&0x00ff000000000000ULL) >> 40) |
      (((u)&0x0000ff0000000000ULL) >> 24) |
      (((u)&0x000000ff00000000ULL) >> 8)  |
      (((u)&0x00000000ff000000ULL) << 8)  |
      (((u)&0x0000000000ff0000ULL) << 24) |
      (((u)&0x000000000000ff00ULL) << 40) |
      (((u)&0x00000000000000ffULL) << 56));
}
//===-- bswapsi2.c - Implement __bswapsi2 ---------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements __bswapsi2 for the compiler_rt library.
//
//===----------------------------------------------------------------------===//


COMPILER_RT_ABI uint32_t __bswapsi2(uint32_t u) {
  return ((((u)&0xff000000) >> 24) |
          (((u)&0x00ff0000) >> 8)  |
          (((u)&0x0000ff00) << 8)  |
          (((u)&0x000000ff) << 24));
}
//===-- clzdi2.c - Implement __clzdi2 -------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// 2024/08/10 - Modified by mintsuki for use inside cc-runtime
//
//===----------------------------------------------------------------------===//
//
// This file implements __clzdi2 for the compiler_rt library.
//
//===----------------------------------------------------------------------===//


// Returns: the number of leading 0-bits

#if !defined(__clang__) &&                                                     \
    ((defined(__sparc__) && defined(__arch64__)) || defined(__mips64) ||       \
     (defined(__riscv) && __SIZEOF_POINTER__ >= 8))
// On 64-bit architectures with neither a native clz instruction nor a native
// ctz instruction, gcc resolves __builtin_clz to __clzdi2 rather than
// __clzsi2, leading to infinite recursion.
#define __builtin_clz(a) __clzsi2(a)
extern int __clzsi2(si_int);
#endif

// Precondition: a != 0

COMPILER_RT_ABI int __clzdi2(di_int a) {
  dwords x;
  x.all = a;
  const si_int f = -(x.s.high == 0);
  return clzsi((x.s.high & ~f) | (x.s.low & f)) +
         (f & ((si_int)(sizeof(si_int) * CHAR_BIT)));
}

#ifdef __builtin_clz
#undef __builtin_clz
#endif
//===-- clzsi2.c - Implement __clzsi2 -------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements __clzsi2 for the compiler_rt library.
//
//===----------------------------------------------------------------------===//


// Returns: the number of leading 0-bits

// Precondition: a != 0

COMPILER_RT_ABI int __clzsi2(si_int a) {
  su_int x = (su_int)a;
  si_int t = ((x & 0xFFFF0000) == 0) << 4; // if (x is small) t = 16 else 0
  x >>= 16 - t;                            // x = [0 - 0xFFFF]
  su_int r = t;                            // r = [0, 16]
  // return r + clz(x)
  t = ((x & 0xFF00) == 0) << 3;
  x >>= 8 - t; // x = [0 - 0xFF]
  r += t;      // r = [0, 8, 16, 24]
  // return r + clz(x)
  t = ((x & 0xF0) == 0) << 2;
  x >>= 4 - t; // x = [0 - 0xF]
  r += t;      // r = [0, 4, 8, 12, 16, 20, 24, 28]
  // return r + clz(x)
  t = ((x & 0xC) == 0) << 1;
  x >>= 2 - t; // x = [0 - 3]
  r += t;      // r = [0 - 30] and is even
  // return r + clz(x)
  //     switch (x)
  //     {
  //     case 0:
  //         return r + 2;
  //     case 1:
  //         return r + 1;
  //     case 2:
  //     case 3:
  //         return r;
  //     }
  return r + ((2 - x) & -((x & 2) == 0));
}
//===-- clzti2.c - Implement __clzti2 -------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements __clzti2 for the compiler_rt library.
//
//===----------------------------------------------------------------------===//


#ifdef CRT_HAS_128BIT

// Returns: the number of leading 0-bits

// Precondition: a != 0

COMPILER_RT_ABI int __clzti2(ti_int a) {
  twords x;
  x.all = a;
  const di_int f = -(x.s.high == 0);
  return __builtin_clzll((x.s.high & ~f) | (x.s.low & f)) +
         ((si_int)f & ((si_int)(sizeof(di_int) * CHAR_BIT)));
}

#endif // CRT_HAS_128BIT
//===-- cmpdi2.c - Implement __cmpdi2 -------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements __cmpdi2 for the compiler_rt library.
//
//===----------------------------------------------------------------------===//


// Returns: if (a <  b) returns 0
//           if (a == b) returns 1
//           if (a >  b) returns 2

COMPILER_RT_ABI si_int __cmpdi2(di_int a, di_int b) {
  dwords x;
  x.all = a;
  dwords y;
  y.all = b;
  if (x.s.high < y.s.high)
    return 0;
  if (x.s.high > y.s.high)
    return 2;
  if (x.s.low < y.s.low)
    return 0;
  if (x.s.low > y.s.low)
    return 2;
  return 1;
}

#ifdef __ARM_EABI__
// Returns: if (a <  b) returns -1
//           if (a == b) returns  0
//           if (a >  b) returns  1
COMPILER_RT_ABI si_int __aeabi_lcmp(di_int a, di_int b) {
  return __cmpdi2(a, b) - 1;
}
#endif
//===-- cmpti2.c - Implement __cmpti2 -------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements __cmpti2 for the compiler_rt library.
//
//===----------------------------------------------------------------------===//


#ifdef CRT_HAS_128BIT

// Returns:  if (a <  b) returns 0
//           if (a == b) returns 1
//           if (a >  b) returns 2

COMPILER_RT_ABI si_int __cmpti2(ti_int a, ti_int b) {
  twords x;
  x.all = a;
  twords y;
  y.all = b;
  if (x.s.high < y.s.high)
    return 0;
  if (x.s.high > y.s.high)
    return 2;
  if (x.s.low < y.s.low)
    return 0;
  if (x.s.low > y.s.low)
    return 2;
  return 1;
}

#endif // CRT_HAS_128BIT
//===-- ctzdi2.c - Implement __ctzdi2 -------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// 2024/08/10 - Modified by mintsuki for use inside cc-runtime
//
//===----------------------------------------------------------------------===//
//
// This file implements __ctzdi2 for the compiler_rt library.
//
//===----------------------------------------------------------------------===//


// Returns: the number of trailing 0-bits

#if !defined(__clang__) &&                                                     \
    ((defined(__sparc__) && defined(__arch64__)) || defined(__mips64) ||       \
     (defined(__riscv) && __SIZEOF_POINTER__ >= 8))
// On 64-bit architectures with neither a native clz instruction nor a native
// ctz instruction, gcc resolves __builtin_ctz to __ctzdi2 rather than
// __ctzsi2, leading to infinite recursion.
#define __builtin_ctz(a) __ctzsi2(a)
extern int __ctzsi2(si_int);
#endif

// Precondition: a != 0

COMPILER_RT_ABI int __ctzdi2(di_int a) {
  dwords x;
  x.all = a;
  const si_int f = -(x.s.low == 0);
  return ctzsi((x.s.high & f) | (x.s.low & ~f)) +
         (f & ((si_int)(sizeof(si_int) * CHAR_BIT)));
}

#ifdef __builtin_ctz
#undef __builtin_ctz
#endif
//===-- ctzsi2.c - Implement __ctzsi2 -------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements __ctzsi2 for the compiler_rt library.
//
//===----------------------------------------------------------------------===//


// Returns: the number of trailing 0-bits

// Precondition: a != 0

COMPILER_RT_ABI int __ctzsi2(si_int a) {
  su_int x = (su_int)a;
  si_int t = ((x & 0x0000FFFF) == 0)
             << 4; // if (x has no small bits) t = 16 else 0
  x >>= t;         // x = [0 - 0xFFFF] + higher garbage bits
  su_int r = t;    // r = [0, 16]
  // return r + ctz(x)
  t = ((x & 0x00FF) == 0) << 3;
  x >>= t; // x = [0 - 0xFF] + higher garbage bits
  r += t;  // r = [0, 8, 16, 24]
  // return r + ctz(x)
  t = ((x & 0x0F) == 0) << 2;
  x >>= t; // x = [0 - 0xF] + higher garbage bits
  r += t;  // r = [0, 4, 8, 12, 16, 20, 24, 28]
  // return r + ctz(x)
  t = ((x & 0x3) == 0) << 1;
  x >>= t;
  x &= 3; // x = [0 - 3]
  r += t; // r = [0 - 30] and is even
  // return r + ctz(x)

  //  The branch-less return statement below is equivalent
  //  to the following switch statement:
  //     switch (x)
  //    {
  //     case 0:
  //         return r + 2;
  //     case 2:
  //         return r + 1;
  //     case 1:
  //     case 3:
  //         return r;
  //     }
  return r + ((2 - (x >> 1)) & -((x & 1) == 0));
}
//===-- ctzti2.c - Implement __ctzti2 -------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements __ctzti2 for the compiler_rt library.
//
//===----------------------------------------------------------------------===//


#ifdef CRT_HAS_128BIT

// Returns: the number of trailing 0-bits

// Precondition: a != 0

COMPILER_RT_ABI int __ctzti2(ti_int a) {
  twords x;
  x.all = a;
  const di_int f = -(x.s.low == 0);
  return __builtin_ctzll((x.s.high & f) | (x.s.low & ~f)) +
         ((si_int)f & ((si_int)(sizeof(di_int) * CHAR_BIT)));
}

#endif // CRT_HAS_128BIT
//===-- divdi3.c - Implement __divdi3 -------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// 2024/08/10 - Modified by mintsuki for use inside cc-runtime
//
//===----------------------------------------------------------------------===//
//
// This file implements __divdi3 for the compiler_rt library.
//
//===----------------------------------------------------------------------===//


// Returns: a / b

#define fixint_t di_int
#define fixuint_t du_int
#define COMPUTE_UDIV(a, b) __udivmoddi4((a), (b), (du_int *)0)
//===-- int_div_impl.inc - Integer division ---------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// 2024/10/20 - Modified by mintsuki for use inside cc-runtime
//
//===----------------------------------------------------------------------===//
//
// Helpers used by __udivsi3, __umodsi3, __udivdi3, and __umodsi3.
//
//===----------------------------------------------------------------------===//

#define clz(a) (sizeof(a) == sizeof(unsigned long long) ? __builtin_clzll(a) : clzsi(a))

#undef __udivXi3
#define __udivXi3 __udivXi3_1

// Adapted from Figure 3-40 of The PowerPC Compiler Writer's Guide
static __inline fixuint_t __udivXi3(fixuint_t n, fixuint_t d) {
  const unsigned N = sizeof(fixuint_t) * CHAR_BIT;
  // d == 0 cases are unspecified.
  unsigned sr = (d ? clz(d) : N) - (n ? clz(n) : N);
  // 0 <= sr <= N - 1 or sr is very large.
  if (sr > N - 1) // n < d
    return 0;
  if (sr == N - 1) // d == 1
    return n;
  ++sr;
  // 1 <= sr <= N - 1. Shifts do not trigger UB.
  fixuint_t r = n >> sr;
  n <<= N - sr;
  fixuint_t carry = 0;
  for (; sr > 0; --sr) {
    r = (r << 1) | (n >> (N - 1));
    n = (n << 1) | carry;
    // Branch-less version of:
    // carry = 0;
    // if (r >= d) r -= d, carry = 1;
    const fixint_t s = (fixint_t)(d - r - 1) >> (N - 1);
    carry = s & 1;
    r -= d & s;
  }
  n = (n << 1) | carry;
  return n;
}

#undef __umodXi3
#define __umodXi3 __umodXi3_2

// Mostly identical to __udivXi3 but the return values are different.
static __inline fixuint_t __umodXi3(fixuint_t n, fixuint_t d) {
  const unsigned N = sizeof(fixuint_t) * CHAR_BIT;
  // d == 0 cases are unspecified.
  unsigned sr = (d ? clz(d) : N) - (n ? clz(n) : N);
  // 0 <= sr <= N - 1 or sr is very large.
  if (sr > N - 1) // n < d
    return n;
  if (sr == N - 1) // d == 1
    return 0;
  ++sr;
  // 1 <= sr <= N - 1. Shifts do not trigger UB.
  fixuint_t r = n >> sr;
  n <<= N - sr;
  fixuint_t carry = 0;
  for (; sr > 0; --sr) {
    r = (r << 1) | (n >> (N - 1));
    n = (n << 1) | carry;
    // Branch-less version of:
    // carry = 0;
    // if (r >= d) r -= d, carry = 1;
    const fixint_t s = (fixint_t)(d - r - 1) >> (N - 1);
    carry = s & 1;
    r -= d & s;
  }
  return r;
}

#undef __divXi3

#ifdef COMPUTE_UDIV

#define __divXi3 __divXi3_3

static __inline fixint_t __divXi3(fixint_t a, fixint_t b) {
  const int N = (int)(sizeof(fixint_t) * CHAR_BIT) - 1;
  fixint_t s_a = a >> N;                            // s_a = a < 0 ? -1 : 0
  fixint_t s_b = b >> N;                            // s_b = b < 0 ? -1 : 0
  fixuint_t a_u = (fixuint_t)(a ^ s_a) + (-s_a);    // negate if s_a == -1
  fixuint_t b_u = (fixuint_t)(b ^ s_b) + (-s_b);    // negate if s_b == -1
  s_a ^= s_b;                                       // sign of quotient
  return (COMPUTE_UDIV(a_u, b_u) ^ s_a) + (-s_a);   // negate if s_a == -1
}
#endif // COMPUTE_UDIV

#undef __modXi3

#ifdef ASSIGN_UMOD

#define __modXi3 __modXi3_4

static __inline fixint_t __modXi3(fixint_t a, fixint_t b) {
  const int N = (int)(sizeof(fixint_t) * CHAR_BIT) - 1;
  fixint_t s = b >> N;                              // s = b < 0 ? -1 : 0
  fixuint_t b_u = (fixuint_t)(b ^ s) + (-s);        // negate if s == -1
  s = a >> N;                                       // s = a < 0 ? -1 : 0
  fixuint_t a_u = (fixuint_t)(a ^ s) + (-s);        // negate if s == -1
  fixuint_t res;
  ASSIGN_UMOD(res, a_u, b_u);
  return (res ^ s) + (-s);                          // negate if s == -1
}
#endif // ASSIGN_UMOD

#undef clz

COMPILER_RT_ABI di_int __divdi3(di_int a, di_int b) { return __divXi3(a, b); }

#undef fixint_t
#undef fixuint_t
#undef COMPUTE_UDIV
//===-- divmoddi4.c - Implement __divmoddi4 -------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements __divmoddi4 for the compiler_rt library.
//
//===----------------------------------------------------------------------===//


// Returns: a / b, *rem = a % b

COMPILER_RT_ABI di_int __divmoddi4(di_int a, di_int b, di_int *rem) {
  const int bits_in_dword_m1 = (int)(sizeof(di_int) * CHAR_BIT) - 1;
  di_int s_a = a >> bits_in_dword_m1;                   // s_a = a < 0 ? -1 : 0
  di_int s_b = b >> bits_in_dword_m1;                   // s_b = b < 0 ? -1 : 0
  a = (du_int)(a ^ s_a) - s_a;                          // negate if s_a == -1
  b = (du_int)(b ^ s_b) - s_b;                          // negate if s_b == -1
  s_b ^= s_a;                                           // sign of quotient
  du_int r;
  di_int q = (__udivmoddi4(a, b, &r) ^ s_b) - s_b;      // negate if s_b == -1
  *rem = (r ^ s_a) - s_a;                               // negate if s_a == -1
  return q;
}
//===-- divmodsi4.c - Implement __divmodsi4
//--------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements __divmodsi4 for the compiler_rt library.
//
//===----------------------------------------------------------------------===//


// Returns: a / b, *rem = a % b

COMPILER_RT_ABI si_int __divmodsi4(si_int a, si_int b, si_int *rem) {
  const int bits_in_word_m1 = (int)(sizeof(si_int) * CHAR_BIT) - 1;
  si_int s_a = a >> bits_in_word_m1;                    // s_a = a < 0 ? -1 : 0
  si_int s_b = b >> bits_in_word_m1;                    // s_b = b < 0 ? -1 : 0
  a = (su_int)(a ^ s_a) - s_a;                          // negate if s_a == -1
  b = (su_int)(b ^ s_b) - s_b;                          // negate if s_b == -1
  s_b ^= s_a;                                           // sign of quotient
  su_int r;
  si_int q = (__udivmodsi4(a, b, &r) ^ s_b) - s_b;      // negate if s_b == -1
  *rem = (r ^ s_a) - s_a;                               // negate if s_a == -1
  return q;
}
//===-- divmodti4.c - Implement __divmodti4 -------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements __divmodti4 for the compiler_rt library.
//
//===----------------------------------------------------------------------===//


#ifdef CRT_HAS_128BIT

// Returns: a / b, *rem = a % b

COMPILER_RT_ABI ti_int __divmodti4(ti_int a, ti_int b, ti_int *rem) {
  const int bits_in_tword_m1 = (int)(sizeof(ti_int) * CHAR_BIT) - 1;
  ti_int s_a = a >> bits_in_tword_m1;                   // s_a = a < 0 ? -1 : 0
  ti_int s_b = b >> bits_in_tword_m1;                   // s_b = b < 0 ? -1 : 0
  a = (tu_int)(a ^ s_a) - s_a;                          // negate if s_a == -1
  b = (tu_int)(b ^ s_b) - s_b;                          // negate if s_b == -1
  s_b ^= s_a;                                           // sign of quotient
  tu_int r;
  ti_int q = (__udivmodti4(a, b, &r) ^ s_b) - s_b;      // negate if s_b == -1
  *rem = (r ^ s_a) - s_a;                               // negate if s_a == -1
  return q;
}

#endif // CRT_HAS_128BIT
//===-- divsi3.c - Implement __divsi3 -------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// 2024/08/10 - Modified by mintsuki for use inside cc-runtime
//
//===----------------------------------------------------------------------===//
//
// This file implements __divsi3 for the compiler_rt library.
//
//===----------------------------------------------------------------------===//


// Returns: a / b

#define fixint_t si_int
#define fixuint_t su_int
// On CPUs without unsigned hardware division support,
//  this calls __udivsi3 (notice the cast to su_int).
// On CPUs with unsigned hardware division support,
//  this uses the unsigned division instruction.
#define COMPUTE_UDIV(a, b) ((su_int)(a) / (su_int)(b))
//===-- int_div_impl.inc - Integer division ---------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// 2024/10/20 - Modified by mintsuki for use inside cc-runtime
//
//===----------------------------------------------------------------------===//
//
// Helpers used by __udivsi3, __umodsi3, __udivdi3, and __umodsi3.
//
//===----------------------------------------------------------------------===//

#define clz(a) (sizeof(a) == sizeof(unsigned long long) ? __builtin_clzll(a) : clzsi(a))

#undef __udivXi3
#define __udivXi3 __udivXi3_5

// Adapted from Figure 3-40 of The PowerPC Compiler Writer's Guide
static __inline fixuint_t __udivXi3(fixuint_t n, fixuint_t d) {
  const unsigned N = sizeof(fixuint_t) * CHAR_BIT;
  // d == 0 cases are unspecified.
  unsigned sr = (d ? clz(d) : N) - (n ? clz(n) : N);
  // 0 <= sr <= N - 1 or sr is very large.
  if (sr > N - 1) // n < d
    return 0;
  if (sr == N - 1) // d == 1
    return n;
  ++sr;
  // 1 <= sr <= N - 1. Shifts do not trigger UB.
  fixuint_t r = n >> sr;
  n <<= N - sr;
  fixuint_t carry = 0;
  for (; sr > 0; --sr) {
    r = (r << 1) | (n >> (N - 1));
    n = (n << 1) | carry;
    // Branch-less version of:
    // carry = 0;
    // if (r >= d) r -= d, carry = 1;
    const fixint_t s = (fixint_t)(d - r - 1) >> (N - 1);
    carry = s & 1;
    r -= d & s;
  }
  n = (n << 1) | carry;
  return n;
}

#undef __umodXi3
#define __umodXi3 __umodXi3_6

// Mostly identical to __udivXi3 but the return values are different.
static __inline fixuint_t __umodXi3(fixuint_t n, fixuint_t d) {
  const unsigned N = sizeof(fixuint_t) * CHAR_BIT;
  // d == 0 cases are unspecified.
  unsigned sr = (d ? clz(d) : N) - (n ? clz(n) : N);
  // 0 <= sr <= N - 1 or sr is very large.
  if (sr > N - 1) // n < d
    return n;
  if (sr == N - 1) // d == 1
    return 0;
  ++sr;
  // 1 <= sr <= N - 1. Shifts do not trigger UB.
  fixuint_t r = n >> sr;
  n <<= N - sr;
  fixuint_t carry = 0;
  for (; sr > 0; --sr) {
    r = (r << 1) | (n >> (N - 1));
    n = (n << 1) | carry;
    // Branch-less version of:
    // carry = 0;
    // if (r >= d) r -= d, carry = 1;
    const fixint_t s = (fixint_t)(d - r - 1) >> (N - 1);
    carry = s & 1;
    r -= d & s;
  }
  return r;
}

#undef __divXi3

#ifdef COMPUTE_UDIV

#define __divXi3 __divXi3_7

static __inline fixint_t __divXi3(fixint_t a, fixint_t b) {
  const int N = (int)(sizeof(fixint_t) * CHAR_BIT) - 1;
  fixint_t s_a = a >> N;                            // s_a = a < 0 ? -1 : 0
  fixint_t s_b = b >> N;                            // s_b = b < 0 ? -1 : 0
  fixuint_t a_u = (fixuint_t)(a ^ s_a) + (-s_a);    // negate if s_a == -1
  fixuint_t b_u = (fixuint_t)(b ^ s_b) + (-s_b);    // negate if s_b == -1
  s_a ^= s_b;                                       // sign of quotient
  return (COMPUTE_UDIV(a_u, b_u) ^ s_a) + (-s_a);   // negate if s_a == -1
}
#endif // COMPUTE_UDIV

#undef __modXi3

#ifdef ASSIGN_UMOD

#define __modXi3 __modXi3_8

static __inline fixint_t __modXi3(fixint_t a, fixint_t b) {
  const int N = (int)(sizeof(fixint_t) * CHAR_BIT) - 1;
  fixint_t s = b >> N;                              // s = b < 0 ? -1 : 0
  fixuint_t b_u = (fixuint_t)(b ^ s) + (-s);        // negate if s == -1
  s = a >> N;                                       // s = a < 0 ? -1 : 0
  fixuint_t a_u = (fixuint_t)(a ^ s) + (-s);        // negate if s == -1
  fixuint_t res;
  ASSIGN_UMOD(res, a_u, b_u);
  return (res ^ s) + (-s);                          // negate if s == -1
}
#endif // ASSIGN_UMOD

#undef clz

COMPILER_RT_ABI si_int __divsi3(si_int a, si_int b) { return __divXi3(a, b); }

#if defined(__ARM_EABI__)
COMPILER_RT_ALIAS(__divsi3, __aeabi_idiv)
#endif

#undef fixint_t
#undef fixuint_t
#undef COMPUTE_UDIV
//===-- divti3.c - Implement __divti3 -------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// 2024/08/10 - Modified by mintsuki for use inside cc-runtime
//
//===----------------------------------------------------------------------===//
//
// This file implements __divti3 for the compiler_rt library.
//
//===----------------------------------------------------------------------===//


#ifdef CRT_HAS_128BIT

// Returns: a / b

#define fixint_t ti_int
#define fixuint_t tu_int
#define COMPUTE_UDIV(a, b) __udivmodti4((a), (b), (tu_int *)0)
//===-- int_div_impl.inc - Integer division ---------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// 2024/10/20 - Modified by mintsuki for use inside cc-runtime
//
//===----------------------------------------------------------------------===//
//
// Helpers used by __udivsi3, __umodsi3, __udivdi3, and __umodsi3.
//
//===----------------------------------------------------------------------===//

#define clz(a) (sizeof(a) == sizeof(unsigned long long) ? __builtin_clzll(a) : clzsi(a))

#undef __udivXi3
#define __udivXi3 __udivXi3_9

// Adapted from Figure 3-40 of The PowerPC Compiler Writer's Guide
static __inline fixuint_t __udivXi3(fixuint_t n, fixuint_t d) {
  const unsigned N = sizeof(fixuint_t) * CHAR_BIT;
  // d == 0 cases are unspecified.
  unsigned sr = (d ? clz(d) : N) - (n ? clz(n) : N);
  // 0 <= sr <= N - 1 or sr is very large.
  if (sr > N - 1) // n < d
    return 0;
  if (sr == N - 1) // d == 1
    return n;
  ++sr;
  // 1 <= sr <= N - 1. Shifts do not trigger UB.
  fixuint_t r = n >> sr;
  n <<= N - sr;
  fixuint_t carry = 0;
  for (; sr > 0; --sr) {
    r = (r << 1) | (n >> (N - 1));
    n = (n << 1) | carry;
    // Branch-less version of:
    // carry = 0;
    // if (r >= d) r -= d, carry = 1;
    const fixint_t s = (fixint_t)(d - r - 1) >> (N - 1);
    carry = s & 1;
    r -= d & s;
  }
  n = (n << 1) | carry;
  return n;
}

#undef __umodXi3
#define __umodXi3 __umodXi3_10

// Mostly identical to __udivXi3 but the return values are different.
static __inline fixuint_t __umodXi3(fixuint_t n, fixuint_t d) {
  const unsigned N = sizeof(fixuint_t) * CHAR_BIT;
  // d == 0 cases are unspecified.
  unsigned sr = (d ? clz(d) : N) - (n ? clz(n) : N);
  // 0 <= sr <= N - 1 or sr is very large.
  if (sr > N - 1) // n < d
    return n;
  if (sr == N - 1) // d == 1
    return 0;
  ++sr;
  // 1 <= sr <= N - 1. Shifts do not trigger UB.
  fixuint_t r = n >> sr;
  n <<= N - sr;
  fixuint_t carry = 0;
  for (; sr > 0; --sr) {
    r = (r << 1) | (n >> (N - 1));
    n = (n << 1) | carry;
    // Branch-less version of:
    // carry = 0;
    // if (r >= d) r -= d, carry = 1;
    const fixint_t s = (fixint_t)(d - r - 1) >> (N - 1);
    carry = s & 1;
    r -= d & s;
  }
  return r;
}

#undef __divXi3

#ifdef COMPUTE_UDIV

#define __divXi3 __divXi3_11

static __inline fixint_t __divXi3(fixint_t a, fixint_t b) {
  const int N = (int)(sizeof(fixint_t) * CHAR_BIT) - 1;
  fixint_t s_a = a >> N;                            // s_a = a < 0 ? -1 : 0
  fixint_t s_b = b >> N;                            // s_b = b < 0 ? -1 : 0
  fixuint_t a_u = (fixuint_t)(a ^ s_a) + (-s_a);    // negate if s_a == -1
  fixuint_t b_u = (fixuint_t)(b ^ s_b) + (-s_b);    // negate if s_b == -1
  s_a ^= s_b;                                       // sign of quotient
  return (COMPUTE_UDIV(a_u, b_u) ^ s_a) + (-s_a);   // negate if s_a == -1
}
#endif // COMPUTE_UDIV

#undef __modXi3

#ifdef ASSIGN_UMOD

#define __modXi3 __modXi3_12

static __inline fixint_t __modXi3(fixint_t a, fixint_t b) {
  const int N = (int)(sizeof(fixint_t) * CHAR_BIT) - 1;
  fixint_t s = b >> N;                              // s = b < 0 ? -1 : 0
  fixuint_t b_u = (fixuint_t)(b ^ s) + (-s);        // negate if s == -1
  s = a >> N;                                       // s = a < 0 ? -1 : 0
  fixuint_t a_u = (fixuint_t)(a ^ s) + (-s);        // negate if s == -1
  fixuint_t res;
  ASSIGN_UMOD(res, a_u, b_u);
  return (res ^ s) + (-s);                          // negate if s == -1
}
#endif // ASSIGN_UMOD

#undef clz

COMPILER_RT_ABI ti_int __divti3(ti_int a, ti_int b) { return __divXi3(a, b); }

#undef fixint_t
#undef fixuint_t
#undef COMPUTE_UDIV

#endif // CRT_HAS_128BIT
//===-- ffsdi2.c - Implement __ffsdi2 -------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements __ffsdi2 for the compiler_rt library.
//
//===----------------------------------------------------------------------===//


// Returns: the index of the least significant 1-bit in a, or
// the value zero if a is zero. The least significant bit is index one.

COMPILER_RT_ABI int __ffsdi2(di_int a) {
  dwords x;
  x.all = a;
  if (x.s.low == 0) {
    if (x.s.high == 0)
      return 0;
    return ctzsi(x.s.high) + (1 + sizeof(si_int) * CHAR_BIT);
  }
  return ctzsi(x.s.low) + 1;
}
//===-- ffssi2.c - Implement __ffssi2 -------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements __ffssi2 for the compiler_rt library.
//
//===----------------------------------------------------------------------===//


// Returns: the index of the least significant 1-bit in a, or
// the value zero if a is zero. The least significant bit is index one.

COMPILER_RT_ABI int __ffssi2(si_int a) {
  if (a == 0) {
    return 0;
  }
  return ctzsi(a) + 1;
}
//===-- ffsti2.c - Implement __ffsti2 -------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements __ffsti2 for the compiler_rt library.
//
//===----------------------------------------------------------------------===//


#ifdef CRT_HAS_128BIT

// Returns: the index of the least significant 1-bit in a, or
// the value zero if a is zero. The least significant bit is index one.

COMPILER_RT_ABI int __ffsti2(ti_int a) {
  twords x;
  x.all = a;
  if (x.s.low == 0) {
    if (x.s.high == 0)
      return 0;
    return __builtin_ctzll(x.s.high) + (1 + sizeof(di_int) * CHAR_BIT);
  }
  return __builtin_ctzll(x.s.low) + 1;
}

#endif // CRT_HAS_128BIT
//===-- int_util.c - Implement internal utilities -------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// 2024/01/21 - Modified by mintsuki for use inside cc-runtime
//
//===----------------------------------------------------------------------===//


// NOTE: The definitions in this file are declared weak because we clients to be
// able to arbitrarily package individual functions into separate .a files. If
// we did not declare these weak, some link situations might end up seeing
// duplicate strong definitions of the same symbol.
//
// We can't use this solution for kernel use (which may not support weak), but
// currently expect that when built for kernel use all the functionality is
// packaged into a single library.

__attribute__((weak))
__attribute__((visibility("hidden")))
void __compilerrt_abort_impl(const char *file, int line, const char *function) {
  (void)file; (void)line; (void)function;
#if !__STDC_HOSTED__
  // Avoid depending on libc when compiling with -ffreestanding.
  __builtin_trap();
#else
  __builtin_abort();
#endif
}
//===-- lshrdi3.c - Implement __lshrdi3 -----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements __lshrdi3 for the compiler_rt library.
//
//===----------------------------------------------------------------------===//


// Returns: logical a >> b

// Precondition:  0 <= b < bits_in_dword

COMPILER_RT_ABI di_int __lshrdi3(di_int a, int b) {
  const int bits_in_word = (int)(sizeof(si_int) * CHAR_BIT);
  udwords input;
  udwords result;
  input.all = a;
  if (b & bits_in_word) /* bits_in_word <= b < bits_in_dword */ {
    result.s.high = 0;
    result.s.low = input.s.high >> (b - bits_in_word);
  } else /* 0 <= b < bits_in_word */ {
    if (b == 0)
      return a;
    result.s.high = input.s.high >> b;
    result.s.low = (input.s.high << (bits_in_word - b)) | (input.s.low >> b);
  }
  return result.all;
}

#if defined(__ARM_EABI__)
COMPILER_RT_ALIAS(__lshrdi3, __aeabi_llsr)
#endif
//===-- lshrti3.c - Implement __lshrti3 -----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements __lshrti3 for the compiler_rt library.
//
//===----------------------------------------------------------------------===//


#ifdef CRT_HAS_128BIT

// Returns: logical a >> b

// Precondition:  0 <= b < bits_in_tword

COMPILER_RT_ABI ti_int __lshrti3(ti_int a, int b) {
  const int bits_in_dword = (int)(sizeof(di_int) * CHAR_BIT);
  utwords input;
  utwords result;
  input.all = a;
  if (b & bits_in_dword) /* bits_in_dword <= b < bits_in_tword */ {
    result.s.high = 0;
    result.s.low = input.s.high >> (b - bits_in_dword);
  } else /* 0 <= b < bits_in_dword */ {
    if (b == 0)
      return a;
    result.s.high = input.s.high >> b;
    result.s.low = (input.s.high << (bits_in_dword - b)) | (input.s.low >> b);
  }
  return result.all;
}

#endif // CRT_HAS_128BIT
//===-- moddi3.c - Implement __moddi3 -------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// 2024/08/10 - Modified by mintsuki for use inside cc-runtime
//
//===----------------------------------------------------------------------===//
//
// This file implements __moddi3 for the compiler_rt library.
//
//===----------------------------------------------------------------------===//


// Returns: a % b

#define fixint_t di_int
#define fixuint_t du_int
#define ASSIGN_UMOD(res, a, b) __udivmoddi4((a), (b), &(res))
//===-- int_div_impl.inc - Integer division ---------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// 2024/10/20 - Modified by mintsuki for use inside cc-runtime
//
//===----------------------------------------------------------------------===//
//
// Helpers used by __udivsi3, __umodsi3, __udivdi3, and __umodsi3.
//
//===----------------------------------------------------------------------===//

#define clz(a) (sizeof(a) == sizeof(unsigned long long) ? __builtin_clzll(a) : clzsi(a))

#undef __udivXi3
#define __udivXi3 __udivXi3_13

// Adapted from Figure 3-40 of The PowerPC Compiler Writer's Guide
static __inline fixuint_t __udivXi3(fixuint_t n, fixuint_t d) {
  const unsigned N = sizeof(fixuint_t) * CHAR_BIT;
  // d == 0 cases are unspecified.
  unsigned sr = (d ? clz(d) : N) - (n ? clz(n) : N);
  // 0 <= sr <= N - 1 or sr is very large.
  if (sr > N - 1) // n < d
    return 0;
  if (sr == N - 1) // d == 1
    return n;
  ++sr;
  // 1 <= sr <= N - 1. Shifts do not trigger UB.
  fixuint_t r = n >> sr;
  n <<= N - sr;
  fixuint_t carry = 0;
  for (; sr > 0; --sr) {
    r = (r << 1) | (n >> (N - 1));
    n = (n << 1) | carry;
    // Branch-less version of:
    // carry = 0;
    // if (r >= d) r -= d, carry = 1;
    const fixint_t s = (fixint_t)(d - r - 1) >> (N - 1);
    carry = s & 1;
    r -= d & s;
  }
  n = (n << 1) | carry;
  return n;
}

#undef __umodXi3
#define __umodXi3 __umodXi3_14

// Mostly identical to __udivXi3 but the return values are different.
static __inline fixuint_t __umodXi3(fixuint_t n, fixuint_t d) {
  const unsigned N = sizeof(fixuint_t) * CHAR_BIT;
  // d == 0 cases are unspecified.
  unsigned sr = (d ? clz(d) : N) - (n ? clz(n) : N);
  // 0 <= sr <= N - 1 or sr is very large.
  if (sr > N - 1) // n < d
    return n;
  if (sr == N - 1) // d == 1
    return 0;
  ++sr;
  // 1 <= sr <= N - 1. Shifts do not trigger UB.
  fixuint_t r = n >> sr;
  n <<= N - sr;
  fixuint_t carry = 0;
  for (; sr > 0; --sr) {
    r = (r << 1) | (n >> (N - 1));
    n = (n << 1) | carry;
    // Branch-less version of:
    // carry = 0;
    // if (r >= d) r -= d, carry = 1;
    const fixint_t s = (fixint_t)(d - r - 1) >> (N - 1);
    carry = s & 1;
    r -= d & s;
  }
  return r;
}

#undef __divXi3

#ifdef COMPUTE_UDIV

#define __divXi3 __divXi3_15

static __inline fixint_t __divXi3(fixint_t a, fixint_t b) {
  const int N = (int)(sizeof(fixint_t) * CHAR_BIT) - 1;
  fixint_t s_a = a >> N;                            // s_a = a < 0 ? -1 : 0
  fixint_t s_b = b >> N;                            // s_b = b < 0 ? -1 : 0
  fixuint_t a_u = (fixuint_t)(a ^ s_a) + (-s_a);    // negate if s_a == -1
  fixuint_t b_u = (fixuint_t)(b ^ s_b) + (-s_b);    // negate if s_b == -1
  s_a ^= s_b;                                       // sign of quotient
  return (COMPUTE_UDIV(a_u, b_u) ^ s_a) + (-s_a);   // negate if s_a == -1
}
#endif // COMPUTE_UDIV

#undef __modXi3

#ifdef ASSIGN_UMOD

#define __modXi3 __modXi3_16

static __inline fixint_t __modXi3(fixint_t a, fixint_t b) {
  const int N = (int)(sizeof(fixint_t) * CHAR_BIT) - 1;
  fixint_t s = b >> N;                              // s = b < 0 ? -1 : 0
  fixuint_t b_u = (fixuint_t)(b ^ s) + (-s);        // negate if s == -1
  s = a >> N;                                       // s = a < 0 ? -1 : 0
  fixuint_t a_u = (fixuint_t)(a ^ s) + (-s);        // negate if s == -1
  fixuint_t res;
  ASSIGN_UMOD(res, a_u, b_u);
  return (res ^ s) + (-s);                          // negate if s == -1
}
#endif // ASSIGN_UMOD

#undef clz

COMPILER_RT_ABI di_int __moddi3(di_int a, di_int b) { return __modXi3(a, b); }

#undef fixint_t
#undef fixuint_t
#undef ASSIGN_UMOD
//===-- modsi3.c - Implement __modsi3 -------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements __modsi3 for the compiler_rt library.
//
//===----------------------------------------------------------------------===//


// Returns: a % b

COMPILER_RT_ABI si_int __modsi3(si_int a, si_int b) {
  return a - __divsi3(a, b) * b;
}
//===-- modti3.c - Implement __modti3 -------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// 2024/08/10 - Modified by mintsuki for use inside cc-runtime
//
//===----------------------------------------------------------------------===//
//
// This file implements __modti3 for the compiler_rt library.
//
//===----------------------------------------------------------------------===//


#ifdef CRT_HAS_128BIT

// Returns: a % b

#define fixint_t ti_int
#define fixuint_t tu_int
#define ASSIGN_UMOD(res, a, b) __udivmodti4((a), (b), &(res))
//===-- int_div_impl.inc - Integer division ---------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// 2024/10/20 - Modified by mintsuki for use inside cc-runtime
//
//===----------------------------------------------------------------------===//
//
// Helpers used by __udivsi3, __umodsi3, __udivdi3, and __umodsi3.
//
//===----------------------------------------------------------------------===//

#define clz(a) (sizeof(a) == sizeof(unsigned long long) ? __builtin_clzll(a) : clzsi(a))

#undef __udivXi3
#define __udivXi3 __udivXi3_17

// Adapted from Figure 3-40 of The PowerPC Compiler Writer's Guide
static __inline fixuint_t __udivXi3(fixuint_t n, fixuint_t d) {
  const unsigned N = sizeof(fixuint_t) * CHAR_BIT;
  // d == 0 cases are unspecified.
  unsigned sr = (d ? clz(d) : N) - (n ? clz(n) : N);
  // 0 <= sr <= N - 1 or sr is very large.
  if (sr > N - 1) // n < d
    return 0;
  if (sr == N - 1) // d == 1
    return n;
  ++sr;
  // 1 <= sr <= N - 1. Shifts do not trigger UB.
  fixuint_t r = n >> sr;
  n <<= N - sr;
  fixuint_t carry = 0;
  for (; sr > 0; --sr) {
    r = (r << 1) | (n >> (N - 1));
    n = (n << 1) | carry;
    // Branch-less version of:
    // carry = 0;
    // if (r >= d) r -= d, carry = 1;
    const fixint_t s = (fixint_t)(d - r - 1) >> (N - 1);
    carry = s & 1;
    r -= d & s;
  }
  n = (n << 1) | carry;
  return n;
}

#undef __umodXi3
#define __umodXi3 __umodXi3_18

// Mostly identical to __udivXi3 but the return values are different.
static __inline fixuint_t __umodXi3(fixuint_t n, fixuint_t d) {
  const unsigned N = sizeof(fixuint_t) * CHAR_BIT;
  // d == 0 cases are unspecified.
  unsigned sr = (d ? clz(d) : N) - (n ? clz(n) : N);
  // 0 <= sr <= N - 1 or sr is very large.
  if (sr > N - 1) // n < d
    return n;
  if (sr == N - 1) // d == 1
    return 0;
  ++sr;
  // 1 <= sr <= N - 1. Shifts do not trigger UB.
  fixuint_t r = n >> sr;
  n <<= N - sr;
  fixuint_t carry = 0;
  for (; sr > 0; --sr) {
    r = (r << 1) | (n >> (N - 1));
    n = (n << 1) | carry;
    // Branch-less version of:
    // carry = 0;
    // if (r >= d) r -= d, carry = 1;
    const fixint_t s = (fixint_t)(d - r - 1) >> (N - 1);
    carry = s & 1;
    r -= d & s;
  }
  return r;
}

#undef __divXi3

#ifdef COMPUTE_UDIV

#define __divXi3 __divXi3_19

static __inline fixint_t __divXi3(fixint_t a, fixint_t b) {
  const int N = (int)(sizeof(fixint_t) * CHAR_BIT) - 1;
  fixint_t s_a = a >> N;                            // s_a = a < 0 ? -1 : 0
  fixint_t s_b = b >> N;                            // s_b = b < 0 ? -1 : 0
  fixuint_t a_u = (fixuint_t)(a ^ s_a) + (-s_a);    // negate if s_a == -1
  fixuint_t b_u = (fixuint_t)(b ^ s_b) + (-s_b);    // negate if s_b == -1
  s_a ^= s_b;                                       // sign of quotient
  return (COMPUTE_UDIV(a_u, b_u) ^ s_a) + (-s_a);   // negate if s_a == -1
}
#endif // COMPUTE_UDIV

#undef __modXi3

#ifdef ASSIGN_UMOD

#define __modXi3 __modXi3_20

static __inline fixint_t __modXi3(fixint_t a, fixint_t b) {
  const int N = (int)(sizeof(fixint_t) * CHAR_BIT) - 1;
  fixint_t s = b >> N;                              // s = b < 0 ? -1 : 0
  fixuint_t b_u = (fixuint_t)(b ^ s) + (-s);        // negate if s == -1
  s = a >> N;                                       // s = a < 0 ? -1 : 0
  fixuint_t a_u = (fixuint_t)(a ^ s) + (-s);        // negate if s == -1
  fixuint_t res;
  ASSIGN_UMOD(res, a_u, b_u);
  return (res ^ s) + (-s);                          // negate if s == -1
}
#endif // ASSIGN_UMOD

#undef clz

COMPILER_RT_ABI ti_int __modti3(ti_int a, ti_int b) { return __modXi3(a, b); }

#undef fixint_t
#undef fixuint_t
#undef ASSIGN_UMOD

#endif // CRT_HAS_128BIT
//===-- muldi3.c - Implement __muldi3 -------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements __muldi3 for the compiler_rt library.
//
//===----------------------------------------------------------------------===//


// Returns: a * b

static di_int __muldsi3(su_int a, su_int b) {
  dwords r;
  const int bits_in_word_2 = (int)(sizeof(si_int) * CHAR_BIT) / 2;
  const su_int lower_mask = (su_int)~0 >> bits_in_word_2;
  r.s.low = (a & lower_mask) * (b & lower_mask);
  su_int t = r.s.low >> bits_in_word_2;
  r.s.low &= lower_mask;
  t += (a >> bits_in_word_2) * (b & lower_mask);
  r.s.low += (t & lower_mask) << bits_in_word_2;
  r.s.high = t >> bits_in_word_2;
  t = r.s.low >> bits_in_word_2;
  r.s.low &= lower_mask;
  t += (b >> bits_in_word_2) * (a & lower_mask);
  r.s.low += (t & lower_mask) << bits_in_word_2;
  r.s.high += t >> bits_in_word_2;
  r.s.high += (a >> bits_in_word_2) * (b >> bits_in_word_2);
  return r.all;
}

// Returns: a * b

COMPILER_RT_ABI di_int __muldi3(di_int a, di_int b) {
  dwords x;
  x.all = a;
  dwords y;
  y.all = b;
  dwords r;
  r.all = __muldsi3(x.s.low, y.s.low);
  r.s.high += x.s.high * y.s.low + x.s.low * y.s.high;
  return r.all;
}

#if defined(__ARM_EABI__)
COMPILER_RT_ALIAS(__muldi3, __aeabi_lmul)
#endif
//===-- mulodi4.c - Implement __mulodi4 -----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// 2024/08/10 - Modified by mintsuki for use inside cc-runtime
//
//===----------------------------------------------------------------------===//
//
// This file implements __mulodi4 for the compiler_rt library.
//
//===----------------------------------------------------------------------===//

#define fixint_t di_int
#define fixuint_t du_int
//===-- int_mulo_impl.inc - Implement __mulo[sdt]i4 ---------------*- C -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// 2024/10/20 - Modified by mintsuki for use inside cc-runtime
//
//===----------------------------------------------------------------------===//
//
// Helper used by __mulosi4, __mulodi4 and __muloti4.
//
//===----------------------------------------------------------------------===//


// Returns: a * b

// Effects: sets *overflow to 1  if a * b overflows

#undef __muloXi4
#define __muloXi4 __muloXi4_21

static __inline fixint_t __muloXi4(fixint_t a, fixint_t b, int *overflow) {
  const int N = (int)(sizeof(fixint_t) * CHAR_BIT);
  const fixint_t MIN = (fixint_t)((fixuint_t)1 << (N - 1));
  const fixint_t MAX = ~MIN;
  *overflow = 0;
  fixint_t result = (fixuint_t)a * b;
  if (a == MIN) {
    if (b != 0 && b != 1)
      *overflow = 1;
    return result;
  }
  if (b == MIN) {
    if (a != 0 && a != 1)
      *overflow = 1;
    return result;
  }
  fixint_t sa = a >> (N - 1);
  fixint_t abs_a = (a ^ sa) - sa;
  fixint_t sb = b >> (N - 1);
  fixint_t abs_b = (b ^ sb) - sb;
  if (abs_a < 2 || abs_b < 2)
    return result;
  if (sa == sb) {
    if (abs_a > MAX / abs_b)
      *overflow = 1;
  } else {
    if (abs_a > MIN / -abs_b)
      *overflow = 1;
  }
  return result;
}

// Returns: a * b

// Effects: sets *overflow to 1  if a * b overflows

COMPILER_RT_ABI di_int __mulodi4(di_int a, di_int b, int *overflow) {
  return __muloXi4(a, b, overflow);
}

#undef fixint_t
#undef fixuint_t
//===-- mulosi4.c - Implement __mulosi4 -----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// 2024/08/10 - Modified by mintsuki for use inside cc-runtime
//
//===----------------------------------------------------------------------===//
//
// This file implements __mulosi4 for the compiler_rt library.
//
//===----------------------------------------------------------------------===//

#define fixint_t si_int
#define fixuint_t su_int
//===-- int_mulo_impl.inc - Implement __mulo[sdt]i4 ---------------*- C -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// 2024/10/20 - Modified by mintsuki for use inside cc-runtime
//
//===----------------------------------------------------------------------===//
//
// Helper used by __mulosi4, __mulodi4 and __muloti4.
//
//===----------------------------------------------------------------------===//


// Returns: a * b

// Effects: sets *overflow to 1  if a * b overflows

#undef __muloXi4
#define __muloXi4 __muloXi4_22

static __inline fixint_t __muloXi4(fixint_t a, fixint_t b, int *overflow) {
  const int N = (int)(sizeof(fixint_t) * CHAR_BIT);
  const fixint_t MIN = (fixint_t)((fixuint_t)1 << (N - 1));
  const fixint_t MAX = ~MIN;
  *overflow = 0;
  fixint_t result = (fixuint_t)a * b;
  if (a == MIN) {
    if (b != 0 && b != 1)
      *overflow = 1;
    return result;
  }
  if (b == MIN) {
    if (a != 0 && a != 1)
      *overflow = 1;
    return result;
  }
  fixint_t sa = a >> (N - 1);
  fixint_t abs_a = (a ^ sa) - sa;
  fixint_t sb = b >> (N - 1);
  fixint_t abs_b = (b ^ sb) - sb;
  if (abs_a < 2 || abs_b < 2)
    return result;
  if (sa == sb) {
    if (abs_a > MAX / abs_b)
      *overflow = 1;
  } else {
    if (abs_a > MIN / -abs_b)
      *overflow = 1;
  }
  return result;
}

// Returns: a * b

// Effects: sets *overflow to 1  if a * b overflows

COMPILER_RT_ABI si_int __mulosi4(si_int a, si_int b, int *overflow) {
  return __muloXi4(a, b, overflow);
}

#undef fixint_t
#undef fixuint_t
//===-- muloti4.c - Implement __muloti4 -----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// 2024/08/10 - Modified by mintsuki for use inside cc-runtime
//
//===----------------------------------------------------------------------===//
//
// This file implements __muloti4 for the compiler_rt library.
//
//===----------------------------------------------------------------------===//


#ifdef CRT_HAS_128BIT

// Returns: a * b

// Effects: sets *overflow to 1  if a * b overflows

#define fixint_t ti_int
#define fixuint_t tu_int
//===-- int_mulo_impl.inc - Implement __mulo[sdt]i4 ---------------*- C -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// 2024/10/20 - Modified by mintsuki for use inside cc-runtime
//
//===----------------------------------------------------------------------===//
//
// Helper used by __mulosi4, __mulodi4 and __muloti4.
//
//===----------------------------------------------------------------------===//


// Returns: a * b

// Effects: sets *overflow to 1  if a * b overflows

#undef __muloXi4
#define __muloXi4 __muloXi4_23

static __inline fixint_t __muloXi4(fixint_t a, fixint_t b, int *overflow) {
  const int N = (int)(sizeof(fixint_t) * CHAR_BIT);
  const fixint_t MIN = (fixint_t)((fixuint_t)1 << (N - 1));
  const fixint_t MAX = ~MIN;
  *overflow = 0;
  fixint_t result = (fixuint_t)a * b;
  if (a == MIN) {
    if (b != 0 && b != 1)
      *overflow = 1;
    return result;
  }
  if (b == MIN) {
    if (a != 0 && a != 1)
      *overflow = 1;
    return result;
  }
  fixint_t sa = a >> (N - 1);
  fixint_t abs_a = (a ^ sa) - sa;
  fixint_t sb = b >> (N - 1);
  fixint_t abs_b = (b ^ sb) - sb;
  if (abs_a < 2 || abs_b < 2)
    return result;
  if (sa == sb) {
    if (abs_a > MAX / abs_b)
      *overflow = 1;
  } else {
    if (abs_a > MIN / -abs_b)
      *overflow = 1;
  }
  return result;
}

COMPILER_RT_ABI ti_int __muloti4(ti_int a, ti_int b, int *overflow) {
  return __muloXi4(a, b, overflow);
}

#undef fixint_t
#undef fixuint_t

#endif // CRT_HAS_128BIT
//===-- multi3.c - Implement __multi3 -------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements __multi3 for the compiler_rt library.
//
//===----------------------------------------------------------------------===//


#ifdef CRT_HAS_128BIT

// Returns: a * b

static ti_int __mulddi3(du_int a, du_int b) {
  twords r;
  const int bits_in_dword_2 = (int)(sizeof(di_int) * CHAR_BIT) / 2;
  const du_int lower_mask = (du_int)~0 >> bits_in_dword_2;
  r.s.low = (a & lower_mask) * (b & lower_mask);
  du_int t = r.s.low >> bits_in_dword_2;
  r.s.low &= lower_mask;
  t += (a >> bits_in_dword_2) * (b & lower_mask);
  r.s.low += (t & lower_mask) << bits_in_dword_2;
  r.s.high = t >> bits_in_dword_2;
  t = r.s.low >> bits_in_dword_2;
  r.s.low &= lower_mask;
  t += (b >> bits_in_dword_2) * (a & lower_mask);
  r.s.low += (t & lower_mask) << bits_in_dword_2;
  r.s.high += t >> bits_in_dword_2;
  r.s.high += (a >> bits_in_dword_2) * (b >> bits_in_dword_2);
  return r.all;
}

// Returns: a * b

COMPILER_RT_ABI ti_int __multi3(ti_int a, ti_int b) {
  twords x;
  x.all = a;
  twords y;
  y.all = b;
  twords r;
  r.all = __mulddi3(x.s.low, y.s.low);
  r.s.high += x.s.high * y.s.low + x.s.low * y.s.high;
  return r.all;
}

#endif // CRT_HAS_128BIT
//===-- mulvdi3.c - Implement __mulvdi3 -----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// 2024/08/10 - Modified by mintsuki for use inside cc-runtime
//
//===----------------------------------------------------------------------===//
//
// This file implements __mulvdi3 for the compiler_rt library.
//
//===----------------------------------------------------------------------===//

#define fixint_t di_int
#define fixuint_t du_int
//===-- int_mulv_impl.inc - Implement __mulv[sdt]i3 ---------------*- C -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// 2024/10/20 - Modified by mintsuki for use inside cc-runtime
//
//===----------------------------------------------------------------------===//
//
// Helper used by __mulvsi3, __mulvdi3 and __mulvti3.
//
//===----------------------------------------------------------------------===//


// Returns: a * b

// Effects: aborts if a * b overflows

#undef __mulvXi3
#define __mulvXi3 __mulvXi3_24

static __inline fixint_t __mulvXi3(fixint_t a, fixint_t b) {
  const int N = (int)(sizeof(fixint_t) * CHAR_BIT);
  const fixint_t MIN = (fixint_t)((fixuint_t)1 << (N - 1));
  const fixint_t MAX = ~MIN;
  if (a == MIN) {
    if (b == 0 || b == 1)
      return a * b;
    compilerrt_abort();
  }
  if (b == MIN) {
    if (a == 0 || a == 1)
      return a * b;
    compilerrt_abort();
  }
  fixint_t sa = a >> (N - 1);
  fixint_t abs_a = (a ^ sa) - sa;
  fixint_t sb = b >> (N - 1);
  fixint_t abs_b = (b ^ sb) - sb;
  if (abs_a < 2 || abs_b < 2)
    return a * b;
  if (sa == sb) {
    if (abs_a > MAX / abs_b)
      compilerrt_abort();
  } else {
    if (abs_a > MIN / -abs_b)
      compilerrt_abort();
  }
  return a * b;
}

// Returns: a * b

// Effects: aborts if a * b overflows

COMPILER_RT_ABI di_int __mulvdi3(di_int a, di_int b) { return __mulvXi3(a, b); }

#undef fixint_t
#undef fixuint_t
//===-- mulvsi3.c - Implement __mulvsi3 -----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// 2024/08/10 - Modified by mintsuki for use inside cc-runtime
//
//===----------------------------------------------------------------------===//
//
// This file implements __mulvsi3 for the compiler_rt library.
//
//===----------------------------------------------------------------------===//

#define fixint_t si_int
#define fixuint_t su_int
//===-- int_mulv_impl.inc - Implement __mulv[sdt]i3 ---------------*- C -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// 2024/10/20 - Modified by mintsuki for use inside cc-runtime
//
//===----------------------------------------------------------------------===//
//
// Helper used by __mulvsi3, __mulvdi3 and __mulvti3.
//
//===----------------------------------------------------------------------===//


// Returns: a * b

// Effects: aborts if a * b overflows

#undef __mulvXi3
#define __mulvXi3 __mulvXi3_25

static __inline fixint_t __mulvXi3(fixint_t a, fixint_t b) {
  const int N = (int)(sizeof(fixint_t) * CHAR_BIT);
  const fixint_t MIN = (fixint_t)((fixuint_t)1 << (N - 1));
  const fixint_t MAX = ~MIN;
  if (a == MIN) {
    if (b == 0 || b == 1)
      return a * b;
    compilerrt_abort();
  }
  if (b == MIN) {
    if (a == 0 || a == 1)
      return a * b;
    compilerrt_abort();
  }
  fixint_t sa = a >> (N - 1);
  fixint_t abs_a = (a ^ sa) - sa;
  fixint_t sb = b >> (N - 1);
  fixint_t abs_b = (b ^ sb) - sb;
  if (abs_a < 2 || abs_b < 2)
    return a * b;
  if (sa == sb) {
    if (abs_a > MAX / abs_b)
      compilerrt_abort();
  } else {
    if (abs_a > MIN / -abs_b)
      compilerrt_abort();
  }
  return a * b;
}

// Returns: a * b

// Effects: aborts if a * b overflows

COMPILER_RT_ABI si_int __mulvsi3(si_int a, si_int b) { return __mulvXi3(a, b); }

#undef fixint_t
#undef fixuint_t
//===-- mulvti3.c - Implement __mulvti3 -----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// 2024/08/10 - Modified by mintsuki for use inside cc-runtime
//
//===----------------------------------------------------------------------===//
//
// This file implements __mulvti3 for the compiler_rt library.
//
//===----------------------------------------------------------------------===//


#ifdef CRT_HAS_128BIT

// Returns: a * b

// Effects: aborts if a * b overflows

#define fixint_t ti_int
#define fixuint_t tu_int
//===-- int_mulv_impl.inc - Implement __mulv[sdt]i3 ---------------*- C -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// 2024/10/20 - Modified by mintsuki for use inside cc-runtime
//
//===----------------------------------------------------------------------===//
//
// Helper used by __mulvsi3, __mulvdi3 and __mulvti3.
//
//===----------------------------------------------------------------------===//


// Returns: a * b

// Effects: aborts if a * b overflows

#undef __mulvXi3
#define __mulvXi3 __mulvXi3_26

static __inline fixint_t __mulvXi3(fixint_t a, fixint_t b) {
  const int N = (int)(sizeof(fixint_t) * CHAR_BIT);
  const fixint_t MIN = (fixint_t)((fixuint_t)1 << (N - 1));
  const fixint_t MAX = ~MIN;
  if (a == MIN) {
    if (b == 0 || b == 1)
      return a * b;
    compilerrt_abort();
  }
  if (b == MIN) {
    if (a == 0 || a == 1)
      return a * b;
    compilerrt_abort();
  }
  fixint_t sa = a >> (N - 1);
  fixint_t abs_a = (a ^ sa) - sa;
  fixint_t sb = b >> (N - 1);
  fixint_t abs_b = (b ^ sb) - sb;
  if (abs_a < 2 || abs_b < 2)
    return a * b;
  if (sa == sb) {
    if (abs_a > MAX / abs_b)
      compilerrt_abort();
  } else {
    if (abs_a > MIN / -abs_b)
      compilerrt_abort();
  }
  return a * b;
}

COMPILER_RT_ABI ti_int __mulvti3(ti_int a, ti_int b) { return __mulvXi3(a, b); }

#undef fixint_t
#undef fixuint_t

#endif // CRT_HAS_128BIT
//===-- negdi2.c - Implement __negdi2 -------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements __negdi2 for the compiler_rt library.
//
//===----------------------------------------------------------------------===//


// Returns: -a

COMPILER_RT_ABI di_int __negdi2(di_int a) {
  // Note: this routine is here for API compatibility; any sane compiler
  // should expand it inline.
  return -(du_int)a;
}
//===-- negti2.c - Implement __negti2 -------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements __negti2 for the compiler_rt library.
//
//===----------------------------------------------------------------------===//


#ifdef CRT_HAS_128BIT

// Returns: -a

COMPILER_RT_ABI ti_int __negti2(ti_int a) {
  // Note: this routine is here for API compatibility; any sane compiler
  // should expand it inline.
  return -(tu_int)a;
}

#endif // CRT_HAS_128BIT
//===-- negvdi2.c - Implement __negvdi2 -----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements __negvdi2 for the compiler_rt library.
//
//===----------------------------------------------------------------------===//


// Returns: -a

// Effects: aborts if -a overflows

COMPILER_RT_ABI di_int __negvdi2(di_int a) {
  const di_int MIN =
      (di_int)((du_int)1 << ((int)(sizeof(di_int) * CHAR_BIT) - 1));
  if (a == MIN)
    compilerrt_abort();
  return -a;
}
//===-- negvsi2.c - Implement __negvsi2 -----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements __negvsi2 for the compiler_rt library.
//
//===----------------------------------------------------------------------===//


// Returns: -a

// Effects: aborts if -a overflows

COMPILER_RT_ABI si_int __negvsi2(si_int a) {
  const si_int MIN =
      (si_int)((su_int)1 << ((int)(sizeof(si_int) * CHAR_BIT) - 1));
  if (a == MIN)
    compilerrt_abort();
  return -a;
}
//===-- negvti2.c - Implement __negvti2 -----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements __negvti2 for the compiler_rt library.
//
//===----------------------------------------------------------------------===//


#ifdef CRT_HAS_128BIT

// Returns: -a

// Effects: aborts if -a overflows

COMPILER_RT_ABI ti_int __negvti2(ti_int a) {
  const ti_int MIN = (tu_int)1 << ((int)(sizeof(ti_int) * CHAR_BIT) - 1);
  if (a == MIN)
    compilerrt_abort();
  return -a;
}

#endif // CRT_HAS_128BIT
//===-- paritydi2.c - Implement __paritydi2 -------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements __paritydi2 for the compiler_rt library.
//
//===----------------------------------------------------------------------===//


// Returns: 1 if number of bits is odd else returns 0

COMPILER_RT_ABI int __paritydi2(di_int a) {
  dwords x;
  x.all = a;
  su_int x2 = x.s.high ^ x.s.low;
  x2 ^= x2 >> 16;
  x2 ^= x2 >> 8;
  x2 ^= x2 >> 4;
  return (0x6996 >> (x2 & 0xF)) & 1;
}
//===-- paritysi2.c - Implement __paritysi2 -------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements __paritysi2 for the compiler_rt library.
//
//===----------------------------------------------------------------------===//


// Returns: 1 if number of bits is odd else returns 0

COMPILER_RT_ABI int __paritysi2(si_int a) {
  su_int x = (su_int)a;
  x ^= x >> 16;
  x ^= x >> 8;
  x ^= x >> 4;
  return (0x6996 >> (x & 0xF)) & 1;
}
//===-- parityti2.c - Implement __parityti2 -------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements __parityti2 for the compiler_rt library.
//
//===----------------------------------------------------------------------===//


#ifdef CRT_HAS_128BIT

// Returns: 1 if number of bits is odd else returns 0

COMPILER_RT_ABI int __parityti2(ti_int a) {
  twords x;
  dwords x2;
  x.all = a;
  x2.all = x.s.high ^ x.s.low;
  su_int x3 = x2.s.high ^ x2.s.low;
  x3 ^= x3 >> 16;
  x3 ^= x3 >> 8;
  x3 ^= x3 >> 4;
  return (0x6996 >> (x3 & 0xF)) & 1;
}

#endif // CRT_HAS_128BIT
//===-- popcountdi2.c - Implement __popcountdi2 ---------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements __popcountdi2 for the compiler_rt library.
//
//===----------------------------------------------------------------------===//


// Returns: count of 1 bits

COMPILER_RT_ABI int __popcountdi2(di_int a) {
  du_int x2 = (du_int)a;
  x2 = x2 - ((x2 >> 1) & 0x5555555555555555uLL);
  // Every 2 bits holds the sum of every pair of bits (32)
  x2 = ((x2 >> 2) & 0x3333333333333333uLL) + (x2 & 0x3333333333333333uLL);
  // Every 4 bits holds the sum of every 4-set of bits (3 significant bits) (16)
  x2 = (x2 + (x2 >> 4)) & 0x0F0F0F0F0F0F0F0FuLL;
  // Every 8 bits holds the sum of every 8-set of bits (4 significant bits) (8)
  su_int x = (su_int)(x2 + (x2 >> 32));
  // The lower 32 bits hold four 16 bit sums (5 significant bits).
  //   Upper 32 bits are garbage
  x = x + (x >> 16);
  // The lower 16 bits hold two 32 bit sums (6 significant bits).
  //   Upper 16 bits are garbage
  return (x + (x >> 8)) & 0x0000007F; // (7 significant bits)
}
//===-- popcountsi2.c - Implement __popcountsi2 ---------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements __popcountsi2 for the compiler_rt library.
//
//===----------------------------------------------------------------------===//


// Returns: count of 1 bits

COMPILER_RT_ABI int __popcountsi2(si_int a) {
  su_int x = (su_int)a;
  x = x - ((x >> 1) & 0x55555555);
  // Every 2 bits holds the sum of every pair of bits
  x = ((x >> 2) & 0x33333333) + (x & 0x33333333);
  // Every 4 bits holds the sum of every 4-set of bits (3 significant bits)
  x = (x + (x >> 4)) & 0x0F0F0F0F;
  // Every 8 bits holds the sum of every 8-set of bits (4 significant bits)
  x = (x + (x >> 16));
  // The lower 16 bits hold two 8 bit sums (5 significant bits).
  //    Upper 16 bits are garbage
  return (x + (x >> 8)) & 0x0000003F; // (6 significant bits)
}
//===-- popcountti2.c - Implement __popcountti2
//----------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements __popcountti2 for the compiler_rt library.
//
//===----------------------------------------------------------------------===//


#ifdef CRT_HAS_128BIT

// Returns: count of 1 bits

COMPILER_RT_ABI int __popcountti2(ti_int a) {
  tu_int x3 = (tu_int)a;
  x3 = x3 - ((x3 >> 1) &
             (((tu_int)0x5555555555555555uLL << 64) | 0x5555555555555555uLL));
  // Every 2 bits holds the sum of every pair of bits (64)
  x3 = ((x3 >> 2) &
        (((tu_int)0x3333333333333333uLL << 64) | 0x3333333333333333uLL)) +
       (x3 & (((tu_int)0x3333333333333333uLL << 64) | 0x3333333333333333uLL));
  // Every 4 bits holds the sum of every 4-set of bits (3 significant bits) (32)
  x3 = (x3 + (x3 >> 4)) &
       (((tu_int)0x0F0F0F0F0F0F0F0FuLL << 64) | 0x0F0F0F0F0F0F0F0FuLL);
  // Every 8 bits holds the sum of every 8-set of bits (4 significant bits) (16)
  du_int x2 = (du_int)(x3 + (x3 >> 64));
  // Every 8 bits holds the sum of every 8-set of bits (5 significant bits) (8)
  su_int x = (su_int)(x2 + (x2 >> 32));
  // Every 8 bits holds the sum of every 8-set of bits (6 significant bits) (4)
  x = x + (x >> 16);
  // Every 8 bits holds the sum of every 8-set of bits (7 significant bits) (2)
  //
  // Upper 16 bits are garbage
  return (x + (x >> 8)) & 0xFF; // (8 significant bits)
}

#endif // CRT_HAS_128BIT
//===-- subvdi3.c - Implement __subvdi3 -----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements __subvdi3 for the compiler_rt library.
//
//===----------------------------------------------------------------------===//


// Returns: a - b

// Effects: aborts if a - b overflows

COMPILER_RT_ABI di_int __subvdi3(di_int a, di_int b) {
  di_int s = (du_int)a - (du_int)b;
  if (b >= 0) {
    if (s > a)
      compilerrt_abort();
  } else {
    if (s <= a)
      compilerrt_abort();
  }
  return s;
}
//===-- subvsi3.c - Implement __subvsi3 -----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements __subvsi3 for the compiler_rt library.
//
//===----------------------------------------------------------------------===//


// Returns: a - b

// Effects: aborts if a - b overflows

COMPILER_RT_ABI si_int __subvsi3(si_int a, si_int b) {
  si_int s = (su_int)a - (su_int)b;
  if (b >= 0) {
    if (s > a)
      compilerrt_abort();
  } else {
    if (s <= a)
      compilerrt_abort();
  }
  return s;
}
//===-- subvti3.c - Implement __subvti3 -----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements __subvti3 for the compiler_rt library.
//
//===----------------------------------------------------------------------===//


#ifdef CRT_HAS_128BIT

// Returns: a - b

// Effects: aborts if a - b overflows

COMPILER_RT_ABI ti_int __subvti3(ti_int a, ti_int b) {
  ti_int s = (tu_int)a - (tu_int)b;
  if (b >= 0) {
    if (s > a)
      compilerrt_abort();
  } else {
    if (s <= a)
      compilerrt_abort();
  }
  return s;
}

#endif // CRT_HAS_128BIT
//===-- ucmpdi2.c - Implement __ucmpdi2 -----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements __ucmpdi2 for the compiler_rt library.
//
//===----------------------------------------------------------------------===//


// Returns:  if (a <  b) returns 0
//           if (a == b) returns 1
//           if (a >  b) returns 2

COMPILER_RT_ABI si_int __ucmpdi2(du_int a, du_int b) {
  udwords x;
  x.all = a;
  udwords y;
  y.all = b;
  if (x.s.high < y.s.high)
    return 0;
  if (x.s.high > y.s.high)
    return 2;
  if (x.s.low < y.s.low)
    return 0;
  if (x.s.low > y.s.low)
    return 2;
  return 1;
}

#ifdef __ARM_EABI__
// Returns: if (a <  b) returns -1
//           if (a == b) returns  0
//           if (a >  b) returns  1
COMPILER_RT_ABI si_int __aeabi_ulcmp(di_int a, di_int b) {
  return __ucmpdi2(a, b) - 1;
}
#endif
//===-- ucmpti2.c - Implement __ucmpti2 -----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements __ucmpti2 for the compiler_rt library.
//
//===----------------------------------------------------------------------===//


#ifdef CRT_HAS_128BIT

// Returns:  if (a <  b) returns 0
//           if (a == b) returns 1
//           if (a >  b) returns 2

COMPILER_RT_ABI si_int __ucmpti2(tu_int a, tu_int b) {
  utwords x;
  x.all = a;
  utwords y;
  y.all = b;
  if (x.s.high < y.s.high)
    return 0;
  if (x.s.high > y.s.high)
    return 2;
  if (x.s.low < y.s.low)
    return 0;
  if (x.s.low > y.s.low)
    return 2;
  return 1;
}

#endif // CRT_HAS_128BIT
//===-- udivdi3.c - Implement __udivdi3 -----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// 2024/08/10 - Modified by mintsuki for use inside cc-runtime
//
//===----------------------------------------------------------------------===//
//
// This file implements __udivdi3 for the compiler_rt library.
//
//===----------------------------------------------------------------------===//


#define fixuint_t du_int
#define fixint_t di_int
//===-- int_div_impl.inc - Integer division ---------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// 2024/10/20 - Modified by mintsuki for use inside cc-runtime
//
//===----------------------------------------------------------------------===//
//
// Helpers used by __udivsi3, __umodsi3, __udivdi3, and __umodsi3.
//
//===----------------------------------------------------------------------===//

#define clz(a) (sizeof(a) == sizeof(unsigned long long) ? __builtin_clzll(a) : clzsi(a))

#undef __udivXi3
#define __udivXi3 __udivXi3_27

// Adapted from Figure 3-40 of The PowerPC Compiler Writer's Guide
static __inline fixuint_t __udivXi3(fixuint_t n, fixuint_t d) {
  const unsigned N = sizeof(fixuint_t) * CHAR_BIT;
  // d == 0 cases are unspecified.
  unsigned sr = (d ? clz(d) : N) - (n ? clz(n) : N);
  // 0 <= sr <= N - 1 or sr is very large.
  if (sr > N - 1) // n < d
    return 0;
  if (sr == N - 1) // d == 1
    return n;
  ++sr;
  // 1 <= sr <= N - 1. Shifts do not trigger UB.
  fixuint_t r = n >> sr;
  n <<= N - sr;
  fixuint_t carry = 0;
  for (; sr > 0; --sr) {
    r = (r << 1) | (n >> (N - 1));
    n = (n << 1) | carry;
    // Branch-less version of:
    // carry = 0;
    // if (r >= d) r -= d, carry = 1;
    const fixint_t s = (fixint_t)(d - r - 1) >> (N - 1);
    carry = s & 1;
    r -= d & s;
  }
  n = (n << 1) | carry;
  return n;
}

#undef __umodXi3
#define __umodXi3 __umodXi3_28

// Mostly identical to __udivXi3 but the return values are different.
static __inline fixuint_t __umodXi3(fixuint_t n, fixuint_t d) {
  const unsigned N = sizeof(fixuint_t) * CHAR_BIT;
  // d == 0 cases are unspecified.
  unsigned sr = (d ? clz(d) : N) - (n ? clz(n) : N);
  // 0 <= sr <= N - 1 or sr is very large.
  if (sr > N - 1) // n < d
    return n;
  if (sr == N - 1) // d == 1
    return 0;
  ++sr;
  // 1 <= sr <= N - 1. Shifts do not trigger UB.
  fixuint_t r = n >> sr;
  n <<= N - sr;
  fixuint_t carry = 0;
  for (; sr > 0; --sr) {
    r = (r << 1) | (n >> (N - 1));
    n = (n << 1) | carry;
    // Branch-less version of:
    // carry = 0;
    // if (r >= d) r -= d, carry = 1;
    const fixint_t s = (fixint_t)(d - r - 1) >> (N - 1);
    carry = s & 1;
    r -= d & s;
  }
  return r;
}

#undef __divXi3

#ifdef COMPUTE_UDIV

#define __divXi3 __divXi3_29

static __inline fixint_t __divXi3(fixint_t a, fixint_t b) {
  const int N = (int)(sizeof(fixint_t) * CHAR_BIT) - 1;
  fixint_t s_a = a >> N;                            // s_a = a < 0 ? -1 : 0
  fixint_t s_b = b >> N;                            // s_b = b < 0 ? -1 : 0
  fixuint_t a_u = (fixuint_t)(a ^ s_a) + (-s_a);    // negate if s_a == -1
  fixuint_t b_u = (fixuint_t)(b ^ s_b) + (-s_b);    // negate if s_b == -1
  s_a ^= s_b;                                       // sign of quotient
  return (COMPUTE_UDIV(a_u, b_u) ^ s_a) + (-s_a);   // negate if s_a == -1
}
#endif // COMPUTE_UDIV

#undef __modXi3

#ifdef ASSIGN_UMOD

#define __modXi3 __modXi3_30

static __inline fixint_t __modXi3(fixint_t a, fixint_t b) {
  const int N = (int)(sizeof(fixint_t) * CHAR_BIT) - 1;
  fixint_t s = b >> N;                              // s = b < 0 ? -1 : 0
  fixuint_t b_u = (fixuint_t)(b ^ s) + (-s);        // negate if s == -1
  s = a >> N;                                       // s = a < 0 ? -1 : 0
  fixuint_t a_u = (fixuint_t)(a ^ s) + (-s);        // negate if s == -1
  fixuint_t res;
  ASSIGN_UMOD(res, a_u, b_u);
  return (res ^ s) + (-s);                          // negate if s == -1
}
#endif // ASSIGN_UMOD

#undef clz

// Returns: a / b

COMPILER_RT_ABI du_int __udivdi3(du_int a, du_int b) {
  return __udivXi3(a, b);
}

#undef fixuint_t
#undef fixint_t
//===-- udivmoddi4.c - Implement __udivmoddi4 -----------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// 2024/01/21 - Modified by mintsuki for use inside cc-runtime
//
//===----------------------------------------------------------------------===//
//
// This file implements __udivmoddi4 for the compiler_rt library.
//
//===----------------------------------------------------------------------===//


// Effects: if rem != 0, *rem = a % b
// Returns: a / b

// Translated from Figure 3-40 of The PowerPC Compiler Writer's Guide

COMPILER_RT_ABI du_int __udivmoddi4(du_int a, du_int b, du_int *rem) {
  const unsigned n_uword_bits = sizeof(su_int) * CHAR_BIT;
  const unsigned n_udword_bits = sizeof(du_int) * CHAR_BIT;
  udwords n;
  n.all = a;
  udwords d;
  d.all = b;
  udwords q;
  udwords r;
  unsigned sr;
  // special cases, X is unknown, K != 0
  if (n.s.high == 0) {
    if (d.s.high == 0) {
      // 0 X
      // ---
      // 0 X
      if (rem)
        *rem = n.s.low % d.s.low;
      return n.s.low / d.s.low;
    }
    // 0 X
    // ---
    // K X
    if (rem)
      *rem = n.s.low;
    return 0;
  }
  // n.s.high != 0
  if (d.s.low == 0) {
    if (d.s.high == 0) {
      // K X
      // ---
      // 0 0
      if (rem)
        *rem = n.s.high % d.s.low;
      return n.s.high / d.s.low;
    }
    // d.s.high != 0
    if (n.s.low == 0) {
      // K 0
      // ---
      // K 0
      if (rem) {
        r.s.high = n.s.high % d.s.high;
        r.s.low = 0;
        *rem = r.all;
      }
      return n.s.high / d.s.high;
    }
    // K K
    // ---
    // K 0
    if ((d.s.high & (d.s.high - 1)) == 0) /* if d is a power of 2 */ {
      if (rem) {
        r.s.low = n.s.low;
        r.s.high = n.s.high & (d.s.high - 1);
        *rem = r.all;
      }
      return n.s.high >> ctzsi(d.s.high);
    }
    // K K
    // ---
    // K 0
    sr = clzsi(d.s.high) - clzsi(n.s.high);
    // 0 <= sr <= n_uword_bits - 2 or sr large
    if (sr > n_uword_bits - 2) {
      if (rem)
        *rem = n.all;
      return 0;
    }
    ++sr;
    // 1 <= sr <= n_uword_bits - 1
    // q.all = n.all << (n_udword_bits - sr);
    q.s.low = 0;
    q.s.high = n.s.low << (n_uword_bits - sr);
    // r.all = n.all >> sr;
    r.s.high = n.s.high >> sr;
    r.s.low = (n.s.high << (n_uword_bits - sr)) | (n.s.low >> sr);
  } else /* d.s.low != 0 */ {
    if (d.s.high == 0) {
      // K X
      // ---
      // 0 K
      if ((d.s.low & (d.s.low - 1)) == 0) /* if d is a power of 2 */ {
        if (rem)
          *rem = n.s.low & (d.s.low - 1);
        if (d.s.low == 1)
          return n.all;
        sr = ctzsi(d.s.low);
        q.s.high = n.s.high >> sr;
        q.s.low = (n.s.high << (n_uword_bits - sr)) | (n.s.low >> sr);
        return q.all;
      }
      // K X
      // ---
      // 0 K
      sr = 1 + n_uword_bits + clzsi(d.s.low) - clzsi(n.s.high);
      // 2 <= sr <= n_udword_bits - 1
      // q.all = n.all << (n_udword_bits - sr);
      // r.all = n.all >> sr;
      if (sr == n_uword_bits) {
        q.s.low = 0;
        q.s.high = n.s.low;
        r.s.high = 0;
        r.s.low = n.s.high;
      } else if (sr < n_uword_bits) /* 2 <= sr <= n_uword_bits - 1 */ {
        q.s.low = 0;
        q.s.high = n.s.low << (n_uword_bits - sr);
        r.s.high = n.s.high >> sr;
        r.s.low = (n.s.high << (n_uword_bits - sr)) | (n.s.low >> sr);
      } else /* n_uword_bits + 1 <= sr <= n_udword_bits - 1 */ {
        q.s.low = n.s.low << (n_udword_bits - sr);
        q.s.high = (n.s.high << (n_udword_bits - sr)) |
                   (n.s.low >> (sr - n_uword_bits));
        r.s.high = 0;
        r.s.low = n.s.high >> (sr - n_uword_bits);
      }
    } else {
      // K X
      // ---
      // K K
      sr = clzsi(d.s.high) - clzsi(n.s.high);
      // 0 <= sr <= n_uword_bits - 1 or sr large
      if (sr > n_uword_bits - 1) {
        if (rem)
          *rem = n.all;
        return 0;
      }
      ++sr;
      // 1 <= sr <= n_uword_bits
      // q.all = n.all << (n_udword_bits - sr);
      q.s.low = 0;
      if (sr == n_uword_bits) {
        q.s.high = n.s.low;
        r.s.high = 0;
        r.s.low = n.s.high;
      } else {
        q.s.high = n.s.low << (n_uword_bits - sr);
        r.s.high = n.s.high >> sr;
        r.s.low = (n.s.high << (n_uword_bits - sr)) | (n.s.low >> sr);
      }
    }
  }
  // Not a special case
  // q and r are initialized with:
  // q.all = n.all << (n_udword_bits - sr);
  // r.all = n.all >> sr;
  // 1 <= sr <= n_udword_bits - 1
  su_int carry = 0;
  for (; sr > 0; --sr) {
    // r:q = ((r:q)  << 1) | carry
    r.s.high = (r.s.high << 1) | (r.s.low >> (n_uword_bits - 1));
    r.s.low = (r.s.low << 1) | (q.s.high >> (n_uword_bits - 1));
    q.s.high = (q.s.high << 1) | (q.s.low >> (n_uword_bits - 1));
    q.s.low = (q.s.low << 1) | carry;
    // carry = 0;
    // if (r.all >= d.all)
    // {
    //      r.all -= d.all;
    //      carry = 1;
    // }
    const di_int s = (di_int)(d.all - r.all - 1) >> (n_udword_bits - 1);
    carry = s & 1;
    r.all -= d.all & s;
  }
  q.all = (q.all << 1) | carry;
  if (rem)
    *rem = r.all;
  return q.all;
}
//===-- udivmodsi4.c - Implement __udivmodsi4 -----------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements __udivmodsi4 for the compiler_rt library.
//
//===----------------------------------------------------------------------===//


// Returns: a / b, *rem = a % b

COMPILER_RT_ABI su_int __udivmodsi4(su_int a, su_int b, su_int *rem) {
  si_int d = __udivsi3(a, b);
  *rem = a - (d * b);
  return d;
}
//===-- udivmodti4.c - Implement __udivmodti4 -----------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements __udivmodti4 for the compiler_rt library.
//
//===----------------------------------------------------------------------===//


#ifdef CRT_HAS_128BIT

// Returns the 128 bit division result by 64 bit. Result must fit in 64 bits.
// Remainder stored in r.
// Taken and adjusted from libdivide libdivide_128_div_64_to_64 division
// fallback. For a correctness proof see the reference for this algorithm
// in Knuth, Volume 2, section 4.3.1, Algorithm D.
UNUSED
static inline du_int udiv128by64to64default(du_int u1, du_int u0, du_int v,
                                            du_int *r) {
  const unsigned n_udword_bits = sizeof(du_int) * CHAR_BIT;
  const du_int b = (1ULL << (n_udword_bits / 2)); // Number base (32 bits)
  du_int un1, un0;                                // Norm. dividend LSD's
  du_int vn1, vn0;                                // Norm. divisor digits
  du_int q1, q0;                                  // Quotient digits
  du_int un64, un21, un10;                        // Dividend digit pairs
  du_int rhat;                                    // A remainder
  si_int s;                                       // Shift amount for normalization

  s = __builtin_clzll(v);
  if (s > 0) {
    // Normalize the divisor.
    v = v << s;
    un64 = (u1 << s) | (u0 >> (n_udword_bits - s));
    un10 = u0 << s; // Shift dividend left
  } else {
    // Avoid undefined behavior of (u0 >> 64).
    un64 = u1;
    un10 = u0;
  }

  // Break divisor up into two 32-bit digits.
  vn1 = v >> (n_udword_bits / 2);
  vn0 = v & 0xFFFFFFFF;

  // Break right half of dividend into two digits.
  un1 = un10 >> (n_udword_bits / 2);
  un0 = un10 & 0xFFFFFFFF;

  // Compute the first quotient digit, q1.
  q1 = un64 / vn1;
  rhat = un64 - q1 * vn1;

  // q1 has at most error 2. No more than 2 iterations.
  while (q1 >= b || q1 * vn0 > b * rhat + un1) {
    q1 = q1 - 1;
    rhat = rhat + vn1;
    if (rhat >= b)
      break;
  }

  un21 = un64 * b + un1 - q1 * v;

  // Compute the second quotient digit.
  q0 = un21 / vn1;
  rhat = un21 - q0 * vn1;

  // q0 has at most error 2. No more than 2 iterations.
  while (q0 >= b || q0 * vn0 > b * rhat + un0) {
    q0 = q0 - 1;
    rhat = rhat + vn1;
    if (rhat >= b)
      break;
  }

  *r = (un21 * b + un0 - q0 * v) >> s;
  return q1 * b + q0;
}

static inline du_int udiv128by64to64(du_int u1, du_int u0, du_int v,
                                     du_int *r) {
#if defined(__x86_64__)
  du_int result;
  __asm__("divq %[v]"
          : "=a"(result), "=d"(*r)
          : [ v ] "r"(v), "a"(u0), "d"(u1));
  return result;
#else
  return udiv128by64to64default(u1, u0, v, r);
#endif
}

// Effects: if rem != 0, *rem = a % b
// Returns: a / b

COMPILER_RT_ABI tu_int __udivmodti4(tu_int a, tu_int b, tu_int *rem) {
  const unsigned n_utword_bits = sizeof(tu_int) * CHAR_BIT;
  utwords dividend;
  dividend.all = a;
  utwords divisor;
  divisor.all = b;
  utwords quotient;
  utwords remainder;
  if (divisor.all > dividend.all) {
    if (rem)
      *rem = dividend.all;
    return 0;
  }
  // When the divisor fits in 64 bits, we can use an optimized path.
  if (divisor.s.high == 0) {
    remainder.s.high = 0;
    if (dividend.s.high < divisor.s.low) {
      // The result fits in 64 bits.
      quotient.s.low = udiv128by64to64(dividend.s.high, dividend.s.low,
                                       divisor.s.low, &remainder.s.low);
      quotient.s.high = 0;
    } else {
      // First, divide with the high part to get the remainder in dividend.s.high.
      // After that dividend.s.high < divisor.s.low.
      quotient.s.high = dividend.s.high / divisor.s.low;
      dividend.s.high = dividend.s.high % divisor.s.low;
      quotient.s.low = udiv128by64to64(dividend.s.high, dividend.s.low,
                                       divisor.s.low, &remainder.s.low);
    }
    if (rem)
      *rem = remainder.all;
    return quotient.all;
  }
  // 0 <= shift <= 63.
  si_int shift =
      __builtin_clzll(divisor.s.high) - __builtin_clzll(dividend.s.high);
  divisor.all <<= shift;
  quotient.s.high = 0;
  quotient.s.low = 0;
  for (; shift >= 0; --shift) {
    quotient.s.low <<= 1;
    // Branch free version of.
    // if (dividend.all >= divisor.all)
    // {
    //    dividend.all -= divisor.all;
    //    carry = 1;
    // }
    const ti_int s =
        (ti_int)(divisor.all - dividend.all - 1) >> (n_utword_bits - 1);
    quotient.s.low |= s & 1;
    dividend.all -= divisor.all & s;
    divisor.all >>= 1;
  }
  if (rem)
    *rem = dividend.all;
  return quotient.all;
}

#endif // CRT_HAS_128BIT
//===-- udivsi3.c - Implement __udivsi3 -----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// 2024/08/10 - Modified by mintsuki for use inside cc-runtime
//
//===----------------------------------------------------------------------===//
//
// This file implements __udivsi3 for the compiler_rt library.
//
//===----------------------------------------------------------------------===//


#define fixuint_t su_int
#define fixint_t si_int
//===-- int_div_impl.inc - Integer division ---------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// 2024/10/20 - Modified by mintsuki for use inside cc-runtime
//
//===----------------------------------------------------------------------===//
//
// Helpers used by __udivsi3, __umodsi3, __udivdi3, and __umodsi3.
//
//===----------------------------------------------------------------------===//

#define clz(a) (sizeof(a) == sizeof(unsigned long long) ? __builtin_clzll(a) : clzsi(a))

#undef __udivXi3
#define __udivXi3 __udivXi3_31

// Adapted from Figure 3-40 of The PowerPC Compiler Writer's Guide
static __inline fixuint_t __udivXi3(fixuint_t n, fixuint_t d) {
  const unsigned N = sizeof(fixuint_t) * CHAR_BIT;
  // d == 0 cases are unspecified.
  unsigned sr = (d ? clz(d) : N) - (n ? clz(n) : N);
  // 0 <= sr <= N - 1 or sr is very large.
  if (sr > N - 1) // n < d
    return 0;
  if (sr == N - 1) // d == 1
    return n;
  ++sr;
  // 1 <= sr <= N - 1. Shifts do not trigger UB.
  fixuint_t r = n >> sr;
  n <<= N - sr;
  fixuint_t carry = 0;
  for (; sr > 0; --sr) {
    r = (r << 1) | (n >> (N - 1));
    n = (n << 1) | carry;
    // Branch-less version of:
    // carry = 0;
    // if (r >= d) r -= d, carry = 1;
    const fixint_t s = (fixint_t)(d - r - 1) >> (N - 1);
    carry = s & 1;
    r -= d & s;
  }
  n = (n << 1) | carry;
  return n;
}

#undef __umodXi3
#define __umodXi3 __umodXi3_32

// Mostly identical to __udivXi3 but the return values are different.
static __inline fixuint_t __umodXi3(fixuint_t n, fixuint_t d) {
  const unsigned N = sizeof(fixuint_t) * CHAR_BIT;
  // d == 0 cases are unspecified.
  unsigned sr = (d ? clz(d) : N) - (n ? clz(n) : N);
  // 0 <= sr <= N - 1 or sr is very large.
  if (sr > N - 1) // n < d
    return n;
  if (sr == N - 1) // d == 1
    return 0;
  ++sr;
  // 1 <= sr <= N - 1. Shifts do not trigger UB.
  fixuint_t r = n >> sr;
  n <<= N - sr;
  fixuint_t carry = 0;
  for (; sr > 0; --sr) {
    r = (r << 1) | (n >> (N - 1));
    n = (n << 1) | carry;
    // Branch-less version of:
    // carry = 0;
    // if (r >= d) r -= d, carry = 1;
    const fixint_t s = (fixint_t)(d - r - 1) >> (N - 1);
    carry = s & 1;
    r -= d & s;
  }
  return r;
}

#undef __divXi3

#ifdef COMPUTE_UDIV

#define __divXi3 __divXi3_33

static __inline fixint_t __divXi3(fixint_t a, fixint_t b) {
  const int N = (int)(sizeof(fixint_t) * CHAR_BIT) - 1;
  fixint_t s_a = a >> N;                            // s_a = a < 0 ? -1 : 0
  fixint_t s_b = b >> N;                            // s_b = b < 0 ? -1 : 0
  fixuint_t a_u = (fixuint_t)(a ^ s_a) + (-s_a);    // negate if s_a == -1
  fixuint_t b_u = (fixuint_t)(b ^ s_b) + (-s_b);    // negate if s_b == -1
  s_a ^= s_b;                                       // sign of quotient
  return (COMPUTE_UDIV(a_u, b_u) ^ s_a) + (-s_a);   // negate if s_a == -1
}
#endif // COMPUTE_UDIV

#undef __modXi3

#ifdef ASSIGN_UMOD

#define __modXi3 __modXi3_34

static __inline fixint_t __modXi3(fixint_t a, fixint_t b) {
  const int N = (int)(sizeof(fixint_t) * CHAR_BIT) - 1;
  fixint_t s = b >> N;                              // s = b < 0 ? -1 : 0
  fixuint_t b_u = (fixuint_t)(b ^ s) + (-s);        // negate if s == -1
  s = a >> N;                                       // s = a < 0 ? -1 : 0
  fixuint_t a_u = (fixuint_t)(a ^ s) + (-s);        // negate if s == -1
  fixuint_t res;
  ASSIGN_UMOD(res, a_u, b_u);
  return (res ^ s) + (-s);                          // negate if s == -1
}
#endif // ASSIGN_UMOD

#undef clz

// Returns: a / b

COMPILER_RT_ABI su_int __udivsi3(su_int a, su_int b) {
  return __udivXi3(a, b);
}

#if defined(__ARM_EABI__)
COMPILER_RT_ALIAS(__udivsi3, __aeabi_uidiv)
#endif

#undef fixuint_t
#undef fixint_t
//===-- udivti3.c - Implement __udivti3 -----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements __udivti3 for the compiler_rt library.
//
//===----------------------------------------------------------------------===//


#ifdef CRT_HAS_128BIT

// Returns: a / b

COMPILER_RT_ABI tu_int __udivti3(tu_int a, tu_int b) {
  return __udivmodti4(a, b, 0);
}

#endif // CRT_HAS_128BIT
//===-- umoddi3.c - Implement __umoddi3 -----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// 2024/08/10 - Modified by mintsuki for use inside cc-runtime
//
//===----------------------------------------------------------------------===//
//
// This file implements __umoddi3 for the compiler_rt library.
//
//===----------------------------------------------------------------------===//


#define fixuint_t du_int
#define fixint_t di_int
//===-- int_div_impl.inc - Integer division ---------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// 2024/10/20 - Modified by mintsuki for use inside cc-runtime
//
//===----------------------------------------------------------------------===//
//
// Helpers used by __udivsi3, __umodsi3, __udivdi3, and __umodsi3.
//
//===----------------------------------------------------------------------===//

#define clz(a) (sizeof(a) == sizeof(unsigned long long) ? __builtin_clzll(a) : clzsi(a))

#undef __udivXi3
#define __udivXi3 __udivXi3_35

// Adapted from Figure 3-40 of The PowerPC Compiler Writer's Guide
static __inline fixuint_t __udivXi3(fixuint_t n, fixuint_t d) {
  const unsigned N = sizeof(fixuint_t) * CHAR_BIT;
  // d == 0 cases are unspecified.
  unsigned sr = (d ? clz(d) : N) - (n ? clz(n) : N);
  // 0 <= sr <= N - 1 or sr is very large.
  if (sr > N - 1) // n < d
    return 0;
  if (sr == N - 1) // d == 1
    return n;
  ++sr;
  // 1 <= sr <= N - 1. Shifts do not trigger UB.
  fixuint_t r = n >> sr;
  n <<= N - sr;
  fixuint_t carry = 0;
  for (; sr > 0; --sr) {
    r = (r << 1) | (n >> (N - 1));
    n = (n << 1) | carry;
    // Branch-less version of:
    // carry = 0;
    // if (r >= d) r -= d, carry = 1;
    const fixint_t s = (fixint_t)(d - r - 1) >> (N - 1);
    carry = s & 1;
    r -= d & s;
  }
  n = (n << 1) | carry;
  return n;
}

#undef __umodXi3
#define __umodXi3 __umodXi3_36

// Mostly identical to __udivXi3 but the return values are different.
static __inline fixuint_t __umodXi3(fixuint_t n, fixuint_t d) {
  const unsigned N = sizeof(fixuint_t) * CHAR_BIT;
  // d == 0 cases are unspecified.
  unsigned sr = (d ? clz(d) : N) - (n ? clz(n) : N);
  // 0 <= sr <= N - 1 or sr is very large.
  if (sr > N - 1) // n < d
    return n;
  if (sr == N - 1) // d == 1
    return 0;
  ++sr;
  // 1 <= sr <= N - 1. Shifts do not trigger UB.
  fixuint_t r = n >> sr;
  n <<= N - sr;
  fixuint_t carry = 0;
  for (; sr > 0; --sr) {
    r = (r << 1) | (n >> (N - 1));
    n = (n << 1) | carry;
    // Branch-less version of:
    // carry = 0;
    // if (r >= d) r -= d, carry = 1;
    const fixint_t s = (fixint_t)(d - r - 1) >> (N - 1);
    carry = s & 1;
    r -= d & s;
  }
  return r;
}

#undef __divXi3

#ifdef COMPUTE_UDIV

#define __divXi3 __divXi3_37

static __inline fixint_t __divXi3(fixint_t a, fixint_t b) {
  const int N = (int)(sizeof(fixint_t) * CHAR_BIT) - 1;
  fixint_t s_a = a >> N;                            // s_a = a < 0 ? -1 : 0
  fixint_t s_b = b >> N;                            // s_b = b < 0 ? -1 : 0
  fixuint_t a_u = (fixuint_t)(a ^ s_a) + (-s_a);    // negate if s_a == -1
  fixuint_t b_u = (fixuint_t)(b ^ s_b) + (-s_b);    // negate if s_b == -1
  s_a ^= s_b;                                       // sign of quotient
  return (COMPUTE_UDIV(a_u, b_u) ^ s_a) + (-s_a);   // negate if s_a == -1
}
#endif // COMPUTE_UDIV

#undef __modXi3

#ifdef ASSIGN_UMOD

#define __modXi3 __modXi3_38

static __inline fixint_t __modXi3(fixint_t a, fixint_t b) {
  const int N = (int)(sizeof(fixint_t) * CHAR_BIT) - 1;
  fixint_t s = b >> N;                              // s = b < 0 ? -1 : 0
  fixuint_t b_u = (fixuint_t)(b ^ s) + (-s);        // negate if s == -1
  s = a >> N;                                       // s = a < 0 ? -1 : 0
  fixuint_t a_u = (fixuint_t)(a ^ s) + (-s);        // negate if s == -1
  fixuint_t res;
  ASSIGN_UMOD(res, a_u, b_u);
  return (res ^ s) + (-s);                          // negate if s == -1
}
#endif // ASSIGN_UMOD

#undef clz

// Returns: a % b

COMPILER_RT_ABI du_int __umoddi3(du_int a, du_int b) {
  return __umodXi3(a, b);
}

#undef fixuint_t
#undef fixint_t
//===-- umodsi3.c - Implement __umodsi3 -----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// 2024/08/10 - Modified by mintsuki for use inside cc-runtime
//
//===----------------------------------------------------------------------===//
//
// This file implements __umodsi3 for the compiler_rt library.
//
//===----------------------------------------------------------------------===//


#define fixuint_t su_int
#define fixint_t si_int
//===-- int_div_impl.inc - Integer division ---------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// 2024/10/20 - Modified by mintsuki for use inside cc-runtime
//
//===----------------------------------------------------------------------===//
//
// Helpers used by __udivsi3, __umodsi3, __udivdi3, and __umodsi3.
//
//===----------------------------------------------------------------------===//

#define clz(a) (sizeof(a) == sizeof(unsigned long long) ? __builtin_clzll(a) : clzsi(a))

#undef __udivXi3
#define __udivXi3 __udivXi3_39

// Adapted from Figure 3-40 of The PowerPC Compiler Writer's Guide
static __inline fixuint_t __udivXi3(fixuint_t n, fixuint_t d) {
  const unsigned N = sizeof(fixuint_t) * CHAR_BIT;
  // d == 0 cases are unspecified.
  unsigned sr = (d ? clz(d) : N) - (n ? clz(n) : N);
  // 0 <= sr <= N - 1 or sr is very large.
  if (sr > N - 1) // n < d
    return 0;
  if (sr == N - 1) // d == 1
    return n;
  ++sr;
  // 1 <= sr <= N - 1. Shifts do not trigger UB.
  fixuint_t r = n >> sr;
  n <<= N - sr;
  fixuint_t carry = 0;
  for (; sr > 0; --sr) {
    r = (r << 1) | (n >> (N - 1));
    n = (n << 1) | carry;
    // Branch-less version of:
    // carry = 0;
    // if (r >= d) r -= d, carry = 1;
    const fixint_t s = (fixint_t)(d - r - 1) >> (N - 1);
    carry = s & 1;
    r -= d & s;
  }
  n = (n << 1) | carry;
  return n;
}

#undef __umodXi3
#define __umodXi3 __umodXi3_40

// Mostly identical to __udivXi3 but the return values are different.
static __inline fixuint_t __umodXi3(fixuint_t n, fixuint_t d) {
  const unsigned N = sizeof(fixuint_t) * CHAR_BIT;
  // d == 0 cases are unspecified.
  unsigned sr = (d ? clz(d) : N) - (n ? clz(n) : N);
  // 0 <= sr <= N - 1 or sr is very large.
  if (sr > N - 1) // n < d
    return n;
  if (sr == N - 1) // d == 1
    return 0;
  ++sr;
  // 1 <= sr <= N - 1. Shifts do not trigger UB.
  fixuint_t r = n >> sr;
  n <<= N - sr;
  fixuint_t carry = 0;
  for (; sr > 0; --sr) {
    r = (r << 1) | (n >> (N - 1));
    n = (n << 1) | carry;
    // Branch-less version of:
    // carry = 0;
    // if (r >= d) r -= d, carry = 1;
    const fixint_t s = (fixint_t)(d - r - 1) >> (N - 1);
    carry = s & 1;
    r -= d & s;
  }
  return r;
}

#undef __divXi3

#ifdef COMPUTE_UDIV

#define __divXi3 __divXi3_41

static __inline fixint_t __divXi3(fixint_t a, fixint_t b) {
  const int N = (int)(sizeof(fixint_t) * CHAR_BIT) - 1;
  fixint_t s_a = a >> N;                            // s_a = a < 0 ? -1 : 0
  fixint_t s_b = b >> N;                            // s_b = b < 0 ? -1 : 0
  fixuint_t a_u = (fixuint_t)(a ^ s_a) + (-s_a);    // negate if s_a == -1
  fixuint_t b_u = (fixuint_t)(b ^ s_b) + (-s_b);    // negate if s_b == -1
  s_a ^= s_b;                                       // sign of quotient
  return (COMPUTE_UDIV(a_u, b_u) ^ s_a) + (-s_a);   // negate if s_a == -1
}
#endif // COMPUTE_UDIV

#undef __modXi3

#ifdef ASSIGN_UMOD

#define __modXi3 __modXi3_42

static __inline fixint_t __modXi3(fixint_t a, fixint_t b) {
  const int N = (int)(sizeof(fixint_t) * CHAR_BIT) - 1;
  fixint_t s = b >> N;                              // s = b < 0 ? -1 : 0
  fixuint_t b_u = (fixuint_t)(b ^ s) + (-s);        // negate if s == -1
  s = a >> N;                                       // s = a < 0 ? -1 : 0
  fixuint_t a_u = (fixuint_t)(a ^ s) + (-s);        // negate if s == -1
  fixuint_t res;
  ASSIGN_UMOD(res, a_u, b_u);
  return (res ^ s) + (-s);                          // negate if s == -1
}
#endif // ASSIGN_UMOD

#undef clz

// Returns: a % b

COMPILER_RT_ABI su_int __umodsi3(su_int a, su_int b) {
  return __umodXi3(a, b);
}

#undef fixuint_t
#undef fixint_t
//===-- umodti3.c - Implement __umodti3 -----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements __umodti3 for the compiler_rt library.
//
//===----------------------------------------------------------------------===//


#ifdef CRT_HAS_128BIT

// Returns: a % b

COMPILER_RT_ABI tu_int __umodti3(tu_int a, tu_int b) {
  tu_int r;
  __udivmodti4(a, b, &r);
  return r;
}

#endif // CRT_HAS_128BIT