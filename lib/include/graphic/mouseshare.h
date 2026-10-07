//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT
//
// mouseshare.h — shared PS/2 mouse state page ABI between the kernel input
// core and userspace clients. The kernel is the single producer; any number
// of processes may map it with dev_mmap(DEV_TYPE_PS2_MOUSE) (syscall 21) and
// read it independently, the same way several clients drain the keyboard ring
// (graphic/kbdshare.h).
//
// Coordinates are in SCREEN space and are clamped by the kernel; a client
// that wants relative motion (DOOM's mouse look) simply diffs consecutive
// samples. This header is the single authority for the layout — do not
// re-declare the struct anywhere else.
#pragma once

#include <stdint.h>
#include <graphic/devtype.h>

typedef struct {
    int32_t x;          /* current cursor X (screen space)                */
    int32_t y;          /* current cursor Y                               */
    uint8_t left;       /* 1 = pressed                                    */
    uint8_t right;
    uint8_t middle;
    uint64_t seq;       /* bumped by the kernel on every update           */
} ps2_mouse_state_t;
