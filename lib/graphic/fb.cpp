//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT
#include <graphic/basicdraw.hpp>
#include <math.h>
#include <string.h>

// ===================== 基础绘图函数（修复+优化版） =====================

void BasicDraw::ClearScreen(uint32_t Color){
    uint32_t *PPtr = (uint32_t*)this->FrameBuf->BaseAddress;
    // 按扫描线总宽度线性清屏，最高效
    uint64_t total = (uint64_t)this->FrameBuf->PixelsPerScanLine
                   * (uint64_t)this->FrameBuf->Height;
    for (uint64_t i = 0; i < total; i++)
        PPtr[i] = Color;
}

void BasicDraw::PutPixel(uint64_t X, uint64_t Y, uint32_t Color){
    if (X >= this->FrameBuf->Width || Y >= this->FrameBuf->Height)
        return;
    uint32_t *PPtr = (uint32_t*)this->FrameBuf->BaseAddress;
    PPtr[Y * this->FrameBuf->PixelsPerScanLine + X] = Color;
}

void BasicDraw::DrawHLine(uint64_t X, uint64_t Y, uint64_t Width, uint32_t Color) {
    if (Y >= FrameBuf->Height || Width == 0) return;
    if (X >= FrameBuf->Width) return;

    uint64_t endX = X + Width;
    if (endX > FrameBuf->Width) endX = FrameBuf->Width;
    if (endX <= X) return;

    uint32_t* p = (uint32_t*)FrameBuf->BaseAddress + (Y * FrameBuf->PixelsPerScanLine);
    for (uint64_t i = X; i < endX; i++)
        p[i] = Color;
}

void BasicDraw::DrawVLine(uint64_t X, uint64_t Y, uint64_t Height, uint32_t Color) {
    if (X >= FrameBuf->Width || Y >= FrameBuf->Height || Height == 0) return;

    uint64_t endY = Y + Height;
    if (endY > FrameBuf->Height) endY = FrameBuf->Height;

    uint32_t* p = (uint32_t*)FrameBuf->BaseAddress + (Y * FrameBuf->PixelsPerScanLine + X);
    uint64_t stride = FrameBuf->PixelsPerScanLine;

    for (uint64_t i = Y; i < endY; i++) {
        *p = Color;
        p += stride;
    }
}

void BasicDraw::DrawRect(uint64_t X, uint64_t Y, uint64_t W, uint64_t H, uint32_t Color, bool Fill) {
    if (W == 0 || H == 0) return; // 防止下面 W-1/H-1 下溢

    if (Fill) {
        for (uint64_t i = 0; i < H; i++)
            DrawHLine(X, Y + i, W, Color);
    } else {
        DrawHLine(X,         Y,         W, Color);
        DrawHLine(X,         Y + H - 1, W, Color);
        DrawVLine(X,         Y,         H, Color);
        DrawVLine(X + W - 1, Y,         H, Color);
    }
}

