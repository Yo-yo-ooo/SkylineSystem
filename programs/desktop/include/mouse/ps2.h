#pragma once
#ifndef PS2_MOUSE_H_
#define PS2_MOUSE_H_

#include <stdint.h>
#include <stddef.h>

extern uint64_t mouse_addr;

typedef struct {
    int32_t x;          // 当前 X 坐标（相对移动累积或绝对坐标）
    int32_t y;          // 当前 Y 坐标
    uint8_t left;       // 左键状态 (1=按下, 0=松开)
    uint8_t right;      // 右键状态
    uint8_t middle;     // 中键状态
    uint64_t seq;
} ps2_mouse_state_t;

void MouseInit();

#endif