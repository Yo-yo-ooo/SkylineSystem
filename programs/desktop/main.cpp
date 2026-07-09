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
// 建议加上 static 防止头文件包含时的重定义错误
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
    //memcpy(UIBase,fb.BaseAddress,fb.BufferSize);

    FILE *fp = fopen("/mp/README.md","r");
    char buf[25];
    fread(buf,25,1,fp);
    syscall(24, (long)buf, 25, 0, 0, 0, 0); 
    fclose(fp);

    TTF_Font *TTFFont;
    uint8_t TF = TTF_ReadFont(&TTFFont,"/mp/SourceHanSerifTC_Medium.ttf",64,32);
    if(TF != 0) {
        // 打印前缀
        syscall(24, (long)"FAULT! Code: ", 13, 0, 0, 0, 0);   
        
        // 显式转换为无符号 64 位整数，或者直接传 TF 让它隐式提升为 int
        const char * TF_STR = to_string((uint64_t)TF); 
        
        // 打印错误码。注意：不知道你 syscall 24 的第三个参数是不是字符串长度？
        // 如果是长度，这里不能用固定值 3，因为错误码可能是 1 位数也可能是 2 位数。
        // 最好用 strlen 计算长度，或者如果你的 syscall 支持以 \0 结尾，就传 0。
        // 这里假设你的 syscall 遇到 0 长度会自动读到 \0 停止，或者你手动算长度。
        uint64_t len = 0;
        while(TF_STR[len] != '\0') len++; // 简单算一下长度
        
        syscall(24, (long)TF_STR, len, 0, 0, 0, 0);
    }
    MouseInit();
    TTF_DrawText(&fb,TTFFont,20,20,"Hello 你好",RGB(0,0,0));
    TTF_DrawText(&fb,TTFFont,200,400,"HaHaHa (￣y▽,￣)╭ ",RGB(0,0,0));
    // 假设你的 BasicDraw 有填充矩形的功能，或者你可以手写一个
    // 如果没有，可以简单写个内联函数刷白背景
    auto clear_rect = [&](int x, int y, int w, int h) {
        uint32_t* fb_ptr = (uint32_t*)fb.BaseAddress;
        for(int yy = y; yy < y+h; yy++)
            for(int xx = x; xx < x+w; xx++)
                fb_ptr[yy * fb.PixelsPerScanLine + xx] = 0xFFFFFFFF; // 白色
    };

    for(;;){
        ps2_mouse_state_t *p = (ps2_mouse_state_t*)mouse_addr;
        
        // 清除旧文字区域 (假设文字宽度大概 150，高度 80)
        clear_rect(200, 180, 300, 80); 
        clear_rect(400, 180, 300, 80);

        // 绘制黑色文字
        TTF_DrawText(&fb,TTFFont,200,200,to_string((int64_t)p->x),RGB(0,0,0));
        TTF_DrawText(&fb,TTFFont,400,200,to_string((int64_t)p->y),RGB(0,0,0));
    }
    
    syscall(24, (long)"OHOHOHOHO!", 7, 0, 0, 0, 0);    

    //while (true);
    

    syscall(9, 0, 0, 0, 0, 0, 0); // Exit
    syscall(24, (long)msg, 13, 0, 0, 0, 0);
    
    while (true);
    return 0;
}
