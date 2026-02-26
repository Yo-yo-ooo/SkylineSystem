/*
* SPDX-License-Identifier: GPL-2.0-only
* File: keyboard.cpp
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
#ifdef __x86_64__
#include <arch/x86_64/allin.h>
#include <drivers/keyboard/x86/keyboard.h>
#include <flanterm/flanterm.h>
#include <drivers/keyboard/x86/keyboard_map.h>

bool keyboard_pressed = false;

u32 keyboard_char = '\0';
bool keyboard_caps = false;
bool keyboard_shift = false;
u8 keyboard_state = 0; // 0 = nothing, 1 = pressed 2 = released

fifo *keyboard_fifo;

spinlock_t kb_lock = 0;

void keyboard_handle_key(u8 key)
{
    spinlock_lock(&kb_lock);

    switch (key)
    {
    case 0x2a:
        // Shift
        keyboard_shift = true;
        keyboard_state = 1;
        break;
    case 0xaa:
        keyboard_shift = false;
        keyboard_state = 2;
        break;
    case 0x3a:
        // Caps
        keyboard_caps = !keyboard_caps;
        keyboard_state = (keyboard_state == 0 ? 1 : 2);
        //kprintf("   ");
        break;
    default:{
        // Letter
        if (!(key & 0x80))
        {
            keyboard_state = 1;
        }
        else
        {
            keyboard_state = 2;
            key -= 0x80;
        }
        if (keyboard_shift)
            keyboard_char = kb_map_keys_shift[key];
        else if (keyboard_caps)
            keyboard_char = kb_map_keys_caps[key];
        else
            keyboard_char = kb_map_keys[key];
        keyboard_event ev;
        ev.type = 1;
        ev.value = keyboard_state;
        ev.code = keyboard_char;
        //if(keyboard_state == 1 )
        //    kprintf("%c", keyboard_char);
        FIFO::Push(keyboard_fifo, &ev);
        break;
    }}
    spinlock_unlock(&kb_lock);
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

void keyboard_handler(registers *regs)
{
    (void)regs;

    u8 key;

    if (!keyboard_wait_read())
    {
        key = inb(0x60);
        keyboard_handle_key(key);
    }
    else
    {
        keyboard_char = 0;
        keyboard_pressed = false;
    }
    LAPIC::EOI();
}

i32 keyboard_read(u8 *buffer)
{
    FIFO::Pop(keyboard_fifo, buffer);
    return sizeof(keyboard_event);
}

void keyboard_init()
{
    // https://wiki.osdev.org/%228042%22_PS/2_Controller#Command_Register
    keyboard_wait_write();
    outb(0x64, 0xAD);
    keyboard_wait_write();
    outb(0x64, 0x20);
    keyboard_wait_read();
    u8 state = inb(0x60);
    state |= (1 << 0) | (1 << 6);
    if ((state & (1 << 5)) != 0)
    {
        state |= (1 << 1);
    }
    keyboard_wait_write();
    outb(0x64, 0x60);
    keyboard_wait_write();
    outb(0x60, state);

    keyboard_wait_write();
    outb(0x64, 0xAE);
    if ((state & (1 << 5)) != 0)
    {
        outb(0x64, 0xa8);
    }

    keyboard_fifo = FIFO::Create(256, sizeof(keyboard_event));
    
    /*
    kb_node = (vfs_node *)kmalloc(sizeof(vfs_node));
    kb_node->name = (char *)kmalloc(9);
    __memcpy(kb_node->name, "keyboard", 9);
    kb_node->ino = 0;
    kb_node->write = 0;
    kb_node->read = keyboard_read;
    kb_node->readdir = 0;
    kb_node->finddir = 0;
    kb_node->size = 1;
    kb_node->type = VFS_DEVICE;
    //Dev::Add(kb_node);
    */

    //irq_register(1, keyboard_handler);
    idt_install_irq(33,keyboard_handler);
    inb(0x60);
}
#endif