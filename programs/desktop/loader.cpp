//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT
#include <syscall.h>
#include <graphic/fb.h>
#include <string.h>

void TLoad(FrameBuffer *Fb){
    uint64_t pid = sys_load((uint64_t)"/mp/hw2.elf",0,0);
    uint64_t dstPID = sys_getpid();

    sys_pmmapSHARE(dstPID,0,Fb->BufferSize,VMM_FLAG_READWRITE,pid,0);
    uint64_t WinFbMappingAddr = 0; // For Desktop (Can R/W) Frame Buffer
    uint64_t WinFb_TAddr = 0;      // target process pagemap mapping addr
    asm volatile("movq %%rdi, %0" : "=r"(WinFbMappingAddr) :: "memory");
    asm volatile("movq %%rsi, %0" : "=r"(WinFb_TAddr     ) :: "memory");
    // Write FB thing to target process 
    // so they can see where the frame buffer mapping!
    // and then they can use printf("XXXX") print to Window!
    sys_pmmapSHARE(dstPID,0,4096,VMM_FLAG_READWRITE,pid,0x400000);
    uint64_t _4KELFSection = 0;
    asm volatile("movq %%rsi, %0" : "=r"(_4KELFSection   ) :: "memory");
    // High 2K of this 4K ELF Section IS FOR LIBC DECL( first 8bytes if buffer ring addr)
    // LOW 2K is for our GUI/TUI/..... decl metadata
    //memcpy((void*)_4KELFSection,(void*)Fb,sizeof(FrameBuffer));


    sys_launch(pid); 
}