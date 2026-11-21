//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
#include <drivers/usb/uac.h>
#include <drivers/usb/xhci.h>
#include <klib/kio.h>

#ifdef __x86_64__
#include <arch/x86_64/pit/pit.h>
extern void usleep_usec(uint64_t x);
#endif

namespace USB::UAC {

void RegisterAudioCallback(Device* uac, AudioSampleCallback cb) { uac->callback = cb; }

static void isochCallback(uint8_t* data, uint32_t len, void* ctx) {
    Device* uac = (Device*)ctx;
    if (!uac->usbDev || uac->stopping) { return; }

    if (uac->callback && data && uac->activeStream < uac->numStreams) {
        uint32_t numFrames = len / (uac->streams[uac->activeStream].numChannels * (uac->streams[uac->activeStream].bitsPerSample / 8));
        uac->callback((int16_t*)data, numFrames, uac->streams[uac->activeStream].numChannels, uac);
    }
    USB::Device* dev = uac->usbDev;
    uint8_t ep = uac->streams[uac->activeStream].endpointAddr;
    uint16_t mps = uac->streams[uac->activeStream].endpointMPS;
    XHCI::StartAsyncIsoch(dev->slotID, ep, data, mps, (ep & 0x80) != 0, isochCallback, uac);
}

void Init(USB::Device* dev, Interface* ifce) {
    Device* uac = (Device*)kmalloc(sizeof(Device));
    _memset(uac, 0, sizeof(*uac));
    uac->usbDev = dev; uac->streamIfce = ifce; dev->driverCtx = uac;
    uac->stopping = true; // 初始处于停止状态

    uint8_t* p = (uint8_t*)dev->cfgBuf;
    uint8_t* end = p + dev->cfgLen;
    uint8_t targetIf = ifce->desc.bInterfaceNumber;

    while (p + 2 <= end) {
        uint8_t len = p[0]; uint8_t type = p[1];
        if (len < 2 || p + len > end) break;
        if (type == DT_INTERFACE) {
            InterfaceDescriptor* ifd = (InterfaceDescriptor*)p;
            if (ifd->bInterfaceClass == CC_AUDIO && ifd->bInterfaceSubClass == 0x02 && 
                ifd->bInterfaceNumber == targetIf && uac->numStreams < 8) {
                
                int stream_idx = uac->numStreams;
                uac->streams[stream_idx].altSetting = ifd->bAlternateSetting;
                
                uint8_t* ep_ptr = p + len;
                while(ep_ptr + 2 <= end) {
                    uint8_t ep_len = ep_ptr[0]; uint8_t ep_type = ep_ptr[1];
                    if (ep_len < 2 || ep_ptr + ep_len > end) break;
                    if (ep_type == DT_INTERFACE) break;
                    
                    if (ep_type == 0x24 && ep_len >= 4) {
                        uint8_t subtype = ep_ptr[2];
                        if (subtype == 0x02 && ep_len >= 8) {
                            uac->streams[stream_idx].numChannels = ep_ptr[4];
                            uac->streams[stream_idx].bitsPerSample = ep_ptr[6];
                            if (ep_ptr[7] == 1 && ep_len >= 11) {
                                uac->streams[stream_idx].sampleRate = (ep_ptr[8] << 16) | (ep_ptr[9] << 8) | ep_ptr[10];
                            }
                        }
                    } else if (ep_type == DT_ENDPOINT) {
                        EndpointDescriptor* epd = (EndpointDescriptor*)ep_ptr;
                        if ((epd->bmAttributes & 0x3) == 1) {
                            uac->streams[stream_idx].endpointAddr = epd->bEndpointAddress;
                            uac->streams[stream_idx].endpointMPS = epd->wMaxPacketSize & 0x7FF;
                            uac->streams[stream_idx].endpointInterval = epd->bInterval;
                        }
                    }
                    ep_ptr += ep_len;
                }
                if (uac->streams[stream_idx].endpointAddr != 0) {
                    uac->numStreams++;
                } else {
                    uac->streams[stream_idx].altSetting = 0;
                }
            }
        }
        p += len;
    }
}

void Deinit(USB::Device* dev) {
    Device* uac = (Device*)dev->driverCtx;
    if (uac) {
        uac->stopping = true;
        // 不调用 StopStream，直接释放内存
        if (uac->frameBuf) { kfree(uac->frameBuf); uac->frameBuf = nullptr; }
        kfree(uac);
        dev->driverCtx = nullptr;
    }
}

bool StartStream(Device* uac, uint8_t altSetting) {
    int idx = -1;
    for (uint8_t i = 0; i < uac->numStreams; i++) {
        if (uac->streams[i].altSetting == altSetting) { idx = i; break; }
    }
    if (idx < 0) return false;
    
    uac->stopping = false; // 恢复回调执行
    uac->activeStream = idx;

    if (!USB::ControlTransfer(uac->usbDev->slotID, 0, USB_REQ_DIR_OUT | USB_REQ_TYPE_STD | USB_REQ_RCPT_IF, USB::SET_INTERFACE, altSetting, uac->streamIfce->desc.bInterfaceNumber, nullptr, 0)) {
        return false;
    }

    uint8_t ep = uac->streams[idx].endpointAddr;
    uint16_t mps = uac->streams[idx].endpointMPS;
    uac->frameBuf = kmalloc(mps);
    XHCI::StartAsyncIsoch(uac->usbDev->slotID, ep, uac->frameBuf, mps, (ep & 0x80) != 0, isochCallback, uac);
    return true;
}

bool StopStream(Device* uac) {
    uac->stopping = true; // 先阻止回调访问缓冲区及重新投递
    if (uac->activeStream < uac->numStreams) {
        USB::ControlTransfer(uac->usbDev->slotID, 0, USB_REQ_DIR_OUT | USB_REQ_TYPE_STD | USB_REQ_RCPT_IF, USB::SET_INTERFACE, 0, uac->streamIfce->desc.bInterfaceNumber, nullptr, 0);
    }
    // 等待硬件排干在途 TRB，防止释放内存后硬件继续 DMA 写入
    usleep_usec(20000); 
    if (uac->frameBuf) { kfree(uac->frameBuf); uac->frameBuf = nullptr; }
    return true;
}

} // namespace USB::UAC