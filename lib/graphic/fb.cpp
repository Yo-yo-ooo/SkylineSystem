//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT
#include <graphic/basicdraw.hpp>

// 辅助函数放入匿名命名空间，防止污染全局符号
namespace {
    uint32_t g_seed = 123456789;

    uint32_t RandomU32() {
        g_seed = g_seed * 1103515245 + 12345;
        return (g_seed >> 16) & 0x7FFF; 
    }

    int RandomRange(int min, int max) {
        if (min > max) { int t = min; min = max; max = t; }
        uint32_t range = (uint32_t)(max - min + 1);
        return min + (int)(RandomU32() % range);
    }

    int clamp(int v, int min, int max) {
        if (v < min) return min;
        if (v > max) return max;
        return v;
    }

    uint32_t RandomColor() {
        uint32_t palette[] = {
            0xFF1A1C3B, 0xFF2A2E5A, 0xFF3A2E6B, 0xFF4B3A7C, 0xFF5B3A8C,
            0xFF1E3A5F, 0xFF1B4A6F, 0xFF2E5A7A, 0xFF5C4A6E, 0xFF6B4A7A,
            0xFF7A4A5C, 0xFF8B4556, 0xFF9B4B83, 0xFFB8457D, 0xFFB84B4B,
            0xFFC05A3A, 0xFFC06A2E, 0xFFB07A25, 0xFF8C6B3A, 0xFF6B5A4A
        };

        uint32_t base = palette[RandomU32() % (sizeof(palette) / sizeof(palette[0]))];
        int r = (base >> 16) & 0xFF;
        int g = (base >> 8) & 0xFF;
        int b = base & 0xFF;

        r = clamp(r + RandomRange(-10, 20), 0, 255);
        g = clamp(g + RandomRange(-10, 20), 0, 255);
        b = clamp(b + RandomRange(-10, 20), 0, 255);

        return (0xFFu << 24) | (r << 16) | (g << 8) | b;
    }
}

// ===================== 基础绘图函数实现 =====================

void BasicDraw::ClearScreen(uint32_t Color) {
    uint32_t* p = (uint32_t*)FrameBuf->BaseAddress;
    // 优化：直接线性填充，比嵌套循环快得多
    uint64_t total_pixels = FrameBuf->PixelsPerScanLine * FrameBuf->Height;
    for (uint64_t i = 0; i < total_pixels; i++) {
        p[i] = Color;
    }
}

void BasicDraw::PutPixel(uint64_t X, uint64_t Y, uint32_t Color) {
    if (X >= FrameBuf->Width || Y >= FrameBuf->Height) return;
    uint32_t* PPtr = (uint32_t*)FrameBuf->BaseAddress;
    PPtr[Y * FrameBuf->PixelsPerScanLine + X] = Color;
}

void BasicDraw::DrawHLine(uint64_t X, uint64_t Y, uint64_t Width, uint32_t Color) {
    if (Y >= FrameBuf->Height || X >= FrameBuf->Width) return;
    
    uint64_t endX = X + Width;
    if (endX > FrameBuf->Width) endX = FrameBuf->Width;
    
    uint32_t* p = (uint32_t*)FrameBuf->BaseAddress + (Y * FrameBuf->PixelsPerScanLine);
    for (uint64_t i = X; i < endX; i++) {
        p[i] = Color;
    }
}

