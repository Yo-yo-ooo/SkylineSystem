//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
// usb/msc.cpp
#include <drivers/usb/msc.h>
#include <klib/kio.h>
#include <klib/klib.h>
#include <mem/heap.h>
#include <klib/cstr.h>

namespace USB::MSC {

static bool sendCBW(Device* msc, uint8_t lun, uint8_t flags, uint32_t dataLen, const uint8_t cb[16], uint8_t cbLen) {
    CBW cbw = {};
    cbw.signature          = 0x43425355;
    cbw.tag                = __atomic_fetch_add(&msc->tagCounter, 1, __ATOMIC_SEQ_CST); // 动态生成 Tag
    cbw.dataTransferLength = dataLen;
    cbw.flags              = flags;
    cbw.lun                = lun;
    cbw.cbLength           = cbLen; // 动态 CB 长度
    __memcpy(cbw.cb, cb, cbLen);
    return USB::BulkTransfer(msc->usbDev->slotID, msc->bulkOutEp, &cbw, sizeof(cbw), false);
}

static bool readCSW(Device* msc, CSW* csw, uint32_t expectedTag) {
    if (!USB::BulkTransfer(msc->usbDev->slotID, msc->bulkInEp, csw, sizeof(CSW), true)) return false;
    // 校验 Tag
    if (csw->tag != expectedTag) {
        kprintf("[MSC] CSW Tag mismatch! Expected %u, got %u\n", expectedTag, csw->tag);
        return false;
    }
    return csw->status == 0;
}

// Bulk-Only 错误恢复
static bool resetRecovery(Device* msc) {
    if (!ControlTransfer(msc->usbDev->slotID, 0, USB_REQ_DIR_OUT | USB_REQ_TYPE_CLS | USB_REQ_RCPT_IF, MSC_BBB_RESET, 0, msc->iface->desc.bInterfaceNumber, nullptr, 0)) return false;
    // 清空 Bulk IN/OUT 端点停摆状态 (需要调用 xHCI Reset Endpoint)
    // XHCI::ResetEndpoint(msc->usbDev->slotID, msc->bulkInEp);
    // XHCI::ResetEndpoint(msc->usbDev->slotID, msc->bulkOutEp);
    return true;
}

bool ReadBlock(Device* msc, uint32_t lba, void* buf) {
    uint8_t cb[16] = {0};
    cb[0] = 0x28; cb[2] = (lba >> 24) & 0xFF; cb[3] = (lba >> 16) & 0xFF; cb[4] = (lba >> 8) & 0xFF; cb[5] = lba & 0xFF; cb[8] = 1;
    
    uint32_t tag = msc->tagCounter + 1;
    if (!sendCBW(msc, 0, 0x80, msc->blockSize, cb, 10)) return false; // READ(10) 长度为 10
    if (!USB::BulkTransfer(msc->usbDev->slotID, msc->bulkInEp, buf, msc->blockSize, true)) {
        resetRecovery(msc);
        return false;
    }
    CSW csw; 
    if (!readCSW(msc, &csw, tag)) {
        resetRecovery(msc);
        return false;
    }
    return true;
}

bool WriteBlock(Device* msc, uint32_t lba, const void* buf) {
    uint8_t cb[16] = {0};
    cb[0] = 0x2A; cb[2] = (lba >> 24) & 0xFF; cb[3] = (lba >> 16) & 0xFF; cb[4] = (lba >> 8) & 0xFF; cb[5] = lba & 0xFF; cb[8] = 1;
    
    uint32_t tag = msc->tagCounter + 1;
    if (!sendCBW(msc, 0, 0x00, msc->blockSize, cb, 10)) return false; // WRITE(10) 长度为 10
    if (!USB::BulkTransfer(msc->usbDev->slotID, msc->bulkOutEp, (void*)buf, msc->blockSize, false)) {
        resetRecovery(msc);
        return false;
    }
    CSW csw; 
    if (!readCSW(msc, &csw, tag)) {
        resetRecovery(msc);
        return false;
    }
    return true;
}

// 动态获取容量和块大小
PACK(struct ReadCapacityResp {
    uint32_t lastLBA;
    uint32_t blockSize;
});

void Init(USB::Device* dev, Interface* ifce) {
    if (ifce->desc.bInterfaceProtocol != 0x50) return;
    Device* msc = (Device*)kmalloc(sizeof(Device));
    _memset(msc, 0, sizeof(*msc));
    msc->usbDev = dev; msc->iface = ifce; msc->blockSize = 512; // 默认 512
    
    for (uint8_t i = 0; i < ifce->numEndpoints; i++) {
        Endpoint& e = ifce->endpoints[i];
        if (e.type == EP_TYPE::BULK_IN)  { msc->bulkInEp  = e.address; msc->bulkInMPS  = e.maxPacketSize; }
        if (e.type == EP_TYPE::BULK_OUT) { msc->bulkOutEp = e.address; msc->bulkOutMPS = e.maxPacketSize; }
    }
    dev->driverCtx = msc;

    if (!ControlTransfer(dev->slotID, 0, USB_REQ_DIR_OUT | USB_REQ_TYPE_CLS | USB_REQ_RCPT_IF, MSC_BBB_RESET, 0, ifce->desc.bInterfaceNumber, nullptr, 0)) return;
    
    uint8_t lun = 0;
    ControlTransfer(dev->slotID, 0, USB_REQ_DIR_IN | USB_REQ_TYPE_CLS | USB_REQ_RCPT_IF, MSC_BBB_GET_MAX_LUN, 0, ifce->desc.bInterfaceNumber, &lun, 1);
    msc->maxLUN = lun;

    // 读取容量
    uint8_t cb[16] = {0}; cb[0] = 0x25; // READ CAPACITY
    ReadCapacityResp resp = {};
    uint32_t tag = msc->tagCounter + 1;
    if (sendCBW(msc, 0, 0x80, sizeof(resp), cb, 10)) {
        if (USB::BulkTransfer(dev->slotID, msc->bulkInEp, &resp, sizeof(resp), true)) {
            CSW csw;
            if (readCSW(msc, &csw, tag)) {
                msc->numBlocks = __builtin_bswap32(resp.lastLBA) + 1;
                msc->blockSize = __builtin_bswap32(resp.blockSize);
                kprintf("[MSC] Block size: %u, Blocks: %llu\n", msc->blockSize, msc->numBlocks);
            }
        }
    }
}

} // namespace USB::MSC