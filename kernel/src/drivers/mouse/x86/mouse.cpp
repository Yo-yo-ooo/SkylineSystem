// SPDX-FileCopyrightText: 2026 Yo-yoo-ooo
// SPDX-License-Identifier: GPL-2.0-only
#ifdef __x86_64__
#include <drivers/mouse/x86/ps2mouse.h>
#include <klib/kio.h>
#include <arch/x86_64/lapic/lapic.h>
#include <arch/x86_64/vmm/vmm.h>
#include <mem/pmm.h>
#include <arch/x86_64/schedule/sched.h>
#include <drivers/dev/dev.h>
#define PS2_DATA_PORT     0x60
#define PS2_STATUS_PORT   0x64
#define PS2_CMD_PORT      0x64

// 8042 控制器命令
#define PS2_CMD_READ_CONFIG   0x20
#define PS2_CMD_WRITE_CONFIG  0x60
#define PS2_CMD_DISABLE_PORT2 0xA7
#define PS2_CMD_ENABLE_PORT2  0xA8
#define PS2_CMD_DISABLE_PORT1 0xAD
#define PS2_CMD_ENABLE_PORT1  0xAE
#define PS2_CMD_SEND_TO_PORT2 0xD4 // 下一个写到 0x60 的数据将发送给鼠标

// 鼠标指令
#define MOUSE_CMD_RESET       0xFF
#define MOUSE_CMD_SET_DEFAULT 0xF6
#define MOUSE_CMD_ENABLE_REPORT 0xF4

// 鼠标响应
#define MOUSE_ACK             0xFA
#define MOUSE_SELF_TEST_PASS  0xAA


ps2_mouse_event PS2MouseEvent = {0};


uint64_t PS2_MOUSE_MemoryMap(
    uint64_t length,uint64_t prot,
    uint64_t offset,uint64_t VADDR
){
    pagemap_t *pm = Schedule::this_proc()->pagemap;
    //uint64_t pages = DIV_ROUND_UP(fb_siz, PAGE_SIZE);

    spinlock_lock(&pm->vma_lock);
    VADDR = VMM::Useless::InternalAlloc(pm, 1,VMM_FLAG_USER | VMM_FLAG_PRESENT);
    spinlock_unlock(&pm->vma_lock);
    VMM::Map4K(pm,VADDR,PHYSICAL(&PS2MouseEvent),VMM_FLAG_USER | VMM_FLAG_PRESENT);
    //VMM::MapRange(pm,VADDR,PHYSICAL(Fb->BaseAddress),MM_USER | VMM_FLAGS_MMIO,pages);
    VMM::NewMapping(pm, VADDR, 1 ,VMM_FLAG_USER | VMM_FLAG_PRESENT);
    uint64_t check_pte = VMM::Useless::GetPageInfo(pm, VADDR).flags;
    kinfoln("VADDR: 0x%X -> PTE Value: 0x%llX", VADDR, check_pte);
    kinfoln("Framebuffer mapped to VADDR: 0x%X", VADDR);
    return (VADDR); 
}

// 内部状态机，用于中断处理中解析 3 字节数据包
static uint8_t mouse_cycle = 0;
static uint8_t mouse_bytes[3];

// 等待 8042 输入缓冲区为空 (可以写数据)
static inline void wait_write() {
    uint32_t timeout = 100000;
    while ((io_in8(PS2_STATUS_PORT) & 0x02) && timeout-- > 0) {
        io_wait();
    }
}

// 等待 8042 输出缓冲区有数据 (可以读数据)
static inline void wait_read() {
    uint32_t timeout = 100000;
    while ((io_in8(PS2_STATUS_PORT) & 0x01) == 0 && timeout-- > 0) {
        io_wait();
    }
}

// 向鼠标发送指令
static void mouse_write(uint8_t data) {
    wait_write();
    io_out8(PS2_CMD_PORT, PS2_CMD_SEND_TO_PORT2); // 告诉控制器接下来是给鼠标的
    io_wait();
    
    wait_write();
    io_out8(PS2_DATA_PORT, data);
}

// 读取鼠标返回的数据
static uint8_t mouse_read(void) {
    wait_read();
    return io_in8(PS2_DATA_PORT);
}

