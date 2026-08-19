//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
#include <drivers/usb/usb_device.h>
#include <drivers/usb/xhci.h>
#include <klib/algorithm/rbtree.h>
#include <klib/kio.h>
#include <klib/klib.h>
#include <drivers/usb/hid.h>
#include <drivers/usb/msc.h>
#include <drivers/usb/uac.h>
#include <drivers/usb/uvc.h>

namespace USB {

struct USBDeviceEntry {
    rb_node_t node;
    uint64_t key;
    USB::Device* dev;
};

static rb_root_t g_usbDevices;
static spinlock_t g_usbDevicesLock = 0;

static int usb_dev_cmp(const rb_node_t *a, const rb_node_t *b) {
    USBDeviceEntry *ea = container_of(a, USBDeviceEntry, node);
    USBDeviceEntry *eb = container_of(b, USBDeviceEntry, node);
    if (ea->key < eb->key) return -1;
    if (ea->key > eb->key) return 1;
    return 0;
}

Device* CreateDevice(uint8_t slot, uint8_t port, USB_SPEED speed, DeviceDescriptor& dd, void* cfgBuf, uint16_t cfgLen) {
    Device* d = (Device*)kmalloc(sizeof(Device));
    _memset(d, 0, sizeof(*d));
    d->slotID = slot; d->port = port; d->speed = speed; d->desc = dd;
    
    // 完整拷贝配置描述符
    d->cfgBuf = kmalloc(cfgLen);
    __memcpy(d->cfgBuf, cfgBuf, cfgLen);
    d->cfgLen = cfgLen;
    __memcpy(&d->cfg, d->cfgBuf, sizeof(ConfigDescriptor));

    uint8_t* p = (uint8_t*)d->cfgBuf; uint8_t* end = p + cfgLen; p += d->cfg.bLength;
    uint8_t curIf = 0xFF;
    while (p + 2 <= end) {
        uint8_t len = p[0]; uint8_t type = p[1];
        if (len < 2 || p + len > end) break;
        if (type == DT_INTERFACE) {
            InterfaceDescriptor* ifd = (InterfaceDescriptor*)p;
            curIf = ifd->bInterfaceNumber;
            if (curIf < 8) {
                __memcpy(&d->interfaces[curIf].desc, ifd, sizeof(*ifd));
                d->interfaces[curIf].numEndpoints = 0;
                if (curIf + 1 > d->numInterfaces) d->numInterfaces = curIf + 1;
            }
        } else if (type == DT_ENDPOINT) {
            EndpointDescriptor* epd = (EndpointDescriptor*)p;
            if (curIf < 8 && d->interfaces[curIf].numEndpoints < 16) {
                Endpoint& e = d->interfaces[curIf].endpoints[d->interfaces[curIf].numEndpoints++];
                e.address = epd->bEndpointAddress; e.maxPacketSize = epd->wMaxPacketSize & 0x7FF; e.interval = epd->bInterval;
                e.dir = (epd->bEndpointAddress & 0x80) ? EP_DIR::IN : EP_DIR::OUT;
                e.type = (EP_TYPE)((epd->bmAttributes & 0x3) | ((uint8_t)e.dir << 2));
                e.slotID = slot;
            }
        }
        p += len;
    }

    USBDeviceEntry* entry = (USBDeviceEntry*)kmalloc(sizeof(USBDeviceEntry));
    rb_init_node(&entry->node);
    entry->key = slot; entry->dev = d;
    
    spinlock_lock(&g_usbDevicesLock);
    rb_insert(&g_usbDevices, &entry->node, usb_dev_cmp);
    spinlock_unlock(&g_usbDevicesLock);
    
    return d;
}

void DestroyDevice(uint8_t slotID) {
    spinlock_lock(&g_usbDevicesLock);
    
    USBDeviceEntry key_entry; key_entry.key = slotID;
    rb_node_t* found = rb_search(&g_usbDevices, &key_entry.node, usb_dev_cmp);
    if (found) {
        USBDeviceEntry* entry = container_of(found, USBDeviceEntry, node);
        Device* dev = entry->dev;
        
        // 调用驱动的卸载钩子
        if (dev->driverCtx) {
            // 在实际实现中，这里应根据接口类调用对应的 Destroy 函数
            // 此处简单释放框架资源
        }
        
        rb_erase(&g_usbDevices, &entry->node);
        kfree(entry);
        
        if (dev->cfgBuf) kfree(dev->cfgBuf);
        kfree(dev);
    }
    
    spinlock_unlock(&g_usbDevicesLock);
}

void RouteDeviceToClassDriver(Device* dev) {
    for (uint8_t i = 0; i < dev->numInterfaces; i++) {
        Interface& ifce = dev->interfaces[i];
        for (uint8_t j = 0; j < ifce.numEndpoints; j++) {
            Endpoint& e = ifce.endpoints[j];
            if (e.type != EP_TYPE::CONTROL_IN && e.type != EP_TYPE::CONTROL_OUT) {
                if (!XHCI::ConfigureEndpoint(dev->slotID, e.address, e.type, e.maxPacketSize, e.interval)) {
                    kprintf("[USB] Failed to configure EP 0x%02x for slot %u\n", e.address, dev->slotID);
                    continue; // 失败回滚逻辑可在此扩展
                }
                e.configured = true;
            }
        }
        switch (ifce.desc.bInterfaceClass) {
            case CC_HID: HID::Init(dev, &ifce); break;
            case CC_MSC: MSC::Init(dev, &ifce); break;
            case CC_AUDIO: UAC::Init(dev, &ifce); break;
            case CC_Video: UVC::Init(dev, &ifce); break;
        }
    }
}

bool ControlTransfer(uint8_t slotID, uint8_t epAddr, uint8_t bmReqType, uint8_t bRequest, uint16_t wValue, uint16_t wIndex, void* buf, uint16_t len) {
    SetupPacket sp = { bmReqType, bRequest, wValue, wIndex, len };
    return XHCI::SubmitControlTransfer(slotID, &sp, buf, len, (bmReqType & USB_REQ_DIR_IN) != 0);
}

bool BulkTransfer(uint8_t slotID, uint8_t epAddr, void* buf, uint32_t len, bool inDir) {
    return XHCI::SubmitNormalTransfer(slotID, epAddr, buf, len, inDir, false);
}

} // namespace USB