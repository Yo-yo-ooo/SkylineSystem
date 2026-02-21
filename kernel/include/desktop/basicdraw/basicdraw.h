#pragma once
#ifndef _DEAKTOP_BASIC_DRAW_H_
#define _DEAKTOP_BASIC_DRAW_H_

#include <stdint.h>
#include <stddef.h>
#include <pdef.h>
#include <conf.h>
#include <klib/renderer/fb.h>

#ifdef CONFIG_USE_DESKTOP_SUBSYS

class BasicDraw
{
private:
    Framebuffer *FrameBuf;
public:
    BasicDraw(Framebuffer *fb) : FrameBuf(fb) {}

    void ClearScreen(uint32_t Color);
    void PutPixel(uint64_t X,uint64_t Y,uint32_t Color);

    void DrawHLine(uint64_t X, uint64_t Y, uint64_t Width, uint32_t Color); // 画水平线（极快）
    void DrawVLine(uint64_t X, uint64_t Y, uint64_t Height, uint32_t Color); // 画垂直线
    void DrawRect(uint64_t X, uint64_t Y, uint64_t Width, uint64_t Height, uint32_t Color, bool Fill);
    void DrawLine(int64_t x0, int64_t y0, int64_t x1, int64_t y1, uint32_t Color); // 万能画线
    void DrawWindow(uint64_t X, uint64_t Y, uint64_t W, uint64_t H, const char* Title);
    void DrawCircle(uint64_t xc, uint64_t yc, uint64_t r, uint32_t Color);

    ~BasicDraw(){}
};

#endif

#endif