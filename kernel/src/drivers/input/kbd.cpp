//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
#ifdef __x86_64__
#include <drivers/input/kbd.h>
#include <drivers/dev/dev.h>
#include <arch/x86_64/schedule/sched.h>
#include <arch/x86_64/vmm/vmm.h>
#include <klib/kio.h>
#include <drivers/usb/hid.h>

namespace Kbd {

/* Single shared event page: kernel produces under g_lock, clients read
   (mmap read-only) with their own cursors. Fits in one 4 KiB page. */
__attribute__((aligned(PAGE_SIZE)))
static KbdShared g_shared;
static spinlock_t g_lock = 0;

void Post(uint16_t key, uint8_t action, uint8_t mods) {
    /* 修复: Post 可被 IRQ1 中断路径调用(keyboard.cpp 的键盘 ISR), 普通
       spinlock_lock 若与本 CPU 上正在持锁的上下文重入将永久自旋(同核
       自死锁); 换 irqsave 版本关中断后再拿锁。 */
    uint64_t rflags = spin_lock_irqsave(&g_lock);
    uint64_t h = g_shared.head;
    KbdEvent& e = g_shared.ring[h & (KBD_RING_CAP - 1u)];
    e.key = key;
    e.action = action;
    e.mods = mods;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    g_shared.head = h + 1;
    spin_unlock_irqrestore(&g_lock, rflags);
}

/* Map the shared event page into the calling GUI process (user, NX). */
static uint64_t KbdMmap(uint64_t /*len*/, uint64_t /*prot*/,
                        uint64_t /*off*/, uint64_t /*hint*/) {
    pagemap_t* pm = Schedule::this_proc()->pagemap;
    uint64_t flags = MM_USER | VMM_FLAG_PRESENT | MM_NX;

    uint64_t phys = VMM::GetPhysics(kernel_pagemap, (uint64_t)&g_shared);
    if (!phys) return 0;

    spinlock_lock(&pm->vma_lock);
    uint64_t va = VMM::Internal::InternalAlloc(pm, 1, flags);
    if (!va) { spinlock_unlock(&pm->vma_lock); return 0; }

    spinlock_lock(&pm->pt_lock);
    VMM::Map4K(pm, va, phys, flags);
    VMM::NewMapping(pm, va, 1, flags);
    spinlock_unlock(&pm->pt_lock);
    spinlock_unlock(&pm->vma_lock);

    __asm__ volatile("invlpg (%0)" : : "r"(va) : "memory");
    return va;
}

/* ===================== USB HID keyboard adaptation ===================== */
static uint8_t g_prevKeys[6] = {0};
static uint8_t g_prevMods = 0;
static bool g_caps = false;

static int find_usage(uint8_t usage, const uint8_t* keys) {
    for (int i = 0; i < 6; i++)
        if (keys[i] == usage) return i;
    return -1;
}

/* Translate one HID usage (+ modifier snapshot) to the shared key value. */
static uint16_t hid_usage_key(uint8_t u, uint8_t mods) {
    const bool shifted = (mods & (KBD_MOD_LSHIFT | KBD_MOD_RSHIFT)) != 0;
    const bool caps    = (mods & KBD_MOD_CAPS) != 0;

    if (u >= 0x04 && u <= 0x1D) {           /* letters a..z */
        char c = (char)('a' + (u - 0x04));
        if (shifted ^ caps) c = (char)(c - 32);
        return (uint16_t)c;
    }
    static const char numRow[]   = "1234567890";
    static const char numShift[] = "!@#$%^&*()";
    if (u >= 0x1E && u <= 0x27)
        return (uint16_t)(shifted ? numShift[u - 0x1E] : numRow[u - 0x1E]);

    switch (u) {
    case 0x28: return '\n';
    case 0x29: return 0x1B;
    case 0x2A: return 0x08;
    case 0x2B: return '\t';
    case 0x2C: return ' ';
    case 0x2D: return shifted ? '_'  : '-';
    case 0x2E: return shifted ? '+'  : '=';
    case 0x2F: return shifted ? '{'  : '[';
    case 0x30: return shifted ? '}'  : ']';
    case 0x31: return shifted ? '|'  : '\\';
    case 0x33: return shifted ? ':'  : ';';
    case 0x34: return shifted ? '"'  : '\'';
    case 0x35: return shifted ? '~'  : '`';
    case 0x36: return shifted ? '<'  : ',';
    case 0x37: return shifted ? '>'  : '.';
    case 0x38: return shifted ? '?'  : '/';
    case 0x4F: return KBD_KEY_RIGHT;
    case 0x50: return KBD_KEY_LEFT;
    case 0x51: return KBD_KEY_DOWN;
    case 0x52: return KBD_KEY_UP;
    case 0x4A: return KBD_KEY_HOME;
    case 0x4D: return KBD_KEY_END;
    case 0x4B: return KBD_KEY_PGUP;
    case 0x4E: return KBD_KEY_PGDN;
    case 0x49: return KBD_KEY_INS;
    case 0x4C: return KBD_KEY_DEL;
    default:   return 0;
    }
}

/* HID reports are current-key snapshots: diff against the previous report to
   emit press/release edges for every changed usage. */
static void OnHidReport(const USB::HID::HIDReport& r) {
    /* CapsLock (usage 0x39) toggles on its press edge. */
    bool capsNow = (find_usage(0x39, r.keys) >= 0);
    bool capsWas = (find_usage(0x39, g_prevKeys) >= 0);
    if (capsNow && !capsWas) g_caps = !g_caps;

    uint8_t mods = 0;
    if (r.modifiers & 0x02) mods |= KBD_MOD_LSHIFT;
    if (r.modifiers & 0x20) mods |= KBD_MOD_RSHIFT;
    if (r.modifiers & 0x01) mods |= KBD_MOD_LCTRL;
    if (r.modifiers & 0x10) mods |= KBD_MOD_RCTRL;
    if (r.modifiers & 0x04) mods |= KBD_MOD_LALT;
    if (r.modifiers & 0x40) mods |= KBD_MOD_RALT;
    if (g_caps) mods |= KBD_MOD_CAPS;

    for (int i = 0; i < 6; i++) {
        uint8_t u = g_prevKeys[i];
        if (u && find_usage(u, r.keys) < 0)
            Post(hid_usage_key(u, g_prevMods), KBD_ACTION_UP, g_prevMods);
    }
    for (int i = 0; i < 6; i++) {
        uint8_t u = r.keys[i];
        if (u && find_usage(u, g_prevKeys) < 0)
            Post(hid_usage_key(u, mods), KBD_ACTION_DOWN, mods);
    }
    __memcpy(g_prevKeys, r.keys, 6);
    g_prevMods = mods;
}

void Init() {
    VDL dev = {};
    DevOPS ops = {};
    ops.MemoryMap = KbdMmap;
    Dev::AddDevice(dev, X86_KEYBOARD, ops);
    USB::HID::RegisterKeyboard(OnHidReport);
    kpokln("[kbd] input core online (shared event ring)");
}

} // namespace Kbd
#endif
