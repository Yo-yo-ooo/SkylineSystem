#pragma once
#ifndef _GRAPHIC_FB_H_
#define _GRAPHIC_FB_H_

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t Color;

#define RGB(r, g, b)   (0xFF000000 | ((r) << 16) | ((g) << 8) | (b))
#define ARGB(a, r, g, b) (((a) << 24) | ((r) << 16) | ((g) << 8) | (b))

#define COLOR_BLACK   RGB(0, 0, 0)
#define COLOR_WHITE   RGB(255, 255, 255)
#define COLOR_RED     RGB(255, 0, 0)
#define COLOR_GREEN   RGB(0, 255, 0)
#define COLOR_BLUE    RGB(0, 0, 255)

typedef struct FrameBuffer {
    void* BaseAddress;
    uint64_t BufferSize;
    uint64_t Width;
    uint64_t Height;
    uint64_t PixelsPerScanLine;
} FrameBuffer;

FrameBuffer GetFBInfo();
uint64_t MapFB();

void PutPixel(FrameBuffer *FB, uint64_t x, uint64_t y, Color color);
void DrawFillRect(
    FrameBuffer *FB, uint64_t x, uint64_t y, 
    uint64_t width, uint64_t height, Color color
);
void DrawRect(
    FrameBuffer *FB, uint64_t x, uint64_t y, 
    uint64_t width, uint64_t height, Color color
);
void DrawLine(
    FrameBuffer *FB, int64_t x0, int64_t y0, 
    int64_t x1, int64_t y1, Color color
);
void DrawCircle(
    FrameBuffer *FB, uint64_t center_x, 
    uint64_t center_y, uint64_t radius, Color color
);
void DrawBitmap(
    FrameBuffer *FB, uint64_t x, uint64_t y, 
    uint64_t img_width, uint64_t img_height, const uint32_t *image_data
);
void ClearScreen(FrameBuffer *FB, Color color);

#ifdef __cplusplus
}
#endif

#endif

