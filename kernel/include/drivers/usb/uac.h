//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <drivers/usb/usb_device.h>

namespace USB::UAC {

struct AudioStreamingInterface {
    uint8_t altSetting; uint16_t formatTag; uint8_t numChannels;
    uint8_t bitsPerSample; uint32_t sampleRate;
    uint8_t endpointAddr; uint16_t endpointMPS; uint8_t endpointInterval;
};
using AudioSampleCallback = void(*)(int16_t* samples, uint32_t numFrames, uint8_t numChannels, void* ctx);
struct Device {
    USB::Device* usbDev; Interface* streamIfce;
    AudioStreamingInterface streams[8];
    uint8_t numStreams; uint8_t activeStream;
    void* frameBuf;
    uint32_t frameCapacity;
    AudioSampleCallback callback;
    volatile bool stopping;
};

void Init(USB::Device* dev, Interface* ifce);
void Deinit(USB::Device* dev);
bool StartStream(Device* uac, uint8_t altSetting);
bool StopStream (Device* uac);


void RegisterAudioCallback(Device* uac, AudioSampleCallback cb);

} // namespace USB::UAC