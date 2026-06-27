//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT

#pragma once
#include <stdint.h>
#include <stddef.h>

typedef struct alignas(16) FrameBuffer {
    void* BaseAddress;
    uint64_t BufferSize;
    uint64_t Width;
    uint64_t Height;
    uint64_t PixelsPerScanLine;
} FrameBuffer;

FrameBuffer GetFBInfo();
uint64_t MapFB();
