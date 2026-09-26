//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <graphic/kbdshare.h>

/* Kernel keyboard input core. Both the PS/2 set-1 ISR and the USB HID
   keyboard callback translate their native reports into the shared ABI and
   call Post(); the event ring is mmap-able read-only by GUI clients. */
namespace Kbd {
    void Post(uint16_t key, uint8_t action, uint8_t mods);
    void Init();
}
