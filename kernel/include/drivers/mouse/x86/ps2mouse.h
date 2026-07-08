// SPDX-FileCopyrightText: 2026 Yo-yo-ooo
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <stdint.h>
#include <stdbool.h>

// 鼠标状态结构体
typedef struct {
    int32_t x;          // 当前 X 坐标（相对移动累积或绝对坐标）
    int32_t y;          // 当前 Y 坐标
    uint8_t left;       // 左键状态 (1=按下, 0=松开)
    uint8_t right;      // 右键状态
    uint8_t middle;     // 中键状态
} ps2_mouse_state_t;

// 全局鼠标状态
extern ps2_mouse_state_t g_ps2_mouse_state;
extern uint64_t ps2stateAddr_v;

// 初始化 PS/2 鼠标
bool ps2_mouse_init(void);

// PS/2 鼠标中断处理函数 (挂载到 IRQ12)
void ps2_mouse_handler(void);