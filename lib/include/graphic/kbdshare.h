//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT
//
// kbdshare.h - shared keyboard event-ring ABI between the kernel input core
// and userspace GUI clients (desktop WM, notepad). The kernel is the single
// producer (advances head); every client keeps its own read cursor, so several
// GUI apps can drain the same stream without the kernel tracking readers.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KBD_RING_CAP 256u   /* power of two; ring index = seq & (CAP-1) */

/* dev_mmap (syscall 21) first argument that maps the keyboard event page;
   mirrors the kernel X86_KEYBOARD device type (userspace has no dev.h). */
#define KBD_DEV_TYPE 9u

#define KBD_ACTION_UP    0u
#define KBD_ACTION_DOWN  1u

/* modifier snapshot carried with every event */
#define KBD_MOD_LSHIFT 0x01u
#define KBD_MOD_RSHIFT 0x02u
#define KBD_MOD_LCTRL  0x04u
#define KBD_MOD_RCTRL  0x08u
#define KBD_MOD_LALT   0x10u
#define KBD_MOD_RALT   0x20u
#define KBD_MOD_CAPS   0x40u

/* Printable keys carry their ASCII value directly, including the standard
   control chars: Enter '\n' (0x0A), Tab '\t' (0x09), Backspace 0x08 and
   Esc 0x1B. Only keys without an ASCII representation use 0x100+. */
#define KBD_KEY_LEFT  0x100u
#define KBD_KEY_RIGHT 0x101u
#define KBD_KEY_UP    0x102u
#define KBD_KEY_DOWN  0x103u
#define KBD_KEY_HOME  0x104u
#define KBD_KEY_END   0x105u
#define KBD_KEY_PGUP  0x106u
#define KBD_KEY_PGDN  0x107u
#define KBD_KEY_INS   0x108u
#define KBD_KEY_DEL   0x109u

typedef struct KbdEvent {
    uint16_t key;
    uint8_t  action;
    uint8_t  mods;
} KbdEvent;

typedef struct KbdShared {
    volatile uint64_t head;        /* total events produced by the kernel */
    KbdEvent ring[KBD_RING_CAP];
} KbdShared;

#ifdef __cplusplus
}
#endif
