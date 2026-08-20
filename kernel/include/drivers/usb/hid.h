//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <drivers/usb/usb_device.h>
#include <pdef.h>

namespace USB::HID {

PACK(struct HIDReport { uint8_t modifiers; uint8_t reserved; uint8_t keys[6]; });
PACK(struct MouseReport { uint8_t buttons; int8_t x; int8_t y; int8_t wheel; });

void Init(Device* dev, Interface* ifce);
void Deinit(Device* dev); // 新增

using KeyboardCallback = void(*)(const HIDReport&);
using MouseCallback     = void(*)(const MouseReport&);
void RegisterKeyboard(KeyboardCallback cb);
void UnregisterKeyboard(KeyboardCallback cb);
void RegisterMouse(MouseCallback cb);
void UnregisterMouse(MouseCallback cb);

} // namespace USB::HID