void BasicDraw::DrawLine(int64_t x0, int64_t y0, int64_t x1, int64_t y1, uint32_t Color) {
    int64_t dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
    int64_t dy = (y1 > y0) ? (y1 - y0) : (y0 - y1);
    dy = -dy;

    int64_t sx = (x0 < x1) ? 1 : -1;
    int64_t sy = (y0 < y1) ? 1 : -1;

    int64_t err = dx + dy;
    int64_t e2;

    while (true) {
        if (x0 >= 0 && y0 >= 0)
            PutPixel((uint64_t)x0, (uint64_t)y0, Color);
        if (x0 == x1 && y0 == y1) break;

        e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void BasicDraw::DrawWindow(uint64_t X, uint64_t Y, uint64_t W, uint64_t H, const char* Title) {
    if (W < 4 || H < 4) return;

    // 客户区背景：浅灰（修复 alpha=0 -> 0xFF）
    DrawRect(X, Y, W, H, 0xFFC6C6C6, true);

    // 标题栏：深蓝
    uint64_t titleHeight = 25;
    if (titleHeight > H - 4) titleHeight = (H > 4) ? (H - 4) : 0;
    if (titleHeight > 0)
        DrawRect(X + 2, Y + 2, W - 4, titleHeight, 0xFF000080, true);

    // 3D 边框
    DrawHLine(X,         Y,         W, 0xFFFFFFFF); // 上：白
    DrawVLine(X,         Y,         H, 0xFFFFFFFF); // 左：白
    DrawHLine(X,         Y + H - 1, W, 0xFF404040); // 下：深灰
    DrawVLine(X + W - 1, Y,         H, 0xFF404040); // 右：深灰

    (void)Title;
}

void BasicDraw::DrawCircle(uint64_t xc, uint64_t yc, uint64_t r, uint32_t Color) {
    if (r == 0) { PutPixel(xc, yc, Color); return; }

    int64_t x = 0, y = (int64_t)r;
    int64_t d = 3 - 2 * (int64_t)r;

    while (y >= x) {
        PutPixel(xc + x, yc + y, Color); PutPixel(xc - x, yc + y, Color);
        PutPixel(xc + x, yc - y, Color); PutPixel(xc - x, yc - y, Color);
        PutPixel(xc + y, yc + x, Color); PutPixel(xc - y, yc + x, Color);
        PutPixel(xc + y, yc - x, Color); PutPixel(xc - y, yc - x, Color);

        x++;
        if (d > 0) { y--; d = d + 4 * (x - y) + 10; }
        else       {     d = d + 4 * x + 6;         }
    }
}

void BasicDraw::DrawRoundedRect(uint64_t X, uint64_t Y, uint64_t W, uint64_t H, uint64_t R, uint32_t Color, bool Fill) {
    if (W == 0 || H == 0) return;
    if (R * 2 > W) R = W / 2;
    if (R * 2 > H) R = H / 2;
    if (R == 0) { DrawRect(X, Y, W, H, Color, Fill); return; }

    int64_t x = 0;
    int64_t y = (int64_t)R;
    int64_t d = 3 - 2 * (int64_t)R;

    while (y >= x) {
        if (Fill) {
            DrawHLine(X + R - x, Y + R - y,         W - 2 * R + 2 * x, Color);
            DrawHLine(X + R - y, Y + R - x,         W - 2 * R + 2 * y, Color);
            DrawHLine(X + R - x, Y + H - R + y - 1, W - 2 * R + 2 * x, Color);
            DrawHLine(X + R - y, Y + H - R + x - 1, W - 2 * R + 2 * y, Color);
        } else {
            PutPixel(X + R - x,         Y + R - y,         Color);
            PutPixel(X + R - y,         Y + R - x,         Color);
            PutPixel(X + W - R + x - 1, Y + R - y,         Color);
            PutPixel(X + W - R + y - 1, Y + R - x,         Color);
            PutPixel(X + R - x,         Y + H - R + y - 1, Color);
            PutPixel(X + R - y,         Y + H - R + x - 1, Color);
            PutPixel(X + W - R + x - 1, Y + H - R + y - 1, Color);
            PutPixel(X + W - R + y - 1, Y + H - R + x - 1, Color);
        }

        if (d > 0) { y--; d = d + 4 * (x - y) + 10; }
        else       {     d = d + 4 * x + 6;         }
        x++;
    }

    if (Fill) {
        for (uint64_t i = R; i < H - R; i++)
            DrawHLine(X, Y + i, W, Color);
    } else {
        DrawHLine(X + R,     Y,         W - 2 * R, Color);
        DrawHLine(X + R,     Y + H - 1, W - 2 * R, Color);
        DrawVLine(X,         Y + R,     H - 2 * R, Color);
        DrawVLine(X + W - 1, Y + R,     H - 2 * R, Color);
    }
}

// ===================== 强化版伪随机数 =====================
static uint32_t g_seed = 123456789u;

static uint32_t RandomU32() {
    g_seed = g_seed * 1103515245u + 12345u;
    g_seed ^= (g_seed >> 13);
    g_seed ^= (g_seed << 17);
    g_seed ^= (g_seed >> 5);
    return g_seed;
}

static int RandomRange(int min, int max) {
    if (min > max) { int t = min; min = max; max = t; }
    uint32_t range = (uint32_t)(max - min + 1);
    return min + (int)(RandomU32() % range);
}

static int clampi(int v, int min, int max) {
    if (v < min) return min;
    if (v > max) return max;
    return v;
}
// ===================== 16.16 定点数扫描线填充三角形 =====================
void BasicDraw::FillTriangle(int x0, int y0, int x1, int y1, int x2, int y2, uint32_t color) {
    // 按 y 排序：y0 <= y1 <= y2
    if (y0 > y1) { int t; t=x0; x0=x1; x1=t; t=y0; y0=y1; y1=t; }
    if (y0 > y2) { int t; t=x0; x0=x2; x2=t; t=y0; y0=y2; y2=t; }
    if (y1 > y2) { int t; t=x1; x1=x2; x2=t; t=y1; y1=y2; y2=t; }

    int totalHeight = y2 - y0;
    if (totalHeight == 0) {
        // 退化为水平线段
        int xmin = x0, xmax = x0;
        if (x1 < xmin) xmin = x1;
        if (x1 > xmax) xmax = x1;
        if (x2 < xmin) xmin = x2;
        if (x2 > xmax) xmax = x2;
        
        if (y0 < 0 || y0 >= (int)FrameBuf->Height) return;
        if (xmin < 0) xmin = 0;
        if (xmax >= (int)FrameBuf->Width) xmax = (int)FrameBuf->Width - 1;
        if (xmin > xmax) return;
        DrawHLine((uint64_t)xmin, (uint64_t)y0, (uint64_t)(xmax - xmin + 1), color);
        return;
    }

    int fbW = (int)FrameBuf->Width;
    int fbH = (int)FrameBuf->Height;

    for (int y = y0; y <= y2; y++) {
        if (y < 0 || y >= fbH) continue;

        int secondHalf    = (y > y1 || y1 == y0) ? 1 : 0;
        int segmentHeight = secondHalf ? (y2 - y1) : (y1 - y0);
        if (segmentHeight == 0) continue;

        // alpha = (y - y0) / totalHeight   (16.16 定点)
        int64_t alphaQ = ((int64_t)(y - y0) << 16) / totalHeight;
        int A = x0 + (int)(((int64_t)(x2 - x0) * alphaQ) >> 16);

        // beta = (secondHalf ? (y - y1) : (y - y0)) / segmentHeight
        int64_t betaQ = ((int64_t)(secondHalf ? (y - y1) : (y - y0)) << 16) / segmentHeight;
        int B = secondHalf
                ? (x1 + (int)(((int64_t)(x2 - x1) * betaQ) >> 16))
                : (x0 + (int)(((int64_t)(x1 - x0) * betaQ) >> 16));

        if (A > B) { int t = A; A = B; B = t; }

        // 裁剪到屏幕
        if (A >= fbW) continue;
        if (B < 0)    continue;
        if (A < 0)    A = 0;
        if (B >= fbW) B = fbW - 1;
        if (A > B) continue;

        DrawHLine((uint64_t)A, (uint64_t)y, (uint64_t)(B - A + 1), color);
    }
}

// ===================== 随机颜色生成 =====================
uint32_t RandomColor() {
    static const uint32_t palette[] = {
        0xFF1A1C3B, 0xFF2A2E5A, 0xFF3A2E6B, 0xFF4B3A7C, 0xFF5B3A8C,
        0xFF1E3A5F, 0xFF1B4A6F, 0xFF2E5A7A, 0xFF5C4A6E, 0xFF6B4A7A,
        0xFF7A4A5C, 0xFF8B4556, 0xFF9B4B83, 0xFFB8457D, 0xFFB84B4B,
        0xFFC05A3A, 0xFFC06A2E, 0xFFB07A25, 0xFF8C6B3A, 0xFF6B5A4A
    };
    uint32_t base = palette[RandomU32() % (sizeof(palette) / sizeof(palette[0]))];

    int r = (base >> 16) & 0xFF;
    int g = (base >>  8) & 0xFF;
    int b =  base        & 0xFF;

    r = clampi(r + RandomRange(-10, 20), 0, 255);
    g = clampi(g + RandomRange(-10, 20), 0, 255);
    b = clampi(b + RandomRange(-10, 20), 0, 255);

    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | ((uint32_t)b);
}

void BasicDraw::DrawProportionalUI() {
    const uint64_t tileSize = 64;
    uint64_t w = FrameBuf->Width;
    uint64_t h = FrameBuf->Height;

    for (uint64_t y = 0; y < h; y += tileSize) {
        uint64_t curH = (y + tileSize > h) ? (h - y) : tileSize;
        for (uint64_t x = 0; x < w; x += tileSize) {
            uint64_t curW = (x + tileSize > w) ? (w - x) : tileSize;
            uint64_t x2 = x + curW, y2 = y + curH;

            uint32_t c1 = RandomColor() | 0xFF000000u;
            uint32_t c2 = RandomColor() | 0xFF000000u;

            if (RandomU32() & 1) {
                FillTriangle((int)x,  (int)y,  (int)x2, (int)y,  (int)x,  (int)y2, c1);
                FillTriangle((int)x2, (int)y,  (int)x2, (int)y2, (int)x,  (int)y2, c2);
            } else {
                FillTriangle((int)x,  (int)y,  (int)x2, (int)y,  (int)x2, (int)y2, c1);
                FillTriangle((int)x,  (int)y,  (int)x,  (int)y2, (int)x2, (int)y2, c2);
            }
        }
    }
}