//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <drivers/usb/usb.h>
#include <drivers/usb/usb_descriptor.h>

namespace USB {

struct Endpoint {
    uint8_t address; EP_TYPE type; EP_DIR dir;
    uint16_t maxPacketSize; uint8_t interval; uint8_t slotID; bool configured;
};

struct Interface {
    InterfaceDescriptor desc;
    Endpoint endpoints[16];
    uint8_t numEndpoints;
};

struct Device {
    uint8_t slotID; uint8_t port; USB_SPEED speed;
    DeviceDescriptor desc;
    ConfigDescriptor cfg;   // 配置描述符头部
    void* cfgBuf;           // 完整配置描述符内存指针
    uint16_t cfgLen;        // 配置描述符总长度
    Interface interfaces[8];
    uint8_t numInterfaces;
    void* driverCtx;
    void (*onRemove)(Device*);
};

Device* CreateDevice(uint8_t slot, uint8_t port, USB_SPEED speed, DeviceDescriptor& dd, void* cfgBuf, uint16_t cfgLen);
void DestroyDevice(uint8_t slotID);

bool ControlTransfer(uint8_t slotID, uint8_t epAddr, uint8_t bmReqType, uint8_t bRequest, uint16_t wValue, uint16_t wIndex, void* buf, uint16_t len);
bool BulkTransfer (uint8_t slotID, uint8_t epAddr, void* buf, uint32_t len, bool inDir);
void RouteDeviceToClassDriver(Device* dev);

} // namespace USB