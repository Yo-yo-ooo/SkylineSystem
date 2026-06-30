// SPDX-FileCopyrightText: 2026 Yo-yo-ooo
// SPDX-License-Identifier: GPL-2.0-only

#include <klib/klib.h>
#include <stdint.h>
#include <arch/x86_64/cpu.h>
#include <mem/heap.h>
#include <mem/pmm.h>
#include <arch/x86_64/smp/smp.h>
#include <arch/x86_64/simd/simd.h>
#include <arch/x86_64/simd/xsave.h>
#include "../../../../../ablib/arch/x86_64/x86mem/x86mem.h"

uint32_t MaxXsaveSize = 0;

void simd_xsave_init(void)
{
    kinfoln("DO XSAVE INIT");
    
    // 开启 CR4 的 XSAVE 支持
    uint64_t cr4 = cr4_read() | CR4_XSAVE_ENABLE;
    cr4_write(cr4);

    // 清零 XFD，避免触发未处理的 #NM 缺陷
    wrmsr(IA32_MSR_XFD, 0);

    uint64_t rax, rbx, rcx, rdx;
    cpuid(13, 0, &rax, &rbx, &rcx, &rdx);
    
    // 确定硬件支持的 User 特性
    xsave_feat_mask_t hw_supported_user = ((uint64_t)rdx << 32) | rax;
    
    // OS 想要启用的 User 特性 (必须包含 X87 和 SSE)
    xsave_feat_mask_t os_want_user = __XSAVE_FEAT_MASK(XSAVE_FEAT_X87)
                                   | __XSAVE_FEAT_MASK(XSAVE_FEAT_SSE);

    if(cpuid_is_avx_avail()){
        os_want_user |= __XSAVE_FEAT_MASK(XSAVE_FEAT_AVX);
        if(cpuid_is_avx512_avail()){
            os_want_user |= __XSAVE_FEAT_MASK(XSAVE_FEAT_AVX_512_OPMASK) 
                          | __XSAVE_FEAT_MASK(XSAVE_FEAT_AVX_512_ZMM_HI256) 
                          | __XSAVE_FEAT_MASK(XSAVE_FEAT_AVX_512_HI16_ZMM);
        }
    }
    
    // 如果支持 CET，CET_USER 也是 User State
    // os_want_user |= __XSAVE_FEAT_MASK(XSAVE_FEAT_CET_USER);

    // 取交集：硬件支持 且 OS想要启用
    xsave_feat_mask_t xsave_user_features = hw_supported_user & os_want_user;

    // 设置 Supervisor 特性
    // 注意：X87, SSE, CET_USER 绝对不能出现在这里！
    xsave_feat_mask_t xsave_supervisor_features = 0;
    if (hw_supported_user & __XSAVE_FEAT_MASK(XSAVE_FEAT_CET_SUPERVISOR)) {
        xsave_supervisor_features |= __XSAVE_FEAT_MASK(XSAVE_FEAT_CET_SUPERVISOR);
    }
    xsave_set_supervisor_features(xsave_supervisor_features);

    // 写入 XCR0
    kinfoln("Read xcr0");
    const uint64_t xcr = read_xcr(XCR_XSTATE_FEATURES_ENABLED);
    kinfoln("Write to xcr0");
    write_xcr(XCR_XSTATE_FEATURES_ENABLED, xcr | xsave_user_features);
}

static spinlock_t simd_lock = 0;

