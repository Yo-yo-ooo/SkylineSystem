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

    uint8_t status = io_in8(0x64);
    // 如果缓冲区有数据，且不是鼠标数据(Bit 5 == 0)
    if ((status & 0x01) && !(status & 0x20)) {
        io_in8(0x60); // 读出键盘数据，排空缓冲区
    }
    
    LAPIC::EOI(); // 尽早发送 EOI
}

i32 keyboard_read(u8 *buffer)
{
    FIFO::Pop(keyboard_fifo, buffer);
    return sizeof(keyboard_event);
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
    idt_install_irq(33, keyboard_handler);
    IOAPIC::RemapIRQ(smp_bsp_cpu,1,33,false);
}
#endif