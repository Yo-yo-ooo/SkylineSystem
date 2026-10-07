//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT
#pragma once
#ifndef PS2_MOUSE_H_
#define PS2_MOUSE_H_

#include <stdint.h>
#include <stddef.h>
#include <graphic/mouseshare.h>   /* ps2_mouse_state_t: 布局的单一权威 */

extern uint64_t mouse_addr;

static inline ps2_mouse_state_t* mouse_state(void) {
    return (ps2_mouse_state_t*)mouse_addr;
}

void MouseInit();

#endif