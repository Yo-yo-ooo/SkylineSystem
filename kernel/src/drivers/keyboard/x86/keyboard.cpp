//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
#ifdef __x86_64__
#include <drivers/keyboard/x86/keyboard.h>
#include <arch/x86_64/ioapic/ioapic.h>
#include <arch/x86_64/lapic/lapic.h>
#include <arch/x86_64/smp/smp.h>
#include <klib/kio.h>
#include <drivers/keyboard/x86/keyboard_map.h>
#include <drivers/input/kbd.h>

/* ---- PS/2 set-1 scancode -> shared event-ring state machine ------------- */
static bool k_lshift = false, k_rshift = false;
static bool k_lctrl  = false, k_rctrl  = false;
static bool k_lalt   = false, k_ralt   = false;
static bool k_caps   = false;
static bool k_ext    = false;

static uint8_t current_mods() {
    uint8_t m = 0;
    if (k_lshift) m |= KBD_MOD_LSHIFT;
    if (k_rshift) m |= KBD_MOD_RSHIFT;
    if (k_lctrl)  m |= KBD_MOD_LCTRL;
    if (k_rctrl)  m |= KBD_MOD_RCTRL;
    if (k_lalt)   m |= KBD_MOD_LALT;
    if (k_ralt)   m |= KBD_MOD_RALT;
    if (k_caps)   m |= KBD_MOD_CAPS;
    return m;
}

static bool is_shift() { return k_lshift || k_rshift; }

/* Extended (0xE0-prefixed) keys: right modifiers + nav cluster. */
static void feed_extended(uint8_t base, bool up) {
    const uint8_t action = up ? KBD_ACTION_UP : KBD_ACTION_DOWN;
    switch (base) {
    case 0x1D: k_rctrl = !up; return;
    case 0x38: k_ralt  = !up; return;
    case 0x48: Kbd::Post(KBD_KEY_UP,    action, current_mods()); return;
    case 0x50: Kbd::Post(KBD_KEY_DOWN,  action, current_mods()); return;
    case 0x4B: Kbd::Post(KBD_KEY_LEFT,  action, current_mods()); return;
    case 0x4D: Kbd::Post(KBD_KEY_RIGHT, action, current_mods()); return;
    case 0x47: Kbd::Post(KBD_KEY_HOME, action, current_mods()); return;
    case 0x4F: Kbd::Post(KBD_KEY_END,  action, current_mods()); return;
    case 0x49: Kbd::Post(KBD_KEY_PGUP, action, current_mods()); return;
    case 0x51: Kbd::Post(KBD_KEY_PGDN, action, current_mods()); return;
    case 0x52: Kbd::Post(KBD_KEY_INS, action, current_mods()); return;
    case 0x53: Kbd::Post(KBD_KEY_DEL, action, current_mods()); return;
    default: return;
    }
}

static void feed_normal(uint8_t base, bool up) {
    /* modifier keys only update internal state (characters already carry the
       resulting ASCII / modifier snapshot) */
    switch (base) {
    case 0x2A: k_lshift = !up; return;
    case 0x36: k_rshift = !up; return;
    case 0x1D: k_lctrl  = !up; return;
    case 0x38: k_lalt   = !up; return;
    case 0x3A: if (!up) k_caps = !k_caps; return;
    default: break;
    }

    uint8_t c = kb_map_keys[base];
    if (c >= 'a' && c <= 'z') {
        /* letters: shift and caps each invert case, together they cancel */
        if (is_shift() != k_caps) c = (uint8_t)(c - 32);
    } else if (is_shift()) {
        c = kb_map_keys_shift[base];
    }
    if (c)
        Kbd::Post(c, up ? KBD_ACTION_UP : KBD_ACTION_DOWN, current_mods());
}

static void feed_scancode(uint8_t code) {
    if (code == 0xE0) { k_ext = true; return; }
    if (code == 0x00 || code == 0xFF) { k_ext = false; return; }

    bool up = (code & 0x80) != 0;
    uint8_t base = code & 0x7F;
    if (k_ext) { k_ext = false; feed_extended(base, up); }
    else        feed_normal(base, up);
}

void keyboard_wait_write()
{
    for (int32_t i = 0; i < 10000; i++)
    {
        if (!(inb(0x64) & 0x02))
        {
            return;
        }
    }
}

int32_t keyboard_wait_read()
{
    for (int32_t i = 0; i < 10000; i++)
    {
        if ((inb(0x64) & 0x01) == 1)
        {
            return 0;
        }
    }
    return 1;
}

/* IRQ1: translate the waiting set-1 scancode into the shared event ring. */
void keyboard_handler(registers *regs) {
    (void)regs;

    uint8_t status = io_in8(0x64);
    /* bit5 == 0 means this byte belongs to the keyboard (not the mouse) */
    if ((status & 0x01) && !(status & 0x20)) {
        uint8_t code = io_in8(0x60);
        feed_scancode(code);
    }

    LAPIC::EOI();
}

void keyboard_init() {
    keyboard_wait_write();
    outb(0x64, 0xAD); // 禁用第一端口

    // 清空输出缓冲区
    while (inb(0x64) & 0x01) {
        keyboard_wait_read();
        inb(0x60);
    }

    // 获取当前配置
    keyboard_wait_write();
    outb(0x64, 0x20);
    keyboard_wait_read();
    u8 state = inb(0x60);

    // 4. 修改配置
    state |= 0x01;  // 设置 Bit 0: 启用第一端口中断 (IRQ1)
    state &= ~0x10; // 清除 Bit 4: 启用第一端口时钟 (1=禁用, 0=启用)
    state |= 0x40;  // 设置 Bit 6: 启用转换模式 (Set 2 -> Set 1)
    // 注意：不要动 Bit 1 和 Bit 5，保留鼠标的配置！

    // 写入配置
    keyboard_wait_write();
    outb(0x64, 0x60);
    keyboard_wait_write();
    outb(0x60, state);

    // 启用第一端口
    keyboard_wait_write();
    outb(0x64, 0xAE);

    // 注册中断
    idt_install_irq(33, (void*)keyboard_handler);
    IOAPIC::RemapIRQ(smp_bsp_cpu,1,33,false);

    /* Bring up the shared input core (registers the mmap device + USB HID). */
    Kbd::Init();
}
#endif
