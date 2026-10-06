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
#include <graphic/devtype.h>   /* P5-97 */

#ifdef __cplusplus
extern "C" {
#endif

#define KBD_RING_CAP 256u   /* power of two; ring index = seq & (CAP-1) */
#define KBD_MAX_READERS 8u  /* concurrent GUI consumers of the event page */

/* dev_mmap (syscall 21) first argument that maps the keyboard event page;
   P5-97: 权威在 graphic/devtype.h (DEV_TYPE_KEYBOARD), 此处别名保持
   兼容 —— 两个定义若漂移即 ABI 错位, 用户态引用一律走 devtype.h。 */
#define KBD_DEV_TYPE DEV_TYPE_KEYBOARD

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

    /* Focus routing (click-to-front keyboard delivery). The WM publishes the
       single reader that should receive keys. focus_owner == 0 means broadcast
       (every reader processes events); otherwise it stores (slot index + 1) of
       the focused window. slot_pid records the pid owning each slot so the WM
       can map a clicked window (client pid) to its reader slot. */
    volatile int64_t focus_owner;                  /* 0 = broadcast */
    volatile int32_t slot_pid[KBD_MAX_READERS];    /* pid owning slot, 0 = free */

    KbdEvent ring[KBD_RING_CAP];
} KbdShared;

/* ---- focus routing helpers ---------------------------------------------- */

/* Broadcast keys to every reader (legacy behaviour). */
static inline void kbd_focus_broadcast(KbdShared* k) {
    __atomic_store_n(&k->focus_owner, 0, __ATOMIC_RELEASE);
}

/* Direct keys at one reader slot. */
static inline void kbd_focus_set_slot(KbdShared* k, int slot) {
    __atomic_store_n(&k->focus_owner, (int64_t)slot + 1, __ATOMIC_RELEASE);
}

/* Focused slot, or -1 when broadcasting. */
static inline int kbd_focus_slot(KbdShared* k) {
    int64_t f = __atomic_load_n(&k->focus_owner, __ATOMIC_ACQUIRE);
    return f > 0 ? (int)(f - 1) : -1;
}

/* Map a client pid to its reader slot, or -1 if it has not registered. */
static inline int kbd_slot_for_pid(KbdShared* k, int32_t pid) {
    for (uint32_t i = 0; i < KBD_MAX_READERS; i++)
        if (__atomic_load_n(&k->slot_pid[i], __ATOMIC_ACQUIRE) == pid && pid != 0)
            return (int)i;
    return -1;
}

/* Whether this reader should process events right now. */
static inline int kbd_focus_allows(KbdShared* k, int slot) {
    int f = kbd_focus_slot(k);
    return f < 0 || f == slot;
}


/* ---- reader-side cursor protocol (shared by kernel and userspace) -------- */

/* Acquire a reader slot. The reader joins at the current head (all earlier
   events are skipped), which also keeps a late-joining reader from replaying
   an overwritten ring. `pid` is the reader's process id, recorded so the WM
   can focus a clicked window. Returns the slot index, or -1 if all slots
   are taken. */
static inline int kbd_reader_register(KbdShared* k, int32_t pid) {
    for (uint32_t i = 0; i < KBD_MAX_READERS; i++) {
        uint64_t expected = 0;
        uint64_t h = __atomic_load_n(&k->head, __ATOMIC_ACQUIRE);
        uint64_t want = h + 1;   /* encode consumed_seq = h */
        if (__atomic_compare_exchange_n(&k->consumer[i], &expected, want,
                                        0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            __atomic_store_n(&k->slot_pid[i], pid, __ATOMIC_RELEASE);
            return (int)i;
        }
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
