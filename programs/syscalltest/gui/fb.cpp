//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT
#include <gui/fb.h>
#include <syscall.h>

FrameBuffer GetFBInfo() {
    FrameBuffer fb;
    fb.BaseAddress = (void*)syscall(26,6,0,0,0,0,0);
    fb.BufferSize = syscall(26,6,0,1,0,0,0);
    fb.Height = syscall(26,6,0,2,0,0,0);
    fb.PixelsPerScanLine = syscall(26,6,0,3,0,0,0);
    fb.Width = syscall(26,6,0,4,0,0,0);
    return fb;
}

uint64_t MapFB() {
    return syscall(21,6,0,0,0,0,0);
}