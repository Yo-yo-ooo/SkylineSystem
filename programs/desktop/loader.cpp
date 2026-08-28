//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
// SPDX-License-Identifier: MIT
#include <syscall.h>
#include <graphic/fb.h>
#include <string.h>

void TLoad(FrameBuffer *Fb){
    /* Load the target program. Returns its pid. */
    uint64_t pid = sys_load((uint64_t)"/mp/hw2.elf", 0, 0);
    if ((int64_t)pid < 0) return;

    uint64_t dstPID = sys_getpid();

    /* ============ Share the framebuffer region (4MB) ============
       src(hw2) gets a new EAlloc'd block, dst(desktop) gets a
       pass-through mapping of the same physical pages.
       flags = 7: bit0 must be set — VMM_FLAG_READWRITE(2) alone
       produces P=0 ghost PTEs (bit0 = P / MM_READ / PROT_READ,
       same value across all three flag conventions). */
    uint64_t desk_fb_va = sys_pmmapSHARE(dstPID, 0, Fb->BufferSize,
                                          7, pid, 0);

    /*  Sideband latch: rdi = src VA (hw2 side), rsi = dst VA (our side).
       MUST be the very first statement after the syscall — any
       intervening call clobbers these registers. The asm read is
       explicit; a plain "register ... asm("rdi")" binding is only
       a compiler hint and may be moved/spilled.
       Contract: rax < 0  =>  rdi == rsi == 0 (kernel guarantees). */
    uint64_t hw2_fb_va, desk_fb_va_check;
    __asm__ volatile ("movq %%rdi, %0\n\t"
                      "movq %%rsi, %1"
                      : "=r"(hw2_fb_va), "=r"(desk_fb_va_check)
                      :
                      : "memory");

    if ((int64_t)desk_fb_va < 0) return;           /* rax: error check */

    /* ============ Share the target's 4K ELF extension area ============
       High 2K = LIBC DECL (first 8 bytes = buffer ring addr)
       LOW 2K  = GUI/TUI/... decl metadata.
       hw2 always accesses it at its own 0x400000;
       we access it via the returned dst VA. */
    uint64_t _4KELFSection = sys_pmmapSHARE(dstPID, 0, 4096,
                                             7, pid, 0x400000);
    if ((int64_t)_4KELFSection < 0) return;

    /* ============ Write protocol data (TARGET's perspective) ============
       Never memcpy the desktop FrameBuffer struct raw — it holds
       desktop-side pointers (stack/heap) that are wild addresses
       to the target. Only plain values and target-side VAs here.
       Layout:
         proto[0] = framebuffer VA as seen BY THE TARGET (hw2_fb_va)
         proto[1] = framebuffer size in bytes */
    memset((void*)_4KELFSection, 0, 4096);
    volatile uint64_t *proto = (volatile uint64_t*)_4KELFSection;
    proto[0] = hw2_fb_va;          /*  hw2 reads this and paints */
    proto[1] = Fb->BufferSize;
    proto[2] = Fb->Width;
    proto[3] = Fb->Height;
    proto[4] = Fb->PixelsPerScanLine;

    /* ============ Kick off the loaded program ============ */
    uint64_t lr = sys_launch(pid);
    if ((int64_t)lr < 0) return;
}