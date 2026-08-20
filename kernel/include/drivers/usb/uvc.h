//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <drivers/usb/usb_device.h>

namespace USB::UVC {

struct VideoStream {
    uint8_t altSetting; uint8_t endpointAddr; uint16_t endpointMPS; uint8_t endpointInterval;
    uint16_t width; uint16_t height; uint32_t frameInterval;
    uint32_t maxPayloadSize; uint8_t formatIndex; uint8_t frameIndex;
};

struct Device {
    USB::Device* usbDev; Interface* vsIfce;
    VideoStream streams[8];
    uint8_t numStreams; uint8_t activeStream;
    uint8_t* frameBuf;
    uint32_t frameOffset;
    uint32_t frameCapacity;
    uint8_t fidPrev;
};

void Init(USB::Device* dev, Interface* ifce);
void Deinit(USB::Device* dev); // 新增
bool StartStream(Device* uvc, uint8_t altSetting);
bool StopStream (Device* uvc);

using FrameCallback = void(*)(uint8_t* payload, uint32_t length, bool headerValid, uint8_t fid, void* ctx);
void RegisterFrameCallback(FrameCallback cb);

} // namespace USB::UVC