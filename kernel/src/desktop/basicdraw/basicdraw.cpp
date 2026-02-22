/*
* SPDX-License-Identifier: GPL-2.0-only
* File: basicdraw.cpp
* Copyright (C) 2026 Yo-yo-ooo
*
* This file is part of SkylineSystem.
*
* SkylineSystem is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
*/
#include <desktop/basicdraw/basicdraw.h>

#ifdef CONFIG_USE_DESKTOP_SUBSYS

extern "C" {
//freestanding cpu features functions
func_optimize(3) void *memcpy_fscpuf(void *dst, const void *src, size_t n);
func_optimize(3) void *memmove_fscpuf(void *dst, const void *src, size_t n);
func_optimize(3) int32_t memcmp_fscpuf(const void *left, const void *right, size_t len);
func_optimize(3) void bzero(void *dst, size_t n);
}

func_optimize(3) void *memset32_fscpuf(void *dst, const int32_t val, size_t n){
    void *ret = dst;

#if defined(__x86_64__)
    // 针对 x86_64 的极致优化
    // 'n' 在这里代表像素数量 (Pixels)，即 4-byte units
    if (n > 0) {
        asm volatile (
            "cld\n"
            "rep stosl"  // 使用 stosl (Double-word) 填充 32 位值
            : "+D"(dst), "+c"(n)
            : "a"(val)
            : "memory"
        );
    }

#elif defined(__aarch64__)
    // 针对 AArch64 的优化：使用 STP (Store Pair) 指令
    uint64_t color64 = ((uint64_t)val << 32) | val;
    
    // 每次处理 4 个像素 (16 字节)
    while (n >= 4) {
        asm volatile (
            "stp %0, %0, [%1], #16" 
            : : "r"(color64), "r"(dst) : "memory"
        );
        n -= 4;
        dst = (uint32_t*)dst + 4;
    }
    
    // 处理剩余像素
    while (n--) {
        *(uint32_t *)dst = val;
        dst = (uint32_t*)dst + 1;
    }

#else
    // 基础兜底逻辑
    uint32_t *p = (uint32_t *)dst;
    while (n--) {
        *p++ = val;
    }
#endif

    return ret;

    return ret;
}

void BasicDraw::ClearScreen(uint32_t Color){
    uint64_t fbBase = (uint64_t)this->FrameBuf->BaseAddress;
    uint64_t pxlsPerScanline = this->FrameBuf->PixelsPerScanLine;
    uint64_t fbHeight = this->FrameBuf->Height;

    for (int64_t y = 0; y < this->FrameBuf->Height; y++)
        for (int64_t x = 0; x < this->FrameBuf->Width; x++)
            *((uint32_t*)(fbBase + 4 * (x + pxlsPerScanline * y))) = Color; 
}

void BasicDraw::PutPixel(uint64_t X,uint64_t Y,uint32_t Color){
    if(X > this->FrameBuf->Width || Y > this->FrameBuf->Height)
        return;
    uint32_t *PPtr = (uint32_t*)this->FrameBuf->BaseAddress;
    PPtr[Y * this->FrameBuf->PixelsPerScanLine + X] = Color;
}

void BasicDraw::DrawHLine(uint64_t X, uint64_t Y, uint64_t Width, uint32_t Color) {
    if (Y >= FrameBuf->Height) return;
    uint64_t startX = X;
    uint64_t endX = (X + Width > FrameBuf->Width) ? FrameBuf->Width : X + Width;
    
    uint32_t* p = (uint32_t*)FrameBuf->BaseAddress + (Y * FrameBuf->PixelsPerScanLine);
    // 使用快速填充
    memset32_fscpuf(&p[startX], Color, (endX - startX));
}

void BasicDraw::DrawRect(uint64_t X, uint64_t Y, uint64_t W, uint64_t H, uint32_t Color, bool Fill) {
    if (Fill) {
        for (uint64_t i = 0; i < H; i++) {
            DrawHLine(X, Y + i, W, Color);
        }
    } else {
        // 画四条边
        DrawHLine(X, Y, W, Color);
        DrawHLine(X, Y + H - 1, W, Color);
        DrawVLine(X, Y, H, Color);
        DrawVLine(X + W - 1, Y, H, Color);
    }
}

