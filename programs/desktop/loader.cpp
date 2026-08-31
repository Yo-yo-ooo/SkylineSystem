//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT
//
// TLoad: spawn the userspace console process (hw2.elf) and hand it a
// FIXED-SIZE haribote-style window surface to render into, WITHOUT mapping
// the real MMIO scanout into it.
//
//   share 1 (window surface)   src = hw2 (fresh zero pages, SKYWIN_W x H),
//                              dst = desktop (alias VA used to present it)
//     hw2 paints the window chrome and runs its libc flanterm console into
//     its own VA; the desktop mounts the SAME physical pages as one normal,
//     centered, non-fullscreen compositor window.
//
//   share 2 (4 KiB protocol page)  src = hw2 fixed 0x400000 (.prepad),
//                                  dst = desktop alias VA for writing.
//
// The protocol (see graphic/winstyle.h) carries TWO geometries:
//   [0..4] inner text area -> consumed verbatim by the libc console
//   [5..7] whole window    -> used by hw2 to paint border/title/close chrome
//
// The desktop-side alias + centered placement is returned through *place.

#include <stdint.h>
#include <string.h>
#include <syscall.h>
#include <base/arch/x86_64/syscalln.h>
#include <graphic/fb.h>
#include <graphic/winstyle.h>

#define PAGE_SIZE    4096UL
#define SHARE_FLAGS  7UL

static inline uint64_t align_up(uint64_t x, uint64_t a) {
    return (x + (a - 1)) & ~(a - 1);
}

/* Capture the rdi/rsi sideband (resolved src/dst VAs) immediately after the
   syscall returns, before the compiler can reuse those registers. */
static inline void read_sideband(uint64_t *src_va, uint64_t *dst_va) {
    __asm__ volatile("movq %%rdi, %0\n\t"
                     "movq %%rsi, %1"
                     : "=r"(*src_va), "=r"(*dst_va)
                     :: "rdi", "rsi", "memory");
}

uint64_t TLoad(FrameBuffer *Fb, SkyWinPlacement *place) {
    if (!Fb || !Fb->BaseAddress) return 0;

    uint64_t pid = sys_load((uint64_t)"/mp/hw2.elf", 0, 0);
    if ((int64_t)pid < 0) return 0;

    uint64_t self      = sys_getpid();
    uint64_t winBytes  = align_up((uint64_t)SKYWIN_W * SKYWIN_H * sizeof(uint32_t),
                                  PAGE_SIZE);

    /* share 1: hw2 owns the fresh window surface, desktop aliases it. */
    uint64_t r1 = sys_pmmapSHARE(self, 0, winBytes, SHARE_FLAGS,
                                 (uint64_t)pid, 0);
    uint64_t hw2_whole = 0, desk_whole = 0;
    read_sideband(&hw2_whole, &desk_whole);
    if ((int64_t)r1 < 0 || hw2_whole == 0 || desk_whole == 0) return 0;

    /* share 2: hw2 fixed protocol page -> desktop alias for writing. */
    uint64_t r2 = sys_pmmapSHARE(self, 0, PAGE_SIZE, SHARE_FLAGS,
                                 (uint64_t)pid, SKYWIN_PROTO_PAGE_VA);
    uint64_t proto_src = 0, proto = 0;
    read_sideband(&proto_src, &proto);
    (void)proto_src;
    if ((int64_t)r2 < 0 || proto == 0) return 0;

    /* Inner text-area origin inside the tightly-packed window surface. */
    uint64_t content_off =
        ((uint64_t)SKYWIN_CONTENT_Y * SKYWIN_W + SKYWIN_CONTENT_X) * sizeof(uint32_t);

    memset((void*)proto, 0, PAGE_SIZE);
    volatile uint64_t *q = (volatile uint64_t*)proto;
    q[SKYWIN_PROTO_CONTENT_VA]  = hw2_whole + content_off;
    q[SKYWIN_PROTO_CONTENT_SZ]  = (uint64_t)SKYWIN_W * SKYWIN_CONTENT_H * sizeof(uint32_t);
    q[SKYWIN_PROTO_CONTENT_W]   = SKYWIN_CONTENT_W;
    q[SKYWIN_PROTO_CONTENT_H]   = SKYWIN_CONTENT_H;
    q[SKYWIN_PROTO_PITCH]       = SKYWIN_W;                 /* window pitch */
    q[SKYWIN_PROTO_WHOLE_VA]    = hw2_whole;
    q[SKYWIN_PROTO_WIN_W]       = SKYWIN_W;
    q[SKYWIN_PROTO_WIN_H]       = SKYWIN_H;

    if ((int64_t)sys_launch(pid) < 0) return 0;

    /* Block until hw2 finishes its FIRST frame (chrome + initial text). The
       window surface is shared memory that hw2 fills concurrently; if the
       desktop registered + composited it immediately, each horizontal worker
       strip could observe a different fill stage -> torn/garbled window on
       creation. Spin briefly then yield so the fresh client can run; the wait
       is bounded so a crashed client can never wedge the desktop. */
    for (uint64_t w = 0; w < SKYWIN_READY_WAIT_LIMIT; w++) {
        if (__atomic_load_n((uint64_t*)&q[SKYWIN_PROTO_READY],
                            __ATOMIC_ACQUIRE) == SKYWIN_READY_MAGIC)
            break;
        if ((w & 63u) == 63u) sys_yield();
    }

    if (place) {
        place->desk_surf = desk_whole;
        place->w = SKYWIN_W;
        place->h = SKYWIN_H;
        place->x = (Fb->Width  > SKYWIN_W) ? (uint32_t)((Fb->Width  - SKYWIN_W) / 2u) : 0u;
        place->y = (Fb->Height > SKYWIN_H) ? (uint32_t)((Fb->Height - SKYWIN_H) / 2u) : 0u;
    }
    return desk_whole;
}
