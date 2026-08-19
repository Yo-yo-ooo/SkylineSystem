//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
// usb/hid.cpp
#include <drivers/usb/hid.h>
#include <drivers/usb/xhci.h>
#include <klib/kio.h>

namespace USB::HID {

static KeyboardCallback g_kbCallbacks[8];
static uint8_t g_kbCbCount = 0;
static MouseCallback g_msCallbacks[8];
static uint8_t g_msCbCount = 0;
static spinlock_t g_cbLock = 0;

void RegisterKeyboard(KeyboardCallback cb) {
    spinlock_lock(&g_cbLock);
    if (g_kbCbCount < 8) g_kbCallbacks[g_kbCbCount++] = cb;
    spinlock_unlock(&g_cbLock);
}
void UnregisterKeyboard(KeyboardCallback cb) {
    spinlock_lock(&g_cbLock);
    for (uint8_t i = 0; i < g_kbCbCount; i++) {
        if (g_kbCallbacks[i] == cb) {
            g_kbCallbacks[i] = g_kbCallbacks[--g_kbCbCount];
            break;
        }
    }
    spinlock_unlock(&g_cbLock);
}
void RegisterMouse(MouseCallback cb) {
    spinlock_lock(&g_cbLock);
    if (g_msCbCount < 8) g_msCallbacks[g_msCbCount++] = cb;
    spinlock_unlock(&g_cbLock);
}
void UnregisterMouse(MouseCallback cb) {
    spinlock_lock(&g_cbLock);
    for (uint8_t i = 0; i < g_msCbCount; i++) {
        if (g_msCallbacks[i] == cb) {
            g_msCallbacks[i] = g_msCallbacks[--g_msCbCount];
            break;
        }
    }
    spinlock_unlock(&g_cbLock);
}

static void interruptInCallback(uint8_t* data, uint32_t len, void* ctx) {
    Device* dev = (Device*)ctx;
    Interface* ifce = (Interface*)dev->driverCtx;
    
    // 处理带 Report ID 的情况
    uint8_t* reportData = data;
    if (len > sizeof(HIDReport) && data[0] != 0) {
        reportData = data + 1; // 偏移掉 Report ID
        len--;
    }

    spinlock_lock(&g_cbLock);
    if (ifce->desc.bInterfaceProtocol == 1) { // Keyboard
        if (len >= sizeof(HIDReport)) {
            HIDReport* r = (HIDReport*)reportData;
            for (uint8_t i = 0; i < g_kbCbCount; i++) g_kbCallbacks[i](*r);
        }
    } else if (ifce->desc.bInterfaceProtocol == 2) { // Mouse
        if (len >= sizeof(MouseReport)) {
            MouseReport* r = (MouseReport*)reportData;
            for (uint8_t i = 0; i < g_msCbCount; i++) g_msCallbacks[i](*r);
        }
    }
    spinlock_unlock(&g_cbLock);
    
    for (uint8_t i = 0; i < ifce->numEndpoints; i++) {
        Endpoint& e = ifce->endpoints[i];
        if (e.type == EP_TYPE::INT_IN && e.dir == EP_DIR::IN) {
            XHCI::StartAsyncInterrupt(dev->slotID, e.address, data, e.maxPacketSize, interruptInCallback, dev);
            break;
        }
    }
}

void Init(Device* dev, Interface* ifce) {
    dev->driverCtx = ifce;
    if (ifce->desc.bInterfaceSubClass == 0x01) {
        if (!ControlTransfer(dev->slotID, 0, USB_REQ_DIR_OUT | USB_REQ_TYPE_CLS | USB_REQ_RCPT_IF, HID_SET_PROTOCOL, 0, ifce->desc.bInterfaceNumber, nullptr, 0)) {
            kprintf("[HID] SET_PROTOCOL failed on slot %u\n", dev->slotID);
        }
    }
    if (!ControlTransfer(dev->slotID, 0, USB_REQ_DIR_OUT | USB_REQ_TYPE_CLS | USB_REQ_RCPT_IF, HID_SET_IDLE, 0, ifce->desc.bInterfaceNumber, nullptr, 0)) {
        kprintf("[HID] SET_IDLE failed on slot %u\n", dev->slotID);
    }

    for (uint8_t i = 0; i < ifce->numEndpoints; i++) {
        Endpoint& e = ifce->endpoints[i];
        if (e.type == EP_TYPE::INT_IN && e.dir == EP_DIR::IN) {
            uint8_t* buf = (uint8_t*)kmalloc(e.maxPacketSize);
            XHCI::StartAsyncInterrupt(dev->slotID, e.address, buf, e.maxPacketSize, interruptInCallback, dev);
            break;
        }
    }
}

} // namespace USB::HID