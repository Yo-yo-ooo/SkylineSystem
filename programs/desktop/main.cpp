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
static char intTo_stringOutput[128];

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
    memcpy(UIBase,fb.BaseAddress,fb.BufferSize);

    /* TTF_Font *TTFFont;
    uint8_t TF = TTF_ReadFont(&TTFFont,"/mp/SourceHanSerifTC_Medium.ttf",64,32);
    if(TF != 0) {
        // 打印前缀
        syscall(24, (long)"FAULT! Code: ", 13, 0, 0, 0, 0);   
        
        // 显式转换为无符号 64 位整数，或者直接传 TF 让它隐式提升为 int
        const char * TF_STR = to_string((uint64_t)TF); 
        
        uint64_t len = 0;
        while(TF_STR[len] != '\0') len++; // 简单算一下长度
        
        syscall(24, (long)TF_STR, len, 0, 0, 0, 0);
    }  */
    MouseInit();


    int32_t prev_x = -100; 
    int32_t prev_y = -100;
    
    // 提前获取指针和宽高，避免每次循环都解引用
    uint32_t* fb_ptr = (uint32_t*)fb.BaseAddress;
    uint32_t* ui_ptr = (uint32_t*)UIBase;
    int32_t fb_width = fb.Width;
    int32_t fb_height = fb.Height;
    int32_t fb_psl = fb.PixelsPerScanLine; // 每行像素数

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

        for (int y = 0; y < 16; y++) {
            int py = prev_y + y;
            if (py < 0 || py >= fb_height) continue; // 越界跳过
            
            for (int x = 0; x < 16; x++) {
                int px = prev_x + x;
                if (px < 0 || px >= fb_width) continue; // 越界跳过
                
                // 从背景恢复到显存
                fb_ptr[py * fb_psl + px] = ui_ptr[py * fb_psl + px];
            }
        }

        DrawMousePointer(mx, my, &fb);

        prev_x = mx;
        prev_y = my;
    }
    syscall(24, (long)"OHOHOHOHO!", 7, 0, 0, 0, 0);    

    //while (true);
    

    syscall(9, 0, 0, 0, 0, 0, 0); // Exit
    syscall(24, (long)msg, 13, 0, 0, 0, 0);
    
    while (true);
    return 0;
}
