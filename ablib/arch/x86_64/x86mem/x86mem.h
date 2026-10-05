// Minimum requirement:
//  x86_64 CPU with SSE4.2, but AVX2 or later is *highly* recommended
//
// This file provides function prototypes for AVX_memmove, AVX_memcpy, AVX_memset, and AVX_memcmp
//
// NOTE: If you need to move/copy memory between overlapping regions, use AVX_memmove instead of AVX_memcpy.
// AVX_memcpy does contain a redirect to AVX_memmove if an overlapping region is found, but it is disabled by default
// since it adds extra latency that really can be avoided by using AVX_memmove directly.
//

#ifndef _avxmem_H
#define _avxmem_H

#ifdef __x86_64__
#ifndef __KERNEL_INC__
#include <stddef.h>
#include <stdint.h>

#include <nmmintrin.h> 

#if defined(__AVX__) 
#include "./avxintrin.h"
#endif
#if defined(__AVX2__) 
#include "./avx2intrin.h"
#endif

#if defined(__AVX512F__)
#include "./avx512fintrin.h"
#endif
#endif

/* ---------------------------------------------------------------------------
 * 非时序 (NT/streaming) 存储阈值
 *
 * 搬运尺寸超过这个值时改用 streaming store, 避免把工作集从 cache 里冲掉。
 * 原本是编译期常量 3MB; 现改为运行时变量, 由启动代码用 CPUID 探测到的
 * L3 容量填进去:
 *   - 用户态: lib/base/arch/x86_64/init.c      _init_runtime_and_global_variables()
 *   - 内核态: kernel/src/arch/x86_64/init.cpp  x86_64_init()
 *
 * mem* 只拿它挑路径: 取错值只影响性能, 不影响正确性。探测不到 (返回 0) 或
 * 启动代码没跑到之前, 都保持 X86MEM_CACHE_LIMIT_DEFAULT。
 * GLIBC ~ LIMIT
 * ------------------------------------------------------------------------ */
#define X86MEM_CACHE_LIMIT_DEFAULT ((size_t)3  * 1024 * 1024)   /*  3 MB */
#define X86MEM_CACHE_LIMIT_MIN     ((size_t)1 * 1024 * 1024)    /* 1 MB */
#define X86MEM_CACHE_LIMIT_MAX     ((size_t)32 * 1024 * 1024)  /* 32 MB */
#ifndef X86MEM_CACHE_LIMIT_L3_DIV
#define X86MEM_CACHE_LIMIT_L3_DIV  8u
#endif

#ifdef __cplusplus
extern "C"{
#endif

/* 这三个符号必须在 extern "C" 里: kernel 是 C++, 否则会被 mangling 掉。 */

/* NT 阈值, 单位字节。只被 mem* 读, 用来挑"普通存储 / streaming 存储"。 */
extern size_t x86mem_cache_limit;
/* CPUID 探测 L3 容量; 探测不到返回 0 (调用方应保持默认值) */
size_t x86mem_detect_l3_size(void);
/* 探测并把结果写进 x86mem_cache_limit (内部会夹到 [MIN, MAX]) */
void   x86mem_init_cache_limit(void);


//-----------------------------------------------------------------------------
// Main Functions:
//-----------------------------------------------------------------------------

// Calling the individual subfunctions directly is also OK. That's why this header is so huge!

#ifdef __AVX512F__
#define x86memlib_UseFunction(x) x##V3
#define x86memlib_DeclFunction(x) x##V3
#elif defined(__AVX2__)
#define x86memlib_UseFunction(x) x##V2
#define x86memlib_DeclFunction(x) x##V2
#elif defined(__AVX__)
#define x86memlib_UseFunction(x) x##V1
#define x86memlib_DeclFunction(x) x##V1
#else
#define x86memlib_UseFunction(x) x##V0
#define x86memlib_DeclFunction(x) x##V0
#endif

extern __attribute__((weak)) uint8_t MEMOPS_SupportV3;
extern __attribute__((weak)) uint8_t MEMOPS_SupportV2;
extern __attribute__((weak)) uint8_t MEMOPS_SupportV1;

void * AVX_memmoveV3(void *dest, void *src, size_t numbytes);
void * AVX_memcpyV3(void *dest, void *src, size_t numbytes);
void * AVX_memsetV3(void *dest, const uint8_t val, size_t numbytes);
int AVX_memcmpV3(const void *str1, const void *str2, size_t numbytes, int equality);

void * AVX_memmoveV2(void *dest, void *src, size_t numbytes);
void * AVX_memcpyV2(void *dest, void *src, size_t numbytes);
void * AVX_memsetV2(void *dest, const uint8_t val, size_t numbytes);
int AVX_memcmpV2(const void *str1, const void *str2, size_t numbytes, int equality);

void * AVX_memmoveV1(void *dest, void *src, size_t numbytes);
void * AVX_memcpyV1(void *dest, void *src, size_t numbytes);
void * AVX_memsetV1(void *dest, const uint8_t val, size_t numbytes);
int AVX_memcmpV1(const void *str1, const void *str2, size_t numbytes, int equality);

void * AVX_memmoveV0(void *dest, void *src, size_t numbytes);
void * AVX_memcpyV0(void *dest, void *src, size_t numbytes);
void * AVX_memsetV0(void *dest, const uint8_t val, size_t numbytes);
int AVX_memcmpV0(const void *str1, const void *str2, size_t numbytes, int equality);
/* 
void * AVX_memmove(void *dest, void *src, size_t numbytes);
void * AVX_memcpy(void *dest, void *src, size_t numbytes);
void * AVX_memset(void *dest, const uint8_t val, size_t numbytes);
int AVX_memcmp(const void *str1, const void *str2, size_t numbytes, int equality); */

/* int memcmp_fpx86 (const void *str1, const void *str2, size_t count);
void * memset_fpx86 (void *dest, const uint8_t val, size_t len);
void * memmove_fpx86 (void *dest, const void *src, size_t len);
void * memcpy_fpx86 (void *dest, const void *src, size_t len); */
/* 
// Numbytes_div_4 is total number of bytes / 4 (since they only do 4 at a time).
void * AVX_memset_4B(void *dest, const uint32_t val, size_t numbytes_div_4);

//-----------------------------------------------------------------------------
// MEMSET:
//-----------------------------------------------------------------------------

void * memset_large(void *dest, const uint8_t val, size_t numbytes);
void * memset_large_a(void *dest, const uint8_t val, size_t numbytes);
void * memset_large_as(void *dest, const uint8_t val, size_t numbytes);

void * memset_zeroes(void *dest, size_t numbytes);
void * memset_zeroes_a(void *dest, size_t numbytes);
void * memset_zeroes_as(void *dest, size_t numbytes);

void * memset_large_4B(void *dest, const uint32_t val, size_t numbytes_div_4);
void * memset_large_4B_a(void *dest, const uint32_t val, size_t numbytes_div_4);
void * memset_large_4B_as(void *dest, const uint32_t val, size_t numbytes_div_4);

// Scalar
void * memset_fpx86 (void *dest, uint8_t val, size_t len); // 1 byte
void * memset_16bit(void *dest, uint16_t val, size_t len); // 2 bytes
void * memset_32bit(void *dest, uint32_t val, size_t len); // 4 bytes
void * memset_64bit(void *dest, uint64_t val, size_t len); // 8 bytes

// SSE2 (Unaligned)
void * memset_128bit_u(void *dest, __m128i_u val, size_t len); // 16 bytes
void * memset_128bit_32B_u(void *dest, __m128i_u val, size_t len); // 32 bytes
void * memset_128bit_64B_u(void *dest, __m128i_u val, size_t len); // 64 bytes
void * memset_128bit_128B_u(void *dest, __m128i_u val, size_t len); // 128 bytes
void * memset_128bit_256B_u(void *dest, __m128i_u val, size_t len); // 256 bytes

// SSE2 (Aligned)
void * memset_128bit_a(void *dest, __m128i val, size_t len); // 16 bytes
void * memset_128bit_32B_a(void *dest, __m128i_u val, size_t len); // 32 bytes
void * memset_128bit_64B_a(void *dest, __m128i_u val, size_t len); // 64 bytes
void * memset_128bit_128B_a(void *dest, __m128i_u val, size_t len); // 128 bytes
void * memset_128bit_256B_a(void *dest, __m128i_u val, size_t len); // 256 bytes

//SSE2 (Aligned, streaming)
void * memset_128bit_as(void *dest, __m128i val, size_t len); // 16 bytes
void * memset_128bit_32B_as(void *dest, __m128i_u val, size_t len); // 32 bytes
void * memset_128bit_64B_as(void *dest, __m128i_u val, size_t len); // 64 bytes
void * memset_128bit_128B_as(void *dest, __m128i_u val, size_t len); // 128 bytes
void * memset_128bit_256B_as(void *dest, __m128i_u val, size_t len); // 256 bytes

// AVX
#ifdef __AVX__
// Unaligned
void * memset_256bit_u(void *dest, __m256i_u val, size_t len); // 32 bytes
void * memset_256bit_64B_u(void *dest, __m256i_u val, size_t len); // 64 bytes
void * memset_256bit_128B_u(void *dest, __m256i_u val, size_t len); // 128 bytes
void * memset_256bit_256B_u(void *dest, __m256i_u val, size_t len); // 256 bytes
void * memset_256bit_512B_u(void *dest, __m256i_u val, size_t len); // 512 bytes

// Aligned
void * memset_256bit_a(void *dest, __m256i_u val, size_t len); // 32 bytes
void * memset_256bit_64B_a(void *dest, __m256i_u val, size_t len); // 64 bytes
void * memset_256bit_128B_a(void *dest, __m256i_u val, size_t len); // 128 bytes
void * memset_256bit_256B_a(void *dest, __m256i_u val, size_t len); // 256 bytes
void * memset_256bit_512B_a(void *dest, __m256i_u val, size_t len); // 512 bytes

// Aligned, Streaming
void * memset_256bit_as(void *dest, __m256i_u val, size_t len); // 32 bytes
void * memset_256bit_64B_as(void *dest, __m256i_u val, size_t len); // 64 bytes
void * memset_256bit_128B_as(void *dest, __m256i_u val, size_t len); // 128 bytes
void * memset_256bit_256B_as(void *dest, __m256i_u val, size_t len); // 256 bytes
void * memset_256bit_512B_as(void *dest, __m256i_u val, size_t len); // 512 bytes
#endif

// AVX512
#ifdef __AVX512F__
// Unaligned
void * memset_512bit_u(void *dest, __m512i_u val, size_t len); // 64 bytes
void * memset_512bit_128B_u(void *dest, __m512i_u val, size_t len); // 128 bytes
void * memset_512bit_256B_u(void *dest, __m512i_u val, size_t len); // 256 bytes
void * memset_512bit_512B_u(void *dest, __m512i_u val, size_t len); // 512 bytes
void * memset_512bit_1kB_u(void *dest, __m512i_u val, size_t len); // 1024 bytes
void * memset_512bit_2kB_u(void *dest, __m512i_u val, size_t len); // 2048 bytes
void * memset_512bit_4kB_u(void *dest, __m512i_u val, size_t len); // 4096 bytes
// Yes that's a whole page. AVX-512 maxes at 2 kB stored at one time in its registers, though.

// Aligned
void * memset_512bit_a(void *dest, __m512i_u val, size_t len); // 64 bytes
void * memset_512bit_128B_a(void *dest, __m512i_u val, size_t len); // 128 bytes
void * memset_512bit_256B_a(void *dest, __m512i_u val, size_t len); // 256 bytes
void * memset_512bit_512B_a(void *dest, __m512i_u val, size_t len); // 512 bytes
void * memset_512bit_1kB_a(void *dest, __m512i_u val, size_t len); // 1024 bytes
void * memset_512bit_2kB_a(void *dest, __m512i_u val, size_t len); // 2048 bytes
void * memset_512bit_4kB_a(void *dest, __m512i_u val, size_t len); // 4096 bytes

// Aligned, Streaming
void * memset_512bit_as(void *dest, __m512i_u val, size_t len); // 64 bytes
void * memset_512bit_128B_as(void *dest, __m512i_u val, size_t len); // 128 bytes
void * memset_512bit_256B_as(void *dest, __m512i_u val, size_t len); // 256 bytes
void * memset_512bit_512B_as(void *dest, __m512i_u val, size_t len); // 512 bytes
void * memset_512bit_1kB_as(void *dest, __m512i_u val, size_t len); // 1024 bytes
void * memset_512bit_2kB_as(void *dest, __m512i_u val, size_t len); // 2048 bytes
void * memset_512bit_4kB_as(void *dest, __m512i_u val, size_t len); // 4096 bytes
#endif
// END MEMSET

//-----------------------------------------------------------------------------
// MEMMOVE:
//-----------------------------------------------------------------------------

//
// The following also applies to memcpy:
//
// Len: Can be thought of as number of times to run the loop in each function
// (i.e. the quantity of that function's # of bytes, like 512 bytes for the 512B
// ones. Giving memmove_512bit_512B a Len of 4 means "move 2 kB.")
// numbytes: Total number of bytes
//
// _a functions require source & destination addresses to be aligned according to
// their x-bit in the function name. E.g. memmove_256bit_64B_a needs to be 32-byte
// aligned (256/8 = 32). The functions will crash/raise an exception otherwise.
//

void * memmove_large(void *dest, void *src, size_t numbytes);
void * memmove_large_a(void *dest, void *src, size_t numbytes);
void * memmove_large_as(void *dest, void *src, size_t numbytes);

void * memmove_large_reverse(void *dest, void *src, size_t numbytes);
void * memmove_large_reverse_a(void *dest, void *src, size_t numbytes);
void * memmove_large_reverse_as(void *dest, void *src, size_t numbytes);

// Scalar
void * memmove(void *dest, const void *src, size_t len); // 1 byte
void * memmove_16bit(void *dest, const void *src, size_t len); // 2 bytes
void * memmove_32bit(void *dest, const void *src, size_t len); // 4 bytes
void * memmove_64bit(void *dest, const void *src, size_t len); // 8 bytes

// SSE2 (Unaligned)
void * memmove_128bit_u(void *dest, const void *src, size_t len); // 16 bytes
void * memmove_128bit_32B_u(void *dest, const void *src, size_t len); // 32 bytes
void * memmove_128bit_64B_u(void *dest, const void *src, size_t len); // 64 bytes
void * memmove_128bit_128B_u(void *dest, const void *src, size_t len); // 128 bytes
void * memmove_128bit_256B_u(void *dest, const void *src, size_t len); // 256 bytes

// SSE2 (Aligned)
void * memmove_128bit_a(void *dest, const void *src, size_t len); // 16 bytes
void * memmove_128bit_32B_a(void *dest, const void *src, size_t len); // 32 bytes
void * memmove_128bit_64B_a(void *dest, const void *src, size_t len); // 64 bytes
void * memmove_128bit_128B_a(void *dest, const void *src, size_t len); // 128 bytes
void * memmove_128bit_256B_a(void *dest, const void *src, size_t len); // 256 bytes

// SSE4.1 (Aligned, Streaming)
void * memmove_128bit_as(void *dest, const void *src, size_t len); // 16 bytes
void * memmove_128bit_32B_as(void *dest, const void *src, size_t len); // 32 bytes
void * memmove_128bit_64B_as(void *dest, const void *src, size_t len); // 64 bytes
void * memmove_128bit_128B_as(void *dest, const void *src, size_t len); // 128 bytes
void * memmove_128bit_256B_as(void *dest, const void *src, size_t len); // 256 bytes


// AVX
#ifdef __AVX__
// Unaligned
void * memmove_256bit_u(void *dest, const void *src, size_t len); // 32 bytes
void * memmove_256bit_64B_u(void *dest, const void *src, size_t len); // 64 bytes
void * memmove_256bit_128B_u(void *dest, const void *src, size_t len); // 128 bytes
void * memmove_256bit_256B_u(void *dest, const void *src, size_t len); // 256 bytes
void * memmove_256bit_512B_u(void *dest, const void *src, size_t len); // 512 bytes

// Aligned
void * memmove_256bit_a(void *dest, const void *src, size_t len); // 32 bytes
void * memmove_256bit_64B_a(void *dest, const void *src, size_t len); // 64 bytes
void * memmove_256bit_128B_a(void *dest, const void *src, size_t len); // 128 bytes
void * memmove_256bit_256B_a(void *dest, const void *src, size_t len); // 256 bytes
void * memmove_256bit_512B_a(void *dest, const void *src, size_t len); // 512 bytes

// Aligned, Streaming
#ifdef __AVX2__
void * memmove_256bit_as(void *dest, const void *src, size_t len); // 32 bytes
void * memmove_256bit_64B_as(void *dest, const void *src, size_t len); // 64 bytes
void * memmove_256bit_128B_as(void *dest, const void *src, size_t len); // 128 bytes
void * memmove_256bit_256B_as(void *dest, const void *src, size_t len); // 256 bytes
void * memmove_256bit_512B_as(void *dest, const void *src, size_t len); // 512 bytes
#endif
#endif

// AVX512
#ifdef __AVX512F__
// Unaligned
void * memmove_512bit_u(void *dest, const void *src, size_t len); // 64 bytes
void * memmove_512bit_128B_u(void *dest, const void *src, size_t len); // 128 bytes
void * memmove_512bit_256B_u(void *dest, const void *src, size_t len); // 256 bytes
void * memmove_512bit_512B_u(void *dest, const void *src, size_t len); // 512 bytes
void * memmove_512bit_1kB_u(void *dest, const void *src, size_t len); // 1024 bytes
void * memmove_512bit_2kB_u(void *dest, const void *src, size_t len); // 2048 bytes
void * memmove_512bit_4kB_u(void *dest, const void *src, size_t len); // 4096 bytes
// Yes that's a whole page. AVX-512 maxes at 2 kB stored at one time in its registers, though.

// Aligned
void * memmove_512bit_a(void *dest, const void *src, size_t len); // 64 bytes
void * memmove_512bit_128B_a(void *dest, const void *src, size_t len); // 128 bytes
void * memmove_512bit_256B_a(void *dest, const void *src, size_t len); // 256 bytes
void * memmove_512bit_512B_a(void *dest, const void *src, size_t len); // 512 bytes
void * memmove_512bit_1kB_a(void *dest, const void *src, size_t len); // 1024 bytes
void * memmove_512bit_2kB_a(void *dest, const void *src, size_t len); // 2048 bytes
void * memmove_512bit_4kB_a(void *dest, const void *src, size_t len); // 4096 bytes

// Aligned, Streaming
void * memmove_512bit_as(void *dest, const void *src, size_t len); // 64 bytes
void * memmove_512bit_128B_as(void *dest, const void *src, size_t len); // 128 bytes
void * memmove_512bit_256B_as(void *dest, const void *src, size_t len); // 256 bytes
void * memmove_512bit_512B_as(void *dest, const void *src, size_t len); // 512 bytes
void * memmove_512bit_1kB_as(void *dest, const void *src, size_t len); // 1024 bytes
void * memmove_512bit_2kB_as(void *dest, const void *src, size_t len); // 2048 bytes
void * memmove_512bit_4kB_as(void *dest, const void *src, size_t len); // 4096 bytes
#endif
// END MEMMOVE

//-----------------------------------------------------------------------------
// MEMCPY:
//-----------------------------------------------------------------------------

void * memcpy_large(void *dest, void *src, size_t numbytes);
void * memcpy_large_a(void *dest, void *src, size_t numbytes);
void * memcpy_large_as(void *dest, void *src, size_t numbytes);

// Scalar
void * memcpy(void *dest, const void *src, size_t len); // 1 byte
void * memcpy_16bit(void *dest, const void *src, size_t len); // 2 bytes
void * memcpy_32bit(void *dest, const void *src, size_t len); // 4 bytes
void * memcpy_64bit(void *dest, const void *src, size_t len); // 8 bytes

// SSE2 (Unaligned)
void * memcpy_128bit_u(void *dest, const void *src, size_t len); // 16 bytes
void * memcpy_128bit_32B_u(void *dest, const void *src, size_t len); // 32 bytes
void * memcpy_128bit_64B_u(void *dest, const void *src, size_t len); // 64 bytes
void * memcpy_128bit_128B_u(void *dest, const void *src, size_t len); // 128 bytes
void * memcpy_128bit_256B_u(void *dest, const void *src, size_t len); // 256 bytes

// SSE2 (aligned)
void * memcpy_128bit_a(void *dest, const void *src, size_t len); // 16 bytes
void * memcpy_128bit_32B_a(void *dest, const void *src, size_t len); // 32 bytes
void * memcpy_128bit_64B_a(void *dest, const void *src, size_t len); // 64 bytes
void * memcpy_128bit_128B_a(void *dest, const void *src, size_t len); // 128 bytes
void * memcpy_128bit_256B_a(void *dest, const void *src, size_t len); // 256 bytes

// SSE4.1 (Aligned, Streaming)
void * memcpy_128bit_as(void *dest, const void *src, size_t len); // 16 bytes
void * memcpy_128bit_32B_as(void *dest, const void *src, size_t len); // 32 bytes
void * memcpy_128bit_64B_as(void *dest, const void *src, size_t len); // 64 bytes
void * memcpy_128bit_128B_as(void *dest, const void *src, size_t len); // 128 bytes
void * memcpy_128bit_256B_as(void *dest, const void *src, size_t len); // 256 bytes

// AVX
#ifdef __AVX__
// Unaligned
void * memcpy_256bit_u(void *dest, const void *src, size_t len); // 32 bytes
void * memcpy_256bit_64B_u(void *dest, const void *src, size_t len); // 64 bytes
void * memcpy_256bit_128B_u(void *dest, const void *src, size_t len); // 128 bytes
void * memcpy_256bit_256B_u(void *dest, const void *src, size_t len); // 256 bytes
void * memcpy_256bit_512B_u(void *dest, const void *src, size_t len); // 512 bytes

// Aligned
void * memcpy_256bit_a(void *dest, const void *src, size_t len); // 32 bytes
void * memcpy_256bit_64B_a(void *dest, const void *src, size_t len); // 64 bytes
void * memcpy_256bit_128B_a(void *dest, const void *src, size_t len); // 128 bytes
void * memcpy_256bit_256B_a(void *dest, const void *src, size_t len); // 256 bytes
void * memcpy_256bit_512B_a(void *dest, const void *src, size_t len); // 512 bytes

// Aligned, Streaming
#ifdef __AVX2__
void * memcpy_256bit_as(void *dest, const void *src, size_t len); // 32 bytes
void * memcpy_256bit_64B_as(void *dest, const void *src, size_t len); // 64 bytes
void * memcpy_256bit_128B_as(void *dest, const void *src, size_t len); // 128 bytes
void * memcpy_256bit_256B_as(void *dest, const void *src, size_t len); // 256 bytes
void * memcpy_256bit_512B_as(void *dest, const void *src, size_t len); // 512 bytes
#endif
#endif

// AVX512
#ifdef __AVX512F__
// Unaligned
void * memcpy_512bit_u(void *dest, const void *src, size_t len); // 64 bytes
void * memcpy_512bit_128B_u(void *dest, const void *src, size_t len); // 128 bytes
void * memcpy_512bit_256B_u(void *dest, const void *src, size_t len); // 256 bytes
void * memcpy_512bit_512B_u(void *dest, const void *src, size_t len); // 512 bytes
void * memcpy_512bit_1kB_u(void *dest, const void *src, size_t len); // 1024 bytes
void * memcpy_512bit_2kB_u(void *dest, const void *src, size_t len); // 2048 bytes
void * memcpy_512bit_4kB_u(void *dest, const void *src, size_t len); // 4096 bytes
// Yes that's a whole page. AVX-512 maxes at 2 kB stored at one time in its registers, though.

// Aligned
void * memcpy_512bit_a(void *dest, const void *src, size_t len); // 64 bytes
void * memcpy_512bit_128B_a(void *dest, const void *src, size_t len); // 128 bytes
void * memcpy_512bit_256B_a(void *dest, const void *src, size_t len); // 256 bytes
void * memcpy_512bit_512B_a(void *dest, const void *src, size_t len); // 512 bytes
void * memcpy_512bit_1kB_a(void *dest, const void *src, size_t len); // 1024 bytes
void * memcpy_512bit_2kB_a(void *dest, const void *src, size_t len); // 2048 bytes
void * memcpy_512bit_4kB_a(void *dest, const void *src, size_t len); // 4096 bytes

// Aligned, Streaming
void * memcpy_512bit_as(void *dest, const void *src, size_t len); // 64 bytes
void * memcpy_512bit_128B_as(void *dest, const void *src, size_t len); // 128 bytes
void * memcpy_512bit_256B_as(void *dest, const void *src, size_t len); // 256 bytes
void * memcpy_512bit_512B_as(void *dest, const void *src, size_t len); // 512 bytes
void * memcpy_512bit_1kB_as(void *dest, const void *src, size_t len); // 1024 bytes
void * memcpy_512bit_2kB_as(void *dest, const void *src, size_t len); // 2048 bytes
void * memcpy_512bit_4kB_as(void *dest, const void *src, size_t len); // 4096 bytes
#endif
// END MEMCPY

//-----------------------------------------------------------------------------
// MEMCMP:
//-----------------------------------------------------------------------------

int memcmp_large(const void *str1, const void *str2, size_t numbytes);
int memcmp_large_eq(const void *str1, const void *str2, size_t numbytes);

int memcmp_large_a(const void *str1, const void *str2, size_t numbytes);
int memcmp_large_eq_a(const void *str1, const void *str2, size_t numbytes);

// Scalar
int memcmp_16bit(const void *str1, const void *str2, size_t count);
int memcmp_16bit_eq(const void *str1, const void *str2, size_t count);
int memcmp_32bit(const void *str1, const void *str2, size_t count);
int memcmp_32bit_eq(const void *str1, const void *str2, size_t count);
int memcmp_64bit(const void *str1, const void *str2, size_t count);
int memcmp_64bit_eq(const void *str1, const void *str2, size_t count);

// SSE4.2 (Unaligned)
int memcmp_128bit_u(const void *str1, const void *str2, size_t count);
int memcmp_128bit_eq_u(const void *str1, const void *str2, size_t count);

// SSE4.2 (Aligned)
int memcmp_128bit_a(const void *str1, const void *str2, size_t count);
int memcmp_128bit_eq_a(const void *str1, const void *str2, size_t count);

// AVX2
#ifdef __AVX2__
// Unaligned
int memcmp_256bit_u(const void *str1, const void *str2, size_t count);
int memcmp_256bit_eq_u(const void *str1, const void *str2, size_t count);

// Aligned
int memcmp_256bit_a(const void *str1, const void *str2, size_t count);
int memcmp_256bit_eq_a(const void *str1, const void *str2, size_t count);
#endif

// AVX512
#ifdef __AVX512F__
// Unaligned
int memcmp_512bit_u(const void *str1, const void *str2, size_t count);
int memcmp_512bit_eq_u(const void *str1, const void *str2, size_t count);

// Aligned
int memcmp_512bit_a(const void *str1, const void *str2, size_t count);
int memcmp_512bit_eq_a(const void *str1, const void *str2, size_t count);
#endif
// END MEMCMP */

#ifdef __cplusplus
}
#endif

#endif

#endif /* _avxmem_H */