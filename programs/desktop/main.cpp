//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT
#include <graphic/fb.h>
#include <syscall.h>
#include <graphic/basicdraw.hpp>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <base/font/ttf/ttf.h>
#include <mouse/ps2.h>
#include <synthesizer/window.h>
static char intTo_stringOutput[128];

extern void TLoad(FrameBuffer *Fb);

// 处理无符号 64 位整数
const char *to_string(uint64_t value)
{
    uint8_t i = 0;
    
    // 处理 0 的特殊情况
    if (value == 0) {
        intTo_stringOutput[i++] = '0';
        intTo_stringOutput[i] = '\0';
        return intTo_stringOutput;
    }

    // 反向写入字符
    while (value > 0) {
        intTo_stringOutput[i++] = (value % 10) + '0';
        value /= 10;
    }
    
    intTo_stringOutput[i] = '\0'; // 添加结束符

    // 翻转字符串
    uint8_t left = 0;
    uint8_t right = i - 1;
    while (left < right) {
        char temp = intTo_stringOutput[left];
        intTo_stringOutput[left] = intTo_stringOutput[right];
        intTo_stringOutput[right] = temp;
        left++;
        right--;
    }

    return intTo_stringOutput;
}
const char *to_string(int64_t value)
{
    if (value < 0) {
        uint64_t u_val = -value; 
        const char* num_str = to_string(u_val); // 这里会覆盖 buffer[0] 的 '-'
        
        uint8_t len = 0;
        while (num_str[len] != '\0') len++;
        
        // 整体后移一位
        for (int8_t j = len; j >= 0; j--) {
            intTo_stringOutput[j + 1] = intTo_stringOutput[j];
        }
        
        intTo_stringOutput[0] = '-'; 
        return intTo_stringOutput;
    }
    return to_string((uint64_t)value);
}

extern void DrawMousePointer(int32_t mousex,int32_t mousey, FrameBuffer* framebuffer);

// 处理单个字符
const char* to_string(char value)
{
    intTo_stringOutput[0] = value;
    intTo_stringOutput[1] = '\0';
    return intTo_stringOutput;
}

int main(){
    const char *msg = "Hello, World!";
    FrameBuffer fb;
    syscall(24, (long)msg, 13, 0, 0, 0, 0);
    uint64_t FbAddr = MapFB();
    fb = GetFBInfo();
    fb.BaseAddress = (void*)FbAddr;

    //

    /* ---- wallpaper -> compositor layer 0 --------------------------------
       The wallpaper is rendered OFF-SCREEN into a tightly packed bitmap and
       registered as a full-screen window on the bottom layer, instead of
       being painted directly onto the scanout framebuffer. The parallel
       compositor blits it every frame while strip-sweeping the screen. */
    uint32_t scrW = (uint32_t)fb.Width;
    uint32_t scrH = (uint32_t)fb.Height;
    size_t wallBytes = (size_t)scrW * scrH * sizeof(uint32_t);
    uint32_t* wallBuf = (uint32_t*)malloc(wallBytes);
    if (wallBuf == nullptr)
        syscall(24, (long)"FAULT: wallpaper OOM", 20, 0, 0, 0, 0);

    BasicDraw bd((FrameBuffer*)&fb);
    bd.RenderWallpaper(wallBuf);              // build wallpaper layer bitmap

    /* ---- bring up the compositor and mount wallpaper as layer 0 ---- */
    Compositor& comp = Compositor::Get();
    if (!comp.Init((FrameBuffer*)&fb))
        syscall(24, (long)"FAULT: comp OOM", 15, 0, 0, 0, 0);

    static Window wallpaperWin;
    wallpaperWin.PosX = wallpaperWin.PosY = 0;
    wallpaperWin.SizeX = scrW;
    wallpaperWin.SizeY = scrH;
    wallpaperWin.FrameStartX = wallpaperWin.FrameStartY = 0;
    wallpaperWin.FrameEndX = scrW;
    wallpaperWin.FrameEndY = scrH;
    wallpaperWin.FbAddr = (uint64_t)wallBuf;

    TLoad(&fb);

    CompLayer* layer0 = comp.CreateLayer(0);
    comp.RegisterWindow(&wallpaperWin, layer0);
    comp.StartWorkers();                      // one pinned worker per CPU

    MouseInit();

    /* TTF_Font *TTFFont;
    uint8_t TF = TTF_ReadFont(&TTFFont,"/mp/SourceHanSerifTC_Medium.ttf",64,32);
    if(TF != 0) {
        // 打印前缀
        syscall(24, (long)"FAULT! Code: ", 13, 0, 0, 0, 0);   
        
        // 显式转换为无符号的 64 位整数，或者直接传 TF 让它隐式提升为 int
        const char * TF_STR = to_string((uint64_t)TF); 
        
        uint64_t len = 0;
        while(TF_STR[len] != '\0') len++; // 简单算一下长度
        
        syscall(24, (long)TF_STR, len, 0, 0, 0, 0);
    }  
    TTF_DrawText(&fb,TTFFont,200,200,"你好!",0);
    for(;;); */
    /*for(;;){ THIS TEST PASSED!!!
        void *p = malloc(0x666d);
        free(p);
    }*/

    int32_t prev_x = -100; 
    int32_t prev_y = -100;
    
    int32_t fb_width = (int32_t)fb.Width;
    int32_t fb_height = (int32_t)fb.Height;

    ps2_mouse_state_t *p = (ps2_mouse_state_t*)mouse_addr;

    uint32_t seq1, seq2;
    int32_t mx, my;

    for(;;){
        while (true) {
            // 读取开始前的序列号
            seq1 = __atomic_load_n(&p->seq, __ATOMIC_ACQUIRE);
            
            // 如果是奇数，说明内核正在写，等一会再读
            if (seq1 & 1) {
                continue;
            }

            // 此时 seq 为偶数，快速拷贝坐标
            mx = p->x;
            my = p->y;

            // 读取结束后的序列号
            seq2 = __atomic_load_n(&p->seq, __ATOMIC_ACQUIRE);
            
            // 如果前后序列号一致，说明读取期间没有发生中断写入，数据有效
            if (seq1 == seq2) {
                break;
            }
            // 否则说明读了一半被中断打断了，作废，重新进入 while 循环读取
        }

        if (mx < 0) mx = 0;
        if (my < 0) my = 0;
        if (mx >= fb_width - 16) mx = fb_width - 16;
        if (my >= fb_height - 16) my = fb_height - 16;

        if (mx == prev_x && my == prev_y) {
            continue;
        }

        // Re-composite the whole screen across all CPUs: layer 0 wallpaper
        // is re-laid first, then every higher layer stacks on top. This also
        // erases the previous cursor, so no manual 16x16 restore is needed.
        comp.Compose();

        // Paint the cursor on top of the freshly composited frame.
        DrawMousePointer(mx, my, (FrameBuffer*)&fb);

        prev_x = mx;
        prev_y = my;
    }
    syscall(9, 0, 0, 0, 0, 0, 0); // Exit
    return 0;
}
