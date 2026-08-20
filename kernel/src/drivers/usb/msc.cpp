//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
#include <drivers/usb/msc.h>
#include <klib/kio.h>
#include <klib/klib.h>
#include <mem/heap.h>
#include <klib/cstr.h>
#include <drivers/usb/xhci.h>

namespace USB::MSC {

static uint32_t sendCBW(Device* msc, uint8_t lun, uint8_t flags, uint32_t dataLen, const uint8_t cb[16], uint8_t cbLen) {
    CBW cbw = {};
    cbw.signature          = 0x43425355;
    cbw.tag                = __atomic_fetch_add(&msc->tagCounter, 1, __ATOMIC_SEQ_CST);
    cbw.dataTransferLength = dataLen;
    cbw.flags              = flags;
    cbw.lun                = lun;
    cbw.cbLength           = cbLen;
    __memcpy(cbw.cb, cb, cbLen);
    if (!USB::BulkTransfer(msc->usbDev->slotID, msc->bulkOutEp, &cbw, sizeof(cbw), false)) {
        return 0xFFFFFFFF; // 返回错误标记
    }
    return cbw.tag; // 返回实际使用的 Tag
}

static bool readCSW(Device* msc, CSW* csw, uint32_t expectedTag) {
    if (!USB::BulkTransfer(msc->usbDev->slotID, msc->bulkInEp, csw, sizeof(CSW), true)) return false;
    if (csw->tag != expectedTag) {
        kprintf("[MSC] CSW Tag mismatch! Expected %u, got %u\n", expectedTag, csw->tag);
        return false;
    }
    return csw->status == 0;
}

static bool resetRecovery(Device* msc) {
    if (!ControlTransfer(msc->usbDev->slotID, 0, USB_REQ_DIR_OUT | USB_REQ_TYPE_CLS | USB_REQ_RCPT_IF, MSC_BBB_RESET, 0, msc->iface->desc.bInterfaceNumber, nullptr, 0)) return false;
    // 完成端点复位和清零停摆状态
    XHCI::ResetEndpoint(msc->usbDev->slotID, msc->bulkInEp);
    XHCI::ResetEndpoint(msc->usbDev->slotID, msc->bulkOutEp);
    return true;
}

bool ReadBlock(Device* msc, uint32_t lba, void* buf) {
    uint8_t cb[16] = {0};
    cb[0] = 0x28; cb[2] = (lba >> 24) & 0xFF; cb[3] = (lba >> 16) & 0xFF; cb[4] = (lba >> 8) & 0xFF; cb[5] = lba & 0xFF; cb[8] = 1;
    
    uint32_t tag = sendCBW(msc, 0, 0x80, msc->blockSize, cb, 10);
    if (tag == 0xFFFFFFFF) return false;
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
    
    uint32_t tag = sendCBW(msc, 0, 0x00, msc->blockSize, cb, 10);
    if (tag == 0xFFFFFFFF) return false;
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

PACK(struct ReadCapacityResp {
    uint32_t lastLBA;
    uint32_t blockSize;
});

void Init(USB::Device* dev, Interface* ifce) {
    if (ifce->desc.bInterfaceProtocol != 0x50) return;
    Device* msc = (Device*)kmalloc(sizeof(Device));
    _memset(msc, 0, sizeof(*msc));
    msc->usbDev = dev; msc->iface = ifce; msc->blockSize = 512;
    
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

    uint8_t cb[16] = {0}; cb[0] = 0x25;
    ReadCapacityResp resp = {};
    uint32_t tag = sendCBW(msc, 0, 0x80, sizeof(resp), cb, 10);
    if (tag != 0xFFFFFFFF) {
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

void Deinit(USB::Device* dev) {
    Device* msc = (Device*)dev->driverCtx;
    if (msc) {
        kfree(msc);
        dev->driverCtx = nullptr;
    }
}

} // namespace USB::MSC