void BasicDraw::DrawLine(int64_t x0, int64_t y0, int64_t x1, int64_t y1, uint32_t Color) {
    // 自实现简单的 abs 逻辑
    int64_t dx = (x1 - x0 > 0) ? (x1 - x0) : (x0 - x1);
    int64_t dy = (y1 - y0 > 0) ? (y0 - y1) : (y1 - y0); // 注意 dy 在 Bresenham 算法中通常取负数
    
    int64_t sx = (x0 < x1) ? 1 : -1;
    int64_t sy = (y0 < y1) ? 1 : -1;
    
    int64_t err = dx + dy;
    int64_t e2;

    while (true) {
        PutPixel(x0, y0, Color);
        
        if (x0 == x1 && y0 == y1) break;
        
        e2 = 2 * err;
        
        if (e2 >= dy) { 
            err += dy; 
            x0 += sx; 
        }
        
        if (e2 <= dx) { 
            err += dx; 
            y0 += sy; 
        }
    }
}

void BasicDraw::DrawVLine(uint64_t X, uint64_t Y, uint64_t Height, uint32_t Color) {
    // 1. 边界检查：确保起点在屏幕内
    if (X >= FrameBuf->Width || Y >= FrameBuf->Height) return;

    // 2. 裁剪：如果高度超过屏幕底部，截断它
    uint64_t endY = Y + Height;
    if (endY > FrameBuf->Height) {
        endY = FrameBuf->Height;
    }

    // 3. 计算起始指针
    // 指向第 Y 行，第 X 个像素
    uint32_t* p = (uint32_t*)FrameBuf->BaseAddress + (Y * FrameBuf->PixelsPerScanLine + X);
    
    // 4. 步进跨度 (Stride)
    // 每一行需要跳过的像素数正是 PixelsPerScanLine
    uint64_t stride = FrameBuf->PixelsPerScanLine;

    // 5. 循环绘制
    for (uint64_t i = Y; i < endY; i++) {
        *p = Color;   // 画当前像素
        p += stride;  // 跳到下一行的相同 X 位置
    } 
}

void BasicDraw::DrawWindow(uint64_t X, uint64_t Y, uint64_t W, uint64_t H, const char* Title) {
    // 1. 绘制窗口背景 (浅灰色 0xC6C6C6)
    for (uint64_t i = 0; i < H; i++) {
        DrawHLine(X, Y + i, W, 0xC6C6C6);
    }

    // 2. 绘制标题栏 (深蓝色 0x000080)
    uint64_t titleHeight = 25;
    for (uint64_t i = 0; i < titleHeight; i++) {
        DrawHLine(X + 2, Y + 2 + i, W - 4, 0x000080);
    }

    // 3. 绘制立体边框 (模拟 3D 阴影)
    DrawHLine(X, Y, W, 0xFFFFFFFF);         // 顶边白
    DrawVLine(X, Y, H, 0xFFFFFFFF);         // 左边白
    DrawHLine(X, Y + H - 1, W, 0x404040);   // 底边深灰
    DrawVLine(X + W - 1, Y, H, 0x404040);   // 右边深灰
}

void BasicDraw::DrawCircle(uint64_t xc, uint64_t yc, uint64_t r, uint32_t Color) {
    int64_t x = 0, y = r;
    int64_t d = 3 - 2 * r;

    auto draw8Points = [&](uint64_t xc, uint64_t yc, uint64_t x, uint64_t y) {
        PutPixel(xc + x, yc + y, Color); PutPixel(xc - x, yc + y, Color);
        PutPixel(xc + x, yc - y, Color); PutPixel(xc - x, yc - y, Color);
        PutPixel(xc + y, yc + x, Color); PutPixel(xc - y, yc + x, Color);
        PutPixel(xc + y, yc - x, Color); PutPixel(xc - y, yc - x, Color);
    };

    while (y >= x) {
        draw8Points(xc, yc, x, y);
        x++;
        if (d > 0) {
            y--;
            d = d + 4 * (x - y) + 10;
        } else {
            d = d + 4 * x + 6;
        }
    }
}
void BasicDraw::DrawRoundedRect(uint64_t X, uint64_t Y, uint64_t W, uint64_t H, uint64_t R, uint32_t Color, bool Fill) {
    // 基础安全检查：防止半径过大导致图形畸变
    if (R * 2 > W) R = W / 2;
    if (R * 2 > H) R = H / 2;
    if (R == 0) { // 半径为0时退化为普通矩形
        DrawRect(X, Y, W, H, Color, Fill);
        return;
    }

    int64_t x = 0;
    int64_t y = R;
    int64_t d = 3 - 2 * R;

    // 绘制四个圆角
    while (y >= x) {
        if (Fill) {
            // 利用你的 DrawHLine (调用了快速 memset)
            // 填充顶部两个圆角之间的缝隙
            DrawHLine(X + R - x, Y + R - y, W - 2 * R + 2 * x, Color);
            DrawHLine(X + R - y, Y + R - x, W - 2 * R + 2 * y, Color);
            // 填充底部两个圆角之间的缝隙
            DrawHLine(X + R - x, Y + H - R + y - 1, W - 2 * R + 2 * x, Color);
            DrawHLine(X + R - y, Y + H - R + x - 1, W - 2 * R + 2 * y, Color);
        } else {
            // 仅仅是描边
            PutPixel(X + R - x, Y + R - y, Color); // 左上
            PutPixel(X + R - y, Y + R - x, Color);
            PutPixel(X + W - R + x - 1, Y + R - y, Color); // 右上
            PutPixel(X + W - R + y - 1, Y + R - x, Color);
            PutPixel(X + R - x, Y + H - R + y - 1, Color); // 左下
            PutPixel(X + R - y, Y + H - R + x - 1, Color);
            PutPixel(X + W - R + x - 1, Y + H - R + y - 1, Color); // 右下
            PutPixel(X + W - R + y - 1, Y + H - R + x - 1, Color);
        }

        if (d > 0) {
            d = d + 4 * (x - y) + 10;
            y--;
        } else {
            d = d + 4 * x + 6;
        }
        x++;
    }

    // 绘制中间的主体部分
    if (Fill) {
        // 这一部分是圆弧中间没有覆盖到的矩形区域
        for (uint64_t i = R; i < H - R; i++) {
            DrawHLine(X, Y + i, W, Color);
        }
    } else {
        // 描边模式：连接四条直线边
        DrawHLine(X + R, Y, W - 2 * R, Color);         // 顶边
        DrawHLine(X + R, Y + H - 1, W - 2 * R, Color); // 底边
        DrawVLine(X, Y + R, H - 2 * R, Color);         // 左边
        DrawVLine(X + W - 1, Y + R, H - 2 * R, Color); // 右边
    }
}

void BasicDraw::DrawProportionalUI() {
    // 1. 获取屏幕实时尺寸
    uint64_t sw = this->FrameBuf->Width;
    uint64_t sh = this->FrameBuf->Height;

    // 2. 根据比例计算尺寸 (使用整数运算防止浮点开销)
    // 宽度 = 40%, 高度 = 50%
    uint64_t rectW = (sw * 40) / 100;
    uint64_t rectH = (sh * 50) / 100;
    
    // 3. 计算居中坐标
    uint64_t x = (sw - rectW) / 2;
    uint64_t y = (sh - rectH) / 2;

    // 4. 计算自适应圆角半径 (设为宽度的 1/20, 即 5%)
    uint64_t radius = rectW / 20;

    // 5. 绘制
    uint32_t darkBg = 0x1A1A1A;     // 极深灰
    uint32_t cardColor = 0x2D2D2D;  // 稍微浅一点的灰色卡片
    uint32_t accent = 0x0078D7;     // 蓝色高亮边框

    this->ClearScreen(darkBg);
    
    // 画阴影/背景卡片
    this->DrawRoundedRect(x, y, rectW, rectH, radius, cardColor, true);
    
    // 画一个 2 像素宽的外边框（通过两次绘制实现）
    this->DrawRoundedRect(x, y, rectW, rectH, radius, accent, false);
}

#endif