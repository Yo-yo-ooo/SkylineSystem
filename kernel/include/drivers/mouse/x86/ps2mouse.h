// SPDX-FileCopyrightText: 2026 Yo-yo-ooo
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <pdef.h>

// 鼠标状态结构体
typedef struct {
    int32_t x;          // 当前 X 坐标（相对移动累积或绝对坐标）
    int32_t y;          // 当前 Y 坐标
    uint8_t left;       // 左键状态 (1=按下, 0=松开)
    uint8_t right;      // 右键状态
    uint8_t middle;     // 中键状态
    uint64_t seq;
} ps2_mouse_state_t;

typedef struct ps2_mouse_event{
    ps2_mouse_state_t ps2_mouse_state;
    char padding[4096 - sizeof(ps2_mouse_state_t)];
}ps2_mouse_event;

extern ps2_mouse_event PS2MouseEvent;

// 初始化 PS/2 鼠标
bool ps2_mouse_init(void);