void simd_cpu_init(cpu_t *cpu)
{
    uint8_t i = 0;
    cr0_write(cr0_read() & ~((uint64_t)CR0_EMULATION));
    cr0_write(cr0_read() | CR0_MONITOR_CO_PROCESSOR | CR0_NUMERIC_ERROR_ENABLE);
    cr4_write(cr4_read() | CR4_FXSR_ENABLE | CR4_SIMD_EXCEPTION);

    if (cpuid_is_xsave_avail())
    {
        simd_xsave_init();
        asm volatile("xgetbv" : "=a"(cpu->XsaveMaskLo), "=d"(cpu->XsaveMaskHi) : "c"(0));
        
        uint32_t eax, ebx, ecx, edx;
        asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0xD), "c"(0));
        
        cpu->XsaveSize = ebx;
        spinlock_lock(&simd_lock);
        if(ebx > MaxXsaveSize) MaxXsaveSize = ebx;
        spinlock_unlock(&simd_lock);
        
        // 分配 XSAVE 区域，VMM::Alloc 通常返回页对齐，满足 64-byte 对齐要求
        cpu->KernelXsaveSpace = (int8_t*)VMM::Alloc(kernel_pagemap, DIV_ROUND_UP(ebx, PAGE_SIZE), false);
        _memset(cpu->KernelXsaveSpace, 0, ebx);
        
        // 将当前 CPU 的干净状态保存下来，作为后续内核态使用的初始状态
        asm volatile("xsave %0" : : "m"(*cpu->KernelXsaveSpace), "a"(cpu->XsaveMaskLo), "d"(cpu->XsaveMaskHi) : "memory");
        
        // 检查 XSAVEOPT 支持
        asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0xD), "c"(1));
        cpu->SupportXSAVEOPT = (eax & (1 << 0));
        if(cpu->SupportXSAVEOPT)
            cpu->OverLoadableFuncs.StoreSIMDState = StoreSIMDState_V2;
        else           
            cpu->OverLoadableFuncs.StoreSIMDState = StoreSIMDState_V1;
        cpu->OverLoadableFuncs.LoadSIMDState = LoadSIMDState_V1;
    } else {
        cpu->OverLoadableFuncs.StoreSIMDState = StoreSIMDState_V0;
        cpu->OverLoadableFuncs.LoadSIMDState = LoadSIMDState_V0;
    }

    // 初始化 FPU 状态
    asm volatile("fninit");
    // 如果支持 AVX，执行 vzeroupper 避免 AVX/SSE 切换惩罚
    if (cpuid_is_avx_avail()) {
        asm volatile("vzeroupper" ::: "memory");
    }

    // --- 打印与函数指针绑定逻辑 (保持原样，非常完美) ---
    kpok("cpu simd:");
    if (cpuid_is_xsave_avail()){ kprintf("xsave ");i++; cpu->SupportXSAVE = true; }
    if (cpuid_is_avx_avail()){
        kprintf("avx ");i++; cpu->SupportAVX = true;
        if((cpu->SupportAVX2 = cpuid_is_avx2_avail())) kprintf("avx2 ");
    }
    if (cpuid_is_avx512_avail()){ kprintf("avx512 ");i++; cpu->SupportAVX512 = true; }
    if(cpuid_is_sse4_2_avail()){ kprintf("sse4.2 ");i++; cpu->SupportSSE4_2 = true; }
    
    kprintf("enabled\n");
    cpu->SupportSIMD = (i > 0);

    if (cpu->SupportAVX512 && MEMOPS_SupportV3) {
        cpu->OverLoadableFuncs.MemcpyCore   = AVX_memcpyV3;
        cpu->OverLoadableFuncs.MemmoveCore  = AVX_memmoveV3;
        cpu->OverLoadableFuncs.MemsetCore   = AVX_memsetV3;
        cpu->OverLoadableFuncs.MemcmpCore   = AVX_memcmpV3;
    } else if (cpu->SupportAVX2 && MEMOPS_SupportV2) {
        cpu->OverLoadableFuncs.MemcpyCore   = AVX_memcpyV2;
        cpu->OverLoadableFuncs.MemmoveCore  = AVX_memmoveV2;
        cpu->OverLoadableFuncs.MemsetCore   = AVX_memsetV2;
        cpu->OverLoadableFuncs.MemcmpCore   = AVX_memcmpV2;
    } else if (cpu->SupportAVX && MEMOPS_SupportV1) {
        cpu->OverLoadableFuncs.MemcpyCore   = AVX_memcpyV1;
        cpu->OverLoadableFuncs.MemmoveCore  = AVX_memmoveV1;
        cpu->OverLoadableFuncs.MemsetCore   = AVX_memsetV1;
        cpu->OverLoadableFuncs.MemcmpCore   = AVX_memcmpV1;
    } else {
        cpu->OverLoadableFuncs.MemcpyCore   = AVX_memcpyV0;
        cpu->OverLoadableFuncs.MemmoveCore  = AVX_memmoveV0;
        cpu->OverLoadableFuncs.MemsetCore   = AVX_memsetV0;
        cpu->OverLoadableFuncs.MemcmpCore   = AVX_memcmpV0;
    }
}


void StoreSIMDState_V0(char* area,uint32_t Lo,uint32_t Hi){
    asm volatile("fxsave (%0)" : : "r"(area) : "memory");
}

void StoreSIMDState_V1(char* area,uint32_t Lo,uint32_t Hi){
    asm volatile("xsave %0" : : "m"(*area), "a"(Lo), "d"(Hi) : "memory");
}

void StoreSIMDState_V2(char* area,uint32_t Lo,uint32_t Hi){
    asm volatile("xsaveopt %0" : : "m"(*area), "a"(Lo), "d"(Hi) : "memory");
}

void LoadSIMDState_V0(char* area,uint32_t Lo,uint32_t Hi){
    asm volatile("fxrstor (%0)" : : "r"(area) : "memory");
}

void LoadSIMDState_V1(char* area,uint32_t Lo,uint32_t Hi){
    asm volatile("xrstor %0" : : "m"(*area), "a"(Lo), "d"(Hi) : "memory");
}