bool ps2_mouse_init(void) {
    uint8_t config;
    uint8_t response;

    // 1. 禁用中断，防止初始化过程中被打断
    DISABLE_INTERRUPTS();

    // 2. 禁用 PS/2 第一和第二端口
    wait_write(); io_out8(PS2_CMD_PORT, PS2_CMD_DISABLE_PORT1);
    io_wait();
    wait_write(); io_out8(PS2_CMD_PORT, PS2_CMD_DISABLE_PORT2);
    io_wait();

    // 3. 清空输出缓冲区
    while (io_in8(PS2_STATUS_PORT) & 0x01) {
        io_in8(PS2_DATA_PORT);
        io_wait();
    }

    // 4. 读取当前控制器配置字节
    wait_write();
    io_out8(PS2_CMD_PORT, PS2_CMD_READ_CONFIG);
    io_wait();
    wait_read();
    config = io_in8(PS2_DATA_PORT);

    // 5. 修改配置：
    // - 清除位 0 (禁用第一端口中断)
    // - 设置位 1 (启用第二端口中断, IRQ12)
    // - 清除位 4 (禁用第一端口时钟)
    // - 设置位 5 (启用第二端口时钟)
    config = (config & ~0x01) | 0x02; 
    config = (config & ~0x10) | 0x20;

    wait_write();
    io_out8(PS2_CMD_PORT, PS2_CMD_WRITE_CONFIG);
    io_wait();
    wait_write();
    io_out8(PS2_DATA_PORT, config);
    io_wait();

    // 6. 启用第二端口 (鼠标)
    wait_write();
    io_out8(PS2_CMD_PORT, PS2_CMD_ENABLE_PORT2);
    io_wait();

    // 7. 复位鼠标
    mouse_write(MOUSE_CMD_RESET);
    response = mouse_read();
    if (response != MOUSE_ACK) {
        ENABLE_INTERRUPTS();
        return false;
    }
    
    // 复位后会返回 0xAA (自检通过) 和 0x00 (鼠标ID)
    response = mouse_read();
    if (response != MOUSE_SELF_TEST_PASS) {
        ENABLE_INTERRUPTS();
        return false;
    }
    mouse_read(); // 读取并丢弃 Mouse ID

    // 8. 设置默认状态
    mouse_write(MOUSE_CMD_SET_DEFAULT);
    mouse_read(); // ACK

    // 9. 启用数据报告 (让鼠标开始自动发包)
    mouse_write(MOUSE_CMD_ENABLE_REPORT);
    mouse_read(); // ACK

    // 10. 重新启用第一端口 (如果你使用键盘的话)
    wait_write();
    io_out8(PS2_CMD_PORT, PS2_CMD_ENABLE_PORT1);
    io_wait();
    VDL PS2MouseDev = {0};
    DevOPS ops = {0};
    ops.MemoryMap = PS2_MOUSE_MemoryMap;
    Dev::AddDevice(PS2MouseDev,X86_PS2_MOUSE,ops);
    idt_install_irq(32+12,(void*)ps2_mouse_handler);
    // 11. 恢复中断
    ENABLE_INTERRUPTS();

    
    //uint16_t X = PAGE_SIZE - sizeof(ps2_mouse_state_t);


    // 初始化状态机
    mouse_cycle = 0;
    return true;
}

void ps2_mouse_handler(void) {
    // 检查状态寄存器，位 0 为 1 表示有数据可读
    // 位 5 为 1 表示数据来自鼠标 (第二端口)
    uint8_t status = io_in8(PS2_STATUS_PORT);
    if (!(status & 0x01)) {
        return; // 没有数据
    }
    if (!(status & 0x20)) {
        // 数据来自键盘，忽略或交给键盘处理程序
        io_in8(PS2_DATA_PORT); 
        return;
    }

    uint8_t data = io_in8(PS2_DATA_PORT);
    PS2MouseEvent.ps2_mouse_state.seq++;

    switch (mouse_cycle) {
        case 0:
            // 字节 0: [Y溢出, X溢出, Y符号, X符号, 1, 中键, 右键, 左键]
            // 始终检查位 3 是否为 1 以同步数据包
            if (data & 0x08) {
                mouse_bytes[0] = data;
                mouse_cycle++;
            }
            break;
        case 1:
            // 字节 1: X 移动增量
            mouse_bytes[1] = data;
            mouse_cycle++;
            break;
        case 2:
            // 字节 2: Y 移动增量
            mouse_bytes[2] = data;
            mouse_cycle = 0; // 重置状态机，准备接收下一个包

            // 解析按键状态
            PS2MouseEvent.ps2_mouse_state.left = mouse_bytes[0] & 0x01;
            PS2MouseEvent.ps2_mouse_state.right = (mouse_bytes[0] >> 1) & 0x01;
            PS2MouseEvent.ps2_mouse_state.middle = (mouse_bytes[0] >> 2) & 0x01;

            // 解析 X 移动
            int16_t dx = mouse_bytes[1];
            if (mouse_bytes[0] & 0x10) { // X 符号位为 1，表示负方向移动
                dx |= 0xFF00; // 符号扩展到 16 位
            }
            PS2MouseEvent.ps2_mouse_state.x += dx;

            // 解析 Y 移动
            int16_t dy = mouse_bytes[2];
            if (mouse_bytes[0] & 0x20) { // Y 符号位为 1，表示负方向移动
                dy |= 0xFF00; // 符号扩展到 16 位
            }
            // PS/2 鼠标的 Y 轴方向是向上的，而屏幕坐标系通常是向下的，所以这里取反
            PS2MouseEvent.ps2_mouse_state.y -= dy; 

            break;
    }
    PS2MouseEvent.ps2_mouse_state.seq++;
    
    LAPIC::EOI();
}
#endif