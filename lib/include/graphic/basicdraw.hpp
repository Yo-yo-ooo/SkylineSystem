#pragma once
#ifndef _BASIC_DRAW_H_
#define _BASIC_DRAW_H_

#include <stdint.h>
#include <stddef.h>
#include <graphic/fb.h>

class BasicDraw
{
private:
    FrameBuffer *FrameBuf;
public:
    BasicDraw(FrameBuffer *fb) : FrameBuf(fb) {}

    void ClearScreen(uint32_t Color);
    void PutPixel(uint64_t X, uint64_t Y, uint32_t Color);
    void DrawHLine(uint64_t X, uint64_t Y, uint64_t Width, uint32_t Color); 
    void DrawVLine(uint64_t X, uint64_t Y, uint64_t Height, uint32_t Color); 
    void DrawRect(uint64_t X, uint64_t Y, uint64_t Width, uint64_t Height, uint32_t Color, bool Fill);
    void DrawLine(int64_t x0, int64_t y0, int64_t x1, int64_t y1, uint32_t Color);
    void DrawWindow(uint64_t X, uint64_t Y, uint64_t W, uint64_t H, const char* Title);
    void DrawCircle(uint64_t xc, uint64_t yc, uint64_t r, uint32_t Color);
    void DrawRoundedRect(uint64_t X, uint64_t Y, uint64_t W, uint64_t H, uint64_t R, uint32_t Color, bool Fill);
    void DrawProportionalUI();
    void FillTriangle(int x0, int y0, int x1, int y1, int x2, int y2, uint32_t color);

    ~BasicDraw(){}
};

#endif
