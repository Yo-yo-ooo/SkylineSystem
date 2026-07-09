//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
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

// 优化后的中断处理程序：无忙等待，极速执行
void keyboard_handler(registers *regs) {
    (void)regs;

    
    // 既然中断触发了，状态寄存器的 OBF 位必定为 1，直接读取即可
    // 但为了严谨，可以做一次非阻塞检查
    if (inb(0x64) & 0x01) {
        u8 key = inb(0x60);
        
        // 快速解析按键（无锁，因为 PS/2 键盘中断通常只在 CPU0 触发，
        keyboard_state = (key & 0x80) ? 2 : 1;
        u8 scancode = key & 0x7F;
        
        // 构造事件
        keyboard_event ev;
        ev.type = 1;
        ev.value = keyboard_state;
        ev.code = kb_map_keys[scancode]; // 简化版解析
        
        //thread_t *thread = Schedule::this_thread();
        
    }
    
    LAPIC::EOI(); // 尽早发送 EOI
}

i32 keyboard_read(u8 *buffer)
{
    FIFO::Pop(keyboard_fifo, buffer);
    return sizeof(keyboard_event);
}

void keyboard_init() {
    // 禁用第一端口
    outb(0x64, 0xAD);
    // 清空输出缓冲区（通过读取清空）
    while (inb(0x64) & 0x01) inb(0x60);
    
    // 获取当前状态
    outb(0x64, 0x20);
    u8 state = inb(0x60);
    state |= 0x01; // 启用第一端口中断
    state &= ~0x10; // 禁用第一端口时钟（可选）
    state |= 0x40; // 启用第一端口时钟（确保启用）
    
    // 写入状态
    outb(0x64, 0x60);
    outb(0x60, state);
    
    // 启用第一端口
    outb(0x64, 0xAE);
    
    // 注册中断
    idt_install_irq(33, keyboard_handler);
    IOAPIC::RemapIRQ(smp_bsp_cpu,1,33,false);
}
#endif