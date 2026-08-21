//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
#include <drivers/usb/hid.h>
#include <drivers/usb/xhci.h>
#include <klib/kio.h>

static inline uint64_t hid_irq_save() {
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    return flags;
}
static inline void hid_irq_restore(uint64_t flags) {
    __asm__ volatile("pushq %0; popfq" :: "r"(flags) : "memory");
}
#define HID_SPIN_LOCK_IRQSAVE(lock, flags) do { flags = hid_irq_save(); spinlock_lock(lock); } while(0)
#define HID_SPIN_UNLOCK_IRQRESTORE(lock, flags) do { spinlock_unlock(lock); hid_irq_restore(flags); } while(0)

namespace USB::HID {

static KeyboardCallback g_kbCallbacks[8];
static uint8_t g_kbCbCount = 0;
static MouseCallback g_msCallbacks[8];
static uint8_t g_msCbCount = 0;
static spinlock_t g_cbLock = 0;

void RegisterKeyboard(KeyboardCallback cb) {
    uint64_t flags;
    HID_SPIN_LOCK_IRQSAVE(&g_cbLock, flags);
    if (g_kbCbCount < 8) g_kbCallbacks[g_kbCbCount++] = cb;
    HID_SPIN_UNLOCK_IRQRESTORE(&g_cbLock, flags);
}
void UnregisterKeyboard(KeyboardCallback cb) {
    uint64_t flags;
    HID_SPIN_LOCK_IRQSAVE(&g_cbLock, flags);
    for (uint8_t i = 0; i < g_kbCbCount; i++) {
        if (g_kbCallbacks[i] == cb) {
            g_kbCallbacks[i] = g_kbCallbacks[--g_kbCbCount];
            break;
        }
    }
    HID_SPIN_UNLOCK_IRQRESTORE(&g_cbLock, flags);
}
void RegisterMouse(MouseCallback cb) {
    uint64_t flags;
    HID_SPIN_LOCK_IRQSAVE(&g_cbLock, flags);
    if (g_msCbCount < 8) g_msCallbacks[g_msCbCount++] = cb;
    HID_SPIN_UNLOCK_IRQRESTORE(&g_cbLock, flags);
}
void UnregisterMouse(MouseCallback cb) {
    uint64_t flags;
    HID_SPIN_LOCK_IRQSAVE(&g_cbLock, flags);
    for (uint8_t i = 0; i < g_msCbCount; i++) {
        if (g_msCallbacks[i] == cb) {
            g_msCallbacks[i] = g_msCallbacks[--g_msCbCount];
            break;
        }
    }
    HID_SPIN_UNLOCK_IRQRESTORE(&g_cbLock, flags);
}

static void interruptInCallback(uint8_t* data, uint32_t len, void* ctx) {
    Device* dev = (Device*)ctx;
    if (!dev->driverCtx) { return; }
    
    HIDCtx* hidCtx = (HIDCtx*)dev->driverCtx;
    Interface* ifce = hidCtx->ifce;
    
    uint8_t* reportData = data;
    if (len > sizeof(HIDReport) && data[0] != 0) {
        reportData = data + 1;
        len--;
    }

    uint64_t flags;
    HID_SPIN_LOCK_IRQSAVE(&g_cbLock, flags);
    if (ifce->desc.bInterfaceProtocol == 1) {
        if (len >= sizeof(HIDReport)) {
            HIDReport* r = (HIDReport*)reportData;
            for (uint8_t i = 0; i < g_kbCbCount; i++) g_kbCallbacks[i](*r);
        }
    } else if (ifce->desc.bInterfaceProtocol == 2) {
        if (len >= sizeof(MouseReport)) {
            MouseReport* r = (MouseReport*)reportData;
            for (uint8_t i = 0; i < g_msCbCount; i++) g_msCallbacks[i](*r);
        }
    }
    HID_SPIN_UNLOCK_IRQRESTORE(&g_cbLock, flags);
    
    for (uint8_t i = 0; i < ifce->numEndpoints; i++) {
        Endpoint& e = ifce->endpoints[i];
        if (e.type == EP_TYPE::INT_IN && e.dir == EP_DIR::IN) {
            XHCI::StartAsyncInterrupt(dev->slotID, e.address, data, e.maxPacketSize, interruptInCallback, dev);
            break;
        }
    }
}

void Init(Device* dev, Interface* ifce) {
    HIDCtx* ctx = (HIDCtx*)kmalloc(sizeof(HIDCtx));
    ctx->ifce = ifce;
    ctx->intBuf = nullptr;
    dev->driverCtx = ctx;

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
            ctx->intBuf = buf;
            XHCI::StartAsyncInterrupt(dev->slotID, e.address, buf, e.maxPacketSize, interruptInCallback, dev);
            break;
        }
    }
}

void Deinit(Device* dev) {
    HIDCtx* ctx = (HIDCtx*)dev->driverCtx;
    if (ctx) {
        if (ctx->intBuf) {
            kfree(ctx->intBuf);
            ctx->intBuf = nullptr;
        }
        kfree(ctx);
        dev->driverCtx = nullptr;
    }
}

} // namespace USB::HID