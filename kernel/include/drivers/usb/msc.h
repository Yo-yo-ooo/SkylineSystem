//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <drivers/usb/usb_device.h>
#include <pdef.h>

namespace USB::MSC {

PACK(struct CBW {
    uint32_t signature; uint32_t tag; uint32_t dataTransferLength;
    uint8_t flags; uint8_t lun; uint8_t cbLength; uint8_t cb[16];
});

PACK(struct CSW {
    uint32_t signature; uint32_t tag; uint32_t dataResidue; uint8_t status;
});

struct Device {
    USB::Device* usbDev; Interface* iface;
    uint8_t bulkInEp; uint8_t bulkOutEp;
    uint16_t bulkInMPS; uint16_t bulkOutMPS;
    uint32_t maxLUN; uint64_t numBlocks;
    uint32_t blockSize; // 动态块大小
    volatile uint32_t tagCounter; // 动态 Tag
};

void Init(USB::Device* dev, Interface* ifce);
bool ReadBlock (Device* msc, uint32_t lba, void* buf);
bool WriteBlock(Device* msc, uint32_t lba, const void* buf);

} // namespace USB::MSC