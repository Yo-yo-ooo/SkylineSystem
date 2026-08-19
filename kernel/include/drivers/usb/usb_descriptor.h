//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <stdint.h>
#include <pdef.h>

namespace USB {

PACK(struct DeviceDescriptor {
    uint8_t  bLength; uint8_t  bDescriptorType; uint16_t bcdUSB;
    uint8_t  bDeviceClass; uint8_t  bDeviceSubClass; uint8_t  bDeviceProtocol;
    uint8_t  bMaxPacketSize0; uint16_t idVendor; uint16_t idProduct;
    uint16_t bcdDevice; uint8_t  iManufacturer; uint8_t  iProduct;
    uint8_t  iSerialNumber; uint8_t  bNumConfigurations;
});

PACK(struct ConfigDescriptor {
    uint8_t  bLength; uint8_t  bDescriptorType; uint16_t wTotalLength;
    uint8_t  bNumInterfaces; uint8_t  bConfigurationValue; uint8_t  iConfiguration;
    uint8_t  bmAttributes; uint8_t  bMaxPower;
});

PACK(struct InterfaceDescriptor {
    uint8_t  bLength; uint8_t  bDescriptorType; uint8_t  bInterfaceNumber;
    uint8_t  bAlternateSetting; uint8_t  bNumEndpoints; uint8_t  bInterfaceClass;
    uint8_t  bInterfaceSubClass; uint8_t  bInterfaceProtocol; uint8_t  iInterface;
});

PACK(struct EndpointDescriptor {
    uint8_t  bLength; uint8_t  bDescriptorType; uint8_t  bEndpointAddress;
    uint8_t  bmAttributes; uint16_t wMaxPacketSize; uint8_t  bInterval;
});

} // namespace USB