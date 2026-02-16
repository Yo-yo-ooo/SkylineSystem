/*
* SPDX-License-Identifier: GPL-2.0-only
* File: aarch64mem.S
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
.section .text
.global NEON_MEMCPY
.global NEON_MEMSET

NEON_MEMCPY:
    sub     sp, sp,#384
    str     x0, [sp, 24]
    str     x1, [sp, 16]
    str     x2, [sp, 8]
    ldr     x0, [sp, 24]
    str     x0, [sp, 376]
    ldr     x0, [sp, 16]
    str     x0, [sp, 368]
    ldr     x0, [sp, 8]
    lsr     x0, x0, 6
    str     w0, [sp, 364]
    ldr     w0, [sp, 364]
    lsl     w0, w0, 6
    sxtw    x0, w0
    ldr     x1, [sp, 8]
    sub     x0, x1, x0
    str     x0, [sp, 8]
    b       .L2
.L7:
    ldr     x0, [sp, 368]
    add     x0, x0, 64
    prfm    PLDL1KEEP, [x0]
    ldr     x0, [sp, 368]
    str     x0, [sp, 96]
    ldr     x0, [sp, 96]
    ldr     q31, [x0]
    nop
    str     q31, [sp, 304]
    ldr     x0, [sp, 368]
    add     x0, x0, 16
    str     x0, [sp, 104]
    ldr     x0, [sp, 104]
    ldr     q31, [x0]
    nop
    str     q31, [sp, 288]
    ldr     x0, [sp, 368]
    add     x0, x0, 32
    str     x0, [sp, 112]
    ldr     x0, [sp, 112]
    ldr     q31, [x0]
    nop
    str     q31, [sp, 272]
    ldr     x0, [sp, 368]
    add     x0, x0, 48
    str     x0, [sp, 120]
    ldr     x0, [sp, 120]
    ldr     q31, [x0]
    nop
    str     q31, [sp, 256]
    ldr     x0, [sp, 376]
    str     x0, [sp, 152]
    ldr     q31, [sp, 304]
    str     q31, [sp, 128]
    ldr     q31, [sp, 128]
    ldr     x0, [sp, 152]
    str     q31, [x0]
    nop
    ldr     x0, [sp, 376]
    add     x0, x0, 16
    str     x0, [sp, 184]
    ldr     q31, [sp, 288]
    str     q31, [sp, 160]
    ldr     q31, [sp, 160]
    ldr     x0, [sp, 184]
    str     q31, [x0]
    nop
    ldr     x0, [sp, 376]
    add     x0, x0, 32
    str     x0, [sp, 216]
    ldr     q31, [sp, 272]
    str     q31, [sp, 192]
    ldr     q31, [sp, 192]
    ldr     x0, [sp, 216]
    str     q31, [x0]
    nop
    ldr     x0, [sp, 376]
    add     x0, x0, 48
    str     x0, [sp, 248]
    ldr     q31, [sp, 256]
    str     q31, [sp, 224]
    ldr     q31, [sp, 224]
    ldr     x0, [sp, 248]
    str     q31, [x0]
    nop
    ldr     x0, [sp, 368]
    add     x0, x0, 64
    str     x0, [sp, 368]
    ldr     x0, [sp, 376]
    add     x0, x0, 64
    str     x0, [sp, 376]
.L2:
    ldr     w0, [sp, 364]
    sub     w1, w0, #1
    str     w1, [sp, 364]
    cmp     w0, 0
    cset    w0, ne
    and     w0, w0, 255
    and     w0, w0, 1
    cmp     w0, 0
    bne     .L7
    ldr     x0, [sp, 8]
    cmp     x0, 16
    bls     .L8
    ldr     x0, [sp, 368]
    str     x0, [sp, 56]
    ldr     x0, [sp, 56]
    ldr     q31, [x0]
    nop
    str     q31, [sp, 336]
    ldr     x0, [sp, 376]
    str     x0, [sp, 88]
    ldr     q31, [sp, 336]
    str     q31, [sp, 64]
    ldr     q31, [sp, 64]
    ldr     x0, [sp, 88]
    str     q31, [x0]
    nop
    ldr     x0, [sp, 368]
    add     x0, x0, 16
    str     x0, [sp, 368]
    ldr     x0, [sp, 376]
    add     x0, x0, 16
    str     x0, [sp, 376]
    ldr     x0, [sp, 8]
    sub     x0, x0, #16
    str     x0, [sp, 8]
.L8:
    ldr     x0, [sp, 8]
    cmp     x0, 8
    bls     .L12
    ldr     x0, [sp, 368]
    str     x0, [sp, 32]
    ldr     x0, [sp, 32]
    ldr     d31, [x0]
    nop
    str     d31, [sp, 328]
    ldr     x0, [sp, 376]
    str     x0, [sp, 48]
    ldr     d31, [sp, 328]
    str     d31, [sp, 40]
    ldr     d31, [sp, 40]
    ldr     x0, [sp, 48]
    str     d31, [x0]
    nop
    ldr     x0, [sp, 368]
    add     x0, x0, 8
    str     x0, [sp, 368]
    ldr     x0, [sp, 376]
    add     x0, x0, 8
    str     x0, [sp, 376]
    ldr     x0, [sp, 8]
    sub     x0, x0, #8
    str     x0, [sp, 8]
    b       .L12
.L13:
    ldr     x0, [sp, 368]
    add     x1, x0, 1
    str     x1, [sp, 368]
    ldrb    w2, [x0]
    ldr     x0, [sp, 376]
    add     x1, x0, 1
    str     x1, [sp, 376]
    mov     w1, w2
    strb    w1, [x0]
.L12:
    ldr     x0, [sp, 8]
    sub     x1, x0, #1
    str     x1, [sp, 8]
    cmp     x0, 0
    cset    w0, ne
    and     w0, w0, 255
    and     w0, w0, 1
    cmp     w0, 0
    bne     .L13
    nop
    nop
    add     sp, sp, 384
    ret

NEON_MEMSET:
    sub     sp, sp, #272
        str     x0, [sp, 24]
        strb    w1, [sp, 23]
        str     x2, [sp, 8]
        ldr     x0, [sp, 24]
        str     x0, [sp, 264]
        ldr     x0, [sp, 8]
        lsr     x0, x0, 6
        str     w0, [sp, 260]
        ldr     w0, [sp, 260]
        lsl     w0, w0, 6
        sxtw    x0, w0
        ldr     x1, [sp, 8]
        sub     x0, x1, x0
        str     x0, [sp, 8]
        ldrb    w0, [sp, 23]
        strb    w0, [sp, 231]
        ldr     b31, [sp, 231]
        dup     v31.16b, v31.b[0]
        str     q31, [sp, 240]
        b       .L16
.L17:
        ldr     x0, [sp, 264]
        add     x0, x0, 64
        prfm    PLDL1KEEP, [x0]
        ldr     x0, [sp, 264]
        str     x0, [sp, 120]
        ldr     q31, [sp, 240]
        str     q31, [sp, 96]
        ldr     q31, [sp, 96]
        ldr     x0, [sp, 120]
        str     q31, [x0]
        nop
        ldr     x0, [sp, 264]
        add     x0, x0, 16
        str     x0, [sp, 152]
        ldr     q31, [sp, 240]
        str     q31, [sp, 128]
        ldr     q31, [sp, 128]
        ldr     x0, [sp, 152]
        str     q31, [x0]
        nop
        ldr     x0, [sp, 264]
        add     x0, x0, 32
        str     x0, [sp, 184]
        ldr     q31, [sp, 240]
        str     q31, [sp, 160]
        ldr     q31, [sp, 160]
        ldr     x0, [sp, 184]
        str     q31, [x0]
        nop
        ldr     x0, [sp, 264]
        add     x0, x0, 48
        str     x0, [sp, 216]
        ldr     q31, [sp, 240]
        str     q31, [sp, 192]
        ldr     q31, [sp, 192]
        ldr     x0, [sp, 216]
        str     q31, [x0]
        nop
        ldr     x0, [sp, 264]
        add     x0, x0, 64
        str     x0, [sp, 264]
.L16:
        ldr     w0, [sp, 260]
        sub     w1, w0, #1
        str     w1, [sp, 260]
        cmp     w0, 0
        cset    w0, ne
        and     w0, w0, 255
        and     w0, w0, 1
        cmp     w0, 0
        bne     .L17
        b       .L18
.L19:
        ldr     x0, [sp, 264]
        str     x0, [sp, 88]
        ldr     q31, [sp, 240]
        str     q31, [sp, 64]
        ldr     q31, [sp, 64]
        ldr     x0, [sp, 88]
        str     q31, [x0]
        nop
        ldr     x0, [sp, 264]
        add     x0, x0, 16
        str     x0, [sp, 264]
        ldr     x0, [sp, 8]
        sub     x0, x0, #16
        str     x0, [sp, 8]
.L18:
        ldr     x0, [sp, 8]
        cmp     x0, 15
        bhi     .L19
        b       .L20
.L22:
        ldrb    w0, [sp, 23]
        strb    w0, [sp, 47]
        ldr     b31, [sp, 47]
        dup     v31.8b, v31.b[0]
        str     d31, [sp, 232]
        ldr     x0, [sp, 264]
        str     x0, [sp, 56]
        ldr     d31, [sp, 232]
        str     d31, [sp, 48]
        ldr     d31, [sp, 48]
        ldr     x0, [sp, 56]
        str     d31, [x0]
        nop
        ldr     x0, [sp, 264]
        add     x0, x0, 8
        str     x0, [sp, 264]
        ldr     x0, [sp, 8]
        sub     x0, x0, #8
        str     x0, [sp, 8]
.L20:
        ldr     x0, [sp, 8]
        cmp     x0, 7
        bhi     .L22
        b       .L23
.L24:
        ldrb    w2, [sp, 23]
        ldr     x0, [sp, 264]
        add     x1, x0, 1
        str     x1, [sp, 264]
        mov     w1, w2
        strb    w1, [x0]
.L23:
        ldr     x0, [sp, 8]
        sub     x1, x0, #1
        str     x1, [sp, 8]
        cmp     x0, 0
        cset    w0, ne
        and     w0, w0, 255
        and     w0, w0, 1
        cmp     w0, 0
        bne     .L24
        nop
        nop
        add     sp, sp, 272
        ret
#endif