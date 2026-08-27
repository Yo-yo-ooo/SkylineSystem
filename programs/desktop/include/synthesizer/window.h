//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT

#pragma once

#include <stdint.h>

typedef struct Window{
    uint32_t SizeX,SizeY;
    uint32_t PosX,PosY;
    // Must in Window Size
    // Frame means (Title bar: Close/Maximize/Minimize)
    uint32_t FrameStartX,FrameStartY;
    uint32_t FrameEndX,FrameEndY;
    // Frame Buffer Base Address
    uint64_t FbAddr;
};

class WindowSyncer
{
private:
    
public:
    WindowSyncer();
    ~WindowSyncer();
};