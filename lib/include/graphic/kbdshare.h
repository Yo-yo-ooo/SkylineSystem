//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT
//
// kbdshare.h - shared keyboard event-ring ABI between the kernel input core
// and userspace GUI clients (desktop WM, notepad). The kernel is the single
// producer (advances head). Each reader acquires one consumer slot, publishes
// its read cursor and can resync after an overwrite, so the kernel observes
// the slowest reader and counts lost events; multiple GUI apps still drain
// the same stream independently.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KBD_RING_CAP 256u   /* power of two; ring index = seq & (CAP-1) */
#define KBD_MAX_READERS 8u  /* concurrent GUI consumers of the event page */

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

    /* Per-reader cursors, encoded as (consumed_seq + 1) so that 0 marks a
       free slot. A reader atomically acquires a slot once at startup and then
       publishes its progress; the kernel reads the slowest slot. */
    volatile uint64_t consumer[KBD_MAX_READERS];

    KbdEvent ring[KBD_RING_CAP];
} KbdShared;

/* ---- reader-side cursor protocol (shared by kernel and userspace) -------- */

/* Acquire a reader slot. The reader joins at the current head (all earlier
   events are skipped), which also keeps a late-joining reader from replaying
   an overwritten ring. Returns the slot index, or -1 if all slots are taken. */
static inline int kbd_reader_register(KbdShared* k) {
    for (uint32_t i = 0; i < KBD_MAX_READERS; i++) {
        uint64_t expected = 0;
        uint64_t h = __atomic_load_n(&k->head, __ATOMIC_ACQUIRE);
        uint64_t want = h + 1;   /* encode consumed_seq = h */
        if (__atomic_compare_exchange_n(&k->consumer[i], &expected, want,
                                        0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            return (int)i;
    }
    return -1;
}

/* Release a reader slot (best effort at process teardown). */
static inline void kbd_reader_unregister(KbdShared* k, int slot) {
    if (slot >= 0)
        __atomic_store_n(&k->consumer[slot], 0, __ATOMIC_RELEASE);
}

/* Next event sequence the reader should consume. */
static inline uint64_t kbd_reader_pos(KbdShared* k, int slot) {
    return __atomic_load_n(&k->consumer[slot], __ATOMIC_ACQUIRE) - 1;
}

/* Publish that events through `seq` (exclusive) have been consumed. */
static inline void kbd_reader_setpos(KbdShared* k, int slot, uint64_t seq) {
    __atomic_store_n(&k->consumer[slot], seq + 1, __ATOMIC_RELEASE);
}

/* Clamp a reader cursor when the producer has lapped the ring: the events in
   (cursor .. head-CAP) were overwritten and must be skipped. Returns the
   resynced cursor. */
static inline uint64_t kbd_reader_resync(KbdShared* k, uint64_t cursor) {
    uint64_t h = __atomic_load_n(&k->head, __ATOMIC_ACQUIRE);
    if (h > cursor && h - cursor > KBD_RING_CAP)
        return h - KBD_RING_CAP;
    return cursor;
}

#ifdef __cplusplus
}
#endif