void BasicDraw::DrawVLine(uint64_t X, uint64_t Y, uint64_t Height, uint32_t Color) {
    if (X >= FrameBuf->Width || Y >= FrameBuf->Height) return;
    
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
    if (Fill) {
        for (uint64_t i = 0; i < H; i++) {
            DrawHLine(X, Y + i, W, Color);
        }
    } else {
        DrawHLine(X, Y, W, Color);
        DrawHLine(X, Y + H - 1, W, Color);
        DrawVLine(X, Y, H, Color);
        DrawVLine(X + W - 1, Y, H, Color);
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
        // 修复：直接在这里做带符号判断，避免强转负数到 uint64_t 导致的隐患
        if (x0 >= 0 && y0 >= 0 && x0 < (int64_t)FrameBuf->Width && y0 < (int64_t)FrameBuf->Height) {
            PutPixel((uint64_t)x0, (uint64_t)y0, Color);
        }
        if (x0 == x1 && y0 == y1) break;
        
        e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void BasicDraw::DrawWindow(uint64_t X, uint64_t Y, uint64_t W, uint64_t H, const char* Title) {
    (void)Title; // 暂未使用，避免编译警告

    // 背景填充
    DrawRect(X, Y, W, H, 0xFFC6C6C6, true);

    uint64_t titleHeight = 25;
    // 标题栏
    DrawRect(X + 2, Y + 2, W - 4, titleHeight, 0xFF000080, true);

    // 3D 边框效果
    DrawHLine(X, Y, W, 0xFFFFFFFF);         // Top
    DrawVLine(X, Y, H, 0xFFFFFFFF);         // Left
    DrawHLine(X, Y + H - 1, W, 0xFF404040); // Bottom
    DrawVLine(X + W - 1, Y, H, 0xFF404040); // Right
}

void BasicDraw::DrawCircle(uint64_t xc, uint64_t yc, uint64_t r, uint32_t Color) {
    int64_t x = 0, y = (int64_t)r;
    int64_t d = 3 - 2 * (int64_t)r;

    auto draw8Points = [&](uint64_t xc, uint64_t yc, int64_t x, int64_t y) {
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
    if (W == 0 || H == 0) return;
    if (R * 2 > W) R = W / 2;
    if (R * 2 > H) R = H / 2;
    if (R == 0) {
        DrawRect(X, Y, W, H, Color, Fill);
        return;
    }

    int64_t x = 0;
    int64_t y = (int64_t)R;
    int64_t d = 3 - 2 * (int64_t)R;

    while (y >= x) {
        if (Fill) {
            DrawHLine(X + R - x, Y + R - y, W - 2 * R + 2 * x, Color);
            DrawHLine(X + R - y, Y + R - x, W - 2 * R + 2 * y, Color);
            DrawHLine(X + R - x, Y + H - R + y - 1, W - 2 * R + 2 * x, Color);
            DrawHLine(X + R - y, Y + H - R + x - 1, W - 2 * R + 2 * y, Color);
        } else {
            PutPixel(X + R - x, Y + R - y, Color);
            PutPixel(X + R - y, Y + R - x, Color);
            PutPixel(X + W - R + x - 1, Y + R - y, Color);
            PutPixel(X + W - R + y - 1, Y + R - x, Color);
            PutPixel(X + R - x, Y + H - R + y - 1, Color);
            PutPixel(X + R - y, Y + H - R + x - 1, Color);
            PutPixel(X + W - R + x - 1, Y + H - R + y - 1, Color);
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

    if (Fill) {
        for (uint64_t i = R; i < H - R; i++) {
            DrawHLine(X, Y + i, W, Color);
        }
    } else {
        DrawHLine(X + R, Y, W - 2 * R, Color);
        DrawHLine(X + R, Y + H - 1, W - 2 * R, Color);
        DrawVLine(X, Y + R, H - 2 * R, Color);
        DrawVLine(X + W - 1, Y + R, H - 2 * R, Color);
    }
}

// ===================== 扫描线填充三角形 (纯整数运算版) =====================
void BasicDraw::FillTriangle(int x0, int y0, int x1, int y1, int x2, int y2, uint32_t color) {
    // 按 y 排序顶点 (y0 <= y1 <= y2)
    if (y0 > y1) { int tx = x0; x0 = x1; x1 = tx; int ty = y0; y0 = y1; y1 = ty; }
    if (y0 > y2) { int tx = x0; x0 = x2; x2 = tx; int ty = y0; y0 = y2; y2 = ty; }
    if (y1 > y2) { int tx = x1; x1 = x2; x2 = tx; int ty = y1; y1 = y2; y2 = ty; }

    int totalHeight = y2 - y0;
    
    // 退化情况：三角形是一条水平线
    if (totalHeight == 0) {
        int minX = x0 < x1 ? (x0 < x2 ? x0 : x2) : (x1 < x2 ? x1 : x2);
        int maxX = x0 > x1 ? (x0 > x2 ? x0 : x2) : (x1 > x2 ? x1 : x2);
        if (minX < maxX) {
            DrawHLine(minX, y0, maxX - minX + 1, color);
        } else {
            PutPixel(minX, y0, color);
        }
        return;
    }

    for (int y = y0; y <= y2; y++) {
        bool secondHalf = (y > y1 || y1 == y0);
        int segmentHeight = secondHalf ? (y2 - y1) : (y1 - y0);
        if (segmentHeight == 0) continue; // 防止除零

        // 使用 64 位整数运算替代浮点数，精确且高效
        int64_t alpha_num = y - y0;
        int64_t alpha_den = totalHeight;
        
        int64_t beta_num = secondHalf ? (y - y1) : (y - y0);
        int64_t beta_den = segmentHeight;

        int A = x0 + (int)((int64_t)(x2 - x0) * alpha_num / alpha_den);
        int B = secondHalf ? (x1 + (int)((int64_t)(x2 - x1) * beta_num / beta_den))
                           : (x0 + (int)((int64_t)(x1 - x0) * beta_num / beta_den));

        if (A > B) { int temp = A; A = B; B = temp; }

        // 裁剪到屏幕边界内
        if (y < 0 || y >= (int)FrameBuf->Height) continue;
        if (A >= (int)FrameBuf->Width) continue;
        if (B < 0) continue;
        if (A < 0) A = 0;
        if (B >= (int)FrameBuf->Width) B = (int)FrameBuf->Width - 1;

        DrawHLine((uint64_t)A, (uint64_t)y, (uint64_t)(B - A + 1), color);
    }
}

void BasicDraw::DrawProportionalUI() {
    int tileSize = 64;
    uint64_t w = FrameBuf->Width;
    uint64_t h = FrameBuf->Height;

    for (uint64_t y = 0; y < h; y += tileSize) {
        uint64_t curH = (y + tileSize > h) ? (h - y) : tileSize;
        for (uint64_t x = 0; x < w; x += tileSize) {
            uint64_t curW = (x + tileSize > w) ? (w - x) : tileSize;
            uint64_t x2 = x + curW, y2 = y + curH;

            // 获取随机颜色 (本身已带有 0xFF 的完全不透明 Alpha 通道)
            uint32_t c1 = RandomColor();
            uint32_t c2 = RandomColor();

            if (RandomU32() & 1) {
                FillTriangle((int)x, (int)y, (int)x2, (int)y, (int)x, (int)y2, c1);
                FillTriangle((int)x2, (int)y, (int)x2, (int)y2, (int)x, (int)y2, c2);
            } else {
                FillTriangle((int)x, (int)y, (int)x2, (int)y, (int)x2, (int)y2, c1);
                FillTriangle((int)x, (int)y, (int)x, (int)y2, (int)x2, (int)y2, c2);
            }
        }
    }
}