//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT
#include <gui/fb.h>
#include <syscall.h>
#include <gui/basicdraw.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

char intTo_stringOutput[128];
const char *to_string(uint64_t value)
{
    uint8_t size = 0;
    uint64_t sizetest = value;
    while (sizetest / 10 > 0)
    {
        sizetest /= 10;
        size++;
    }
    uint8_t index = 0;
    while (value / 10 > 0)
    {
        uint8_t remainder = value % 10;
        value /= 10;
        intTo_stringOutput[size - index] = remainder + '0';
        index++;
    }
    uint8_t remainder = value % 10;
    intTo_stringOutput[size - index] = remainder + '0';
    intTo_stringOutput[size + 1] = 0;
    intTo_stringOutput[size + 2] = 0;
    return intTo_stringOutput;
}

int main(){
    const char *msg = "Hello, World!";
    FrameBuffer fb;
    syscall(24, (long)msg, 13, 0, 0, 0, 0);
    uint64_t FbAddr = MapFB();
    fb = GetFBInfo();
    fb.BaseAddress = (void*)FbAddr;
    
    BasicDraw bd((FrameBuffer*)&fb);
    // 绘制自适应UI
    bd.DrawProportionalUI();
    
    syscall(24,(long)to_string(fb.BufferSize),256,0,0,0,0);

    uint64_t bufsz = __atomic_load_n(&fb.BufferSize, __ATOMIC_ACQUIRE);
    size_t x = (size_t)fb.BufferSize;
    syscall(24,(long)to_string(x),256,0,0,0,0);
    void *UIBase = malloc(x);
    
    if(UIBase == nullptr)
        syscall(24, (long)"FAULT!", 7, 0, 0, 0, 0);    
    
    syscall(24, (long)msg, 13, 0, 0, 0, 0);
    //memcpy(UIBase,fb.BaseAddress,fb.BufferSize);

    FILE *fp = fopen("/mp/README.md","r");
    char buf[25];
    fread(buf,25,1,fp);
    syscall(24, (long)buf, 25, 0, 0, 0, 0); 
    

    while (true);
    

    syscall(9, 0, 0, 0, 0, 0, 0); // Exit
    syscall(24, (long)msg, 13, 0, 0, 0, 0);
    
    while (true);
    return 0;
}
