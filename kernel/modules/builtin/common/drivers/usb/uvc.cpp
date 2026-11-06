//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
#include <drivers/usb/uvc.h>
#include <drivers/usb/xhci.h>
#include <klib/kio.h>
#include <pdef.h>

#ifdef __x86_64__
#include <arch/x86_64/pit/pit.h>
extern void usleep_usec(uint64_t x);
#endif

namespace USB::UVC {

static FrameCallback g_frameCb = nullptr;
void RegisterFrameCallback(FrameCallback cb) { g_frameCb = cb; }

PACK(struct UVCPayloadHeader {
    uint8_t headerLength;
    uint8_t flags;
    uint8_t data[10];
});

static void isochCallback(uint8_t* data, uint32_t len, void* ctx) {
    Device* uvc = (Device*)ctx;
    if (!uvc->usbDev || uvc->stopping) { return; }

    if (data && len >= sizeof(UVCPayloadHeader)) {
        UVCPayloadHeader* hdr = (UVCPayloadHeader*)data;
        uint8_t fid = hdr->flags & 0x01;
        bool eof = (hdr->flags & 0x02) != 0;
        bool err = (hdr->flags & 0x40) != 0;

        if (g_frameCb) {
            uint32_t payloadLen = len - hdr->headerLength;
            g_frameCb(data + hdr->headerLength, payloadLen, !err && (hdr->headerLength >= 2), fid, uvc);
        }

        if (fid != uvc->fidPrev) {
            uvc->frameOffset = 0;
            uvc->fidPrev = fid;
        }
        if (uvc->frameCapacity) {
            uint32_t payloadLen = len - hdr->headerLength;
            if (uvc->frameOffset + payloadLen <= uvc->frameCapacity) {
                __memcpy(uvc->frameBuf + uvc->frameOffset, data + hdr->headerLength, payloadLen);
                uvc->frameOffset += payloadLen;
            }
            if (eof) {
                if (g_frameCb) g_frameCb(uvc->frameBuf, uvc->frameOffset, true, fid, uvc);
                uvc->frameOffset = 0;
            }
        }
    }

    USB::Device* dev = uvc->usbDev;
    uint8_t ep = uvc->streams[uvc->activeStream].endpointAddr;
    uint16_t mps = uvc->streams[uvc->activeStream].endpointMPS;
    XHCI::StartAsyncIsoch(dev->slotID, ep, data, mps, (ep & 0x80) != 0, isochCallback, uvc);
}

void Init(USB::Device* dev, Interface* ifce) {
    Device* uvc = (Device*)kmalloc(sizeof(Device));
    _memset(uvc, 0, sizeof(*uvc));
    uvc->usbDev = dev; uvc->vsIfce = ifce; dev->driverCtx = uvc;
    uvc->fidPrev = 0xFF;
    uvc->stopping = true;

    uint8_t* p = (uint8_t*)dev->cfgBuf;
    uint8_t* end = p + dev->cfgLen;
    uint8_t targetIf = ifce->desc.bInterfaceNumber;
    uint8_t curFormatIndex = 0;

    while (p + 2 <= end) {
        uint8_t len = p[0]; uint8_t type = p[1];
        if (len < 2 || p + len > end) break;
        if (type == DT_INTERFACE) {
            InterfaceDescriptor* ifd = (InterfaceDescriptor*)p;
            if (ifd->bInterfaceClass == CC_Video && ifd->bInterfaceSubClass == 0x02 && 
                ifd->bInterfaceNumber == targetIf) {
                
                if (ifd->bAlternateSetting == 0) {
                    uint8_t* ep_ptr = p + len;
                    while(ep_ptr + 2 <= end) {
                        uint8_t ep_len = ep_ptr[0]; uint8_t ep_type = ep_ptr[1];
                        if (ep_len < 2 || ep_ptr + ep_len > end) break;
                        if (ep_type == DT_INTERFACE) break;
                        
                        if (ep_type == 0x24 && ep_len >= 3) {
                            uint8_t subtype = ep_ptr[2];
                            if ((subtype == 0x04 || subtype == 0x06) && ep_len >= 5) {
                                curFormatIndex = ep_ptr[3];
                            } else if ((subtype == 0x05 || subtype == 0x07) && ep_len >= 26) {
                                if (uvc->numStreams < 8 && uvc->streams[0].formatIndex == 0) {
                                    uvc->streams[0].formatIndex = curFormatIndex;
                                    uvc->streams[0].frameIndex = ep_ptr[3];
                                    uvc->streams[0].width = *(uint16_t*)&ep_ptr[5];
                                    uvc->streams[0].height = *(uint16_t*)&ep_ptr[7];
                                    uvc->streams[0].frameInterval = *(uint32_t*)&ep_ptr[25];
                                }
                            }
                        }
                        ep_ptr += ep_len;
                    }
                } else if (ifd->bAlternateSetting > 0 && uvc->numStreams < 8) {
                    int stream_idx = uvc->numStreams;
                    uvc->streams[stream_idx].altSetting = ifd->bAlternateSetting;
                    uvc->streams[stream_idx].formatIndex = uvc->streams[0].formatIndex;
                    uvc->streams[stream_idx].frameIndex = uvc->streams[0].frameIndex;
                    uvc->streams[stream_idx].width = uvc->streams[0].width;
                    uvc->streams[stream_idx].height = uvc->streams[0].height;
                    uvc->streams[stream_idx].frameInterval = uvc->streams[0].frameInterval;

                    uint8_t* ep_ptr = p + len;
                    while(ep_ptr + 2 <= end) {
                        uint8_t ep_len = ep_ptr[0]; uint8_t ep_type = ep_ptr[1];
                        if (ep_len < 2 || ep_ptr + ep_len > end) break;
                        if (ep_type == DT_INTERFACE) break;
                        
                        if (ep_type == DT_ENDPOINT) {
                            EndpointDescriptor* epd = (EndpointDescriptor*)ep_ptr;
                            if ((epd->bmAttributes & 0x3) == 1) {
                                uvc->streams[stream_idx].endpointAddr = epd->bEndpointAddress;
                                uvc->streams[stream_idx].endpointMPS = epd->wMaxPacketSize & 0x7FF;
                                uvc->streams[stream_idx].endpointInterval = epd->bInterval;
                                uvc->streams[stream_idx].maxPayloadSize = epd->wMaxPacketSize & 0x7FF;
                                break;
                            }
                        }
                        ep_ptr += ep_len;
                    }
                    uvc->numStreams++;
                }
            }
        }
        p += len;
    }
}

void Deinit(USB::Device* dev) {
    Device* uvc = (Device*)dev->driverCtx;
    if (uvc) {
        uvc->stopping = true;
        if (uvc->frameBuf) { kfree(uvc->frameBuf); uvc->frameBuf = nullptr; uvc->frameCapacity = 0; }
        if (uvc->isochBuf) { kfree(uvc->isochBuf); uvc->isochBuf = nullptr; }
        kfree(uvc);
        dev->driverCtx = nullptr;
    }
}

bool StartStream(Device* uvc, uint8_t altSetting) {
    int idx = -1;
    for (uint8_t i = 0; i < uvc->numStreams; i++) if (uvc->streams[i].altSetting == altSetting) { idx = i; break; }
    if (idx < 0) return false;
    
    uvc->stopping = false;
    uvc->activeStream = idx;

    uint8_t probe[34] = {};
    probe[2] = uvc->streams[idx].formatIndex;
    probe[3] = uvc->streams[idx].frameIndex;
    *(uint32_t*)&probe[4] = uvc->streams[idx].frameInterval;
    
    if (!USB::ControlTransfer(uvc->usbDev->slotID, 0, USB_REQ_DIR_OUT | USB_REQ_TYPE_CLS | USB_REQ_RCPT_IF, 0x01, 0x0100, uvc->vsIfce->desc.bInterfaceNumber, probe, 34)) return false;
    if (!USB::ControlTransfer(uvc->usbDev->slotID, 0, USB_REQ_DIR_IN | USB_REQ_TYPE_CLS | USB_REQ_RCPT_IF, 0x81, 0x0100, uvc->vsIfce->desc.bInterfaceNumber, probe, 34)) return false;
    if (!USB::ControlTransfer(uvc->usbDev->slotID, 0, USB_REQ_DIR_OUT | USB_REQ_TYPE_CLS | USB_REQ_RCPT_IF, 0x01, 0x0200, uvc->vsIfce->desc.bInterfaceNumber, probe, 34)) return false;
    if (!USB::ControlTransfer(uvc->usbDev->slotID, 0, USB_REQ_DIR_OUT | USB_REQ_TYPE_STD | USB_REQ_RCPT_IF, USB::SET_INTERFACE, altSetting, uvc->vsIfce->desc.bInterfaceNumber, nullptr, 0)) return false;

    uvc->frameCapacity = uvc->streams[idx].maxPayloadSize * 64;
    uvc->frameBuf = (uint8_t*)kmalloc(uvc->frameCapacity);
    if (!uvc->frameBuf) {
        USB::ControlTransfer(uvc->usbDev->slotID, 0, USB_REQ_DIR_OUT | USB_REQ_TYPE_STD | USB_REQ_RCPT_IF, USB::SET_INTERFACE, 0, uvc->vsIfce->desc.bInterfaceNumber, nullptr, 0);
        return false;
    }
    
    uvc->frameOffset = 0; uvc->fidPrev = 0xFF;

    uint8_t ep = uvc->streams[idx].endpointAddr;
    uint16_t mps = uvc->streams[idx].endpointMPS;
    uint8_t* buf = (uint8_t*)kmalloc(mps);
    if (!buf) {
        kfree(uvc->frameBuf);
        uvc->frameBuf = nullptr;
        USB::ControlTransfer(uvc->usbDev->slotID, 0, USB_REQ_DIR_OUT | USB_REQ_TYPE_STD | USB_REQ_RCPT_IF, USB::SET_INTERFACE, 0, uvc->vsIfce->desc.bInterfaceNumber, nullptr, 0);
        return false;
    }
    
    uvc->isochBuf = buf;
    XHCI::StartAsyncIsoch(uvc->usbDev->slotID, ep, buf, mps, (ep & 0x80) != 0, isochCallback, uvc);
    return true;
}

bool StopStream(Device* uvc) {
    uvc->stopping = true;
    if (uvc->activeStream < uvc->numStreams) {
        USB::ControlTransfer(uvc->usbDev->slotID, 0, USB_REQ_DIR_OUT | USB_REQ_TYPE_STD | USB_REQ_RCPT_IF, USB::SET_INTERFACE, 0, uvc->vsIfce->desc.bInterfaceNumber, nullptr, 0);
    }
    usleep_usec(20000);
    if (uvc->frameBuf) { kfree(uvc->frameBuf); uvc->frameBuf = nullptr; uvc->frameCapacity = 0; }
    if (uvc->isochBuf) { kfree(uvc->isochBuf); uvc->isochBuf = nullptr; }
    return true;
}

} // namespace USB::UVC