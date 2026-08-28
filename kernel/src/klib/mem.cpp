//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
// SPDX-License-Identifier: GPL-2.0-only
#include <klib/klib.h>
#include <klib/kio.h>
#include <conf.h>

#ifdef __x86_64__
#pragma GCC push_options
#endif
#if defined(__x86_64__) && NOT_COMPILE_X86MEM == 0
#define __KERNEL_INC__
#include "../../../ablib/arch/x86_64/x86mem/x86mem.h"
#include <arch/x86_64/smp/smp.h>
#include <arch/x86_64/schedule/sched.h>
#include <klib/serial.h>

#if defined(__x86_64__)
#include <arch/x86_64/smp/smp.h>
#include <arch/x86_64/schedule/sched.h>
#elif defined(__aarch64__)
func_optimize(3) void NEON_MEMCPY(void* dst, const void* src, size_t size);
func_optimize(3) void NEON_MEMSET(void* dst, uint8_t value, size_t size);
#endif
#endif

#ifdef __x86_64__

/* ============================================================
 * XFXSAVE_CAS / XFXSAVE_CASB — 内核态 SIMD 上下文切换
 *
 * 作用域契约: __fx_rflags_save_ 由调用方在函数体声明
 *   (do{}while(0) 块内声明的变量出块即亡, CASB 看不到 —
 *    四个 _mem* 函数的调用模式固定为:
 *      uint64_t __fx_rflags_save_;
 *      XFXSAVE_CAS
 *      ... SIMD 核心 ...
 *      XFXSAVE_CASB )
 *
 * asm volatile("" ::: "memory");
 *     防止编译器把后面的代码重排到前面 (写屏障序)
 * ============================================================ */

#define XFXSAVE_CAS do{\
        asm volatile("pushfq\n\tpopq %0\n\tcli" \
                     : "=r"(__fx_rflags_save_) :: "memory", "cc"); \
        cpu->preempt_count++; \
        asm volatile("" ::: "memory"); \
        if(cpu->SupportXSAVE){ \
            if(fx_area != nullptr){ \
                if(cpu->SupportXSAVEOPT) \
                    asm volatile("xsaveopt %0" : : "m"(*fx_area), "a"(UINT32_MAX), "d"(UINT32_MAX) : "memory");\
                else \
                    asm volatile("xsave %0" : : "m"(*fx_area), "a"(UINT32_MAX), "d"(UINT32_MAX) : "memory"); \
            } \
            asm volatile("xrstor %0" : : "m"(*cpu->KernelXsaveSpace), "a"(UINT32_MAX), "d"(UINT32_MAX) : "memory"); \
        }else{ \
            if(fx_area != nullptr) \
                asm volatile("fxsave (%0)" : : "r"(fx_area) : "memory"); \
            asm volatile("fxrstor (%0)" : : "r"(cpu->KernelXsaveSpace) : "memory"); \
        } \
    }while(0);

#define XFXSAVE_CASB do{\
        asm volatile("sfence" ::: "memory"); \
        if(cpu->SupportXSAVE){ \
            if(cpu->SupportXSAVEOPT) \
                asm volatile("xsaveopt %0" : : "m"(*cpu->KernelXsaveSpace), "a"(UINT32_MAX), "d"(UINT32_MAX) : "memory"); \
            else \
                asm volatile("xsave %0" : : "m"(*cpu->KernelXsaveSpace), "a"(UINT32_MAX), "d"(UINT32_MAX) : "memory"); \
            if(fx_area != nullptr) \
                asm volatile("xrstor %0" : : "m"(*fx_area), "a"(UINT32_MAX), "d"(UINT32_MAX) : "memory"); \
        }else{ \
            asm volatile("fxsave (%0)" : : "r"(cpu->KernelXsaveSpace) : "memory"); \
            if(fx_area != nullptr) \
                asm volatile("fxrstor (%0)" : : "r"(fx_area) : "memory"); \
        } \
        asm volatile("" ::: "memory"); \
        cpu->preempt_count--; \
        /*  恢复进入时的完整 rflags — 之前 IF=0 恢复关,
           之前 IF=1 恢复开。sti 永远不再无条件出现 */ \
        asm volatile("pushq %0\n\tpopfq" :: "r"(__fx_rflags_save_) : "memory", "cc"); \
    }while(0);

#endif

extern "C" {

void _memcpy(void* src, void* dest, uint64_t size){
#if defined(__x86_64__)
#if (defined(COMPILER_SUPPORT_AVX512) || \
    defined(COMPILER_SUPPORT_AVX2) || \
    defined(COMPILER_SUPPORT_SSE_4_2)) \
    && (USE_HOST_CPU_EXTENSIONS == 1) && ((CONFIG_FAST_MEMCPY == 1))
    cpu_t *cpu = this_cpu();
    if(cpu == nullptr || (cpu->InIntr == true))
        goto base_ver;
    if(size >= 16384  && cpu->SupportSSE4_2){
        char *fx_area = Schedule::this_thread()->fx_area;
        if(fx_area == nullptr)
            goto base_ver;
        uint64_t __fx_rflags_save_;          /*  声明挪到函数体级 */
        XFXSAVE_CAS
        cpu->OverLoadableFuncs.MemcpyCore(dest,src,size);
        XFXSAVE_CASB
        return;
    }
#endif
#elif defined(__aarch64__)
    NEON_MEMCPY(dest,src,size);
    return;
#endif
base_ver:
    memcpy_fscpuf(dest,src,size);
}


void _memset(void* dest, uint8_t value, uint64_t size){
#if defined(__x86_64__)
#if (defined(COMPILER_SUPPORT_AVX512) || \
    defined(COMPILER_SUPPORT_AVX2) || \
    defined(COMPILER_SUPPORT_SSE_4_2)) \
    && (USE_HOST_CPU_EXTENSIONS == 1) && ((CONFIG_FAST_MEMSET == 1))
    cpu_t *cpu = this_cpu();
    if(cpu == nullptr || (cpu->InIntr == true))
        goto base_ver;
    if(size >= 16384  && cpu->SupportSSE4_2){

        char *fx_area = Schedule::this_thread()->fx_area;
        if(fx_area == nullptr)
            goto base_ver;
        uint64_t __fx_rflags_save_;          /*  */
        XFXSAVE_CAS
        cpu->OverLoadableFuncs.MemsetCore(dest,value,size);
        XFXSAVE_CASB
        return;
    }
#endif
#elif defined(__aarch64__)
    NEON_MEMSET(dest,value,size);
    return;
#endif
base_ver:
    memset_fscpuf(dest,(int32_t)value,size);
}


void _memmove(void* dest,void* src, uint64_t size) {
#if defined(__x86_64__)
#if (defined(COMPILER_SUPPORT_AVX512) || \
    defined(COMPILER_SUPPORT_AVX2) || \
    defined(COMPILER_SUPPORT_SSE_4_2)) \
    && (USE_HOST_CPU_EXTENSIONS == 1) && ((CONFIG_FAST_MEMMOVE == 1))
    cpu_t *cpu = this_cpu();
    if(cpu == nullptr || (cpu->InIntr == true))
        goto base_ver;
    if(size >= 32768  && cpu->SupportSSE4_2){
        char *fx_area = Schedule::this_thread()->fx_area;
        if(fx_area == nullptr)
            goto base_ver;
        uint64_t __fx_rflags_save_;          /*  */
        XFXSAVE_CAS
        cpu->OverLoadableFuncs.MemmoveCore(dest,src,size);
        XFXSAVE_CASB
        return;
    }
#endif
#endif
base_ver:
    memmove_fscpuf(dest,src,size);
}

int32_t _memcmp(const void* buffer1,const void* buffer2,size_t  size){
#if defined(__x86_64__)
#if (defined(COMPILER_SUPPORT_AVX512) || \
    defined(COMPILER_SUPPORT_AVX2) || \
    defined(COMPILER_SUPPORT_SSE_4_2)) \
    && (USE_HOST_CPU_EXTENSIONS == 1) && ((CONFIG_FAST_MEMCMP == 1))
    cpu_t *cpu = this_cpu();
    if(cpu == nullptr || (cpu->InIntr == true))
        goto base_ver;
    if(size >= 1048576  && cpu->SupportSSE4_2){
        char *fx_area = Schedule::this_thread()->fx_area;
        if(fx_area == nullptr)
            goto base_ver;
        uint64_t __fx_rflags_save_;          /*  */
        XFXSAVE_CAS
        int32_t result = cpu->OverLoadableFuncs.MemcmpCore(buffer1,buffer2,size,1);
        XFXSAVE_CASB
        return result;
    }
#endif
#endif
base_ver:
    return memcmp_fscpuf(buffer1,buffer2,size);
}
}

#ifdef __x86_64__
#pragma GCC pop_options
#endif