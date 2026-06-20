#include <gui/basicdraw.h>
#include <math.h>
#include <string.h>

// ===================== 修复BUG后的基础绘图函数 =====================
void BasicDraw::ClearScreen(uint32_t Color){
    uint64_t fbBase = (uint64_t)this->FrameBuf->BaseAddress;
    uint64_t pxlsPerScanline = this->FrameBuf->PixelsPerScanLine;
    uint64_t fbHeight = this->FrameBuf->Height;

    for (uint64_t y = 0; y < fbHeight; y++)
        for (uint64_t x = 0; x < this->FrameBuf->Width; x++)
            *((uint32_t*)(fbBase + 4 * (x + pxlsPerScanline * y))) = Color; 
    
}

// 修复：边界判断错误（>= 而非 >）
void BasicDraw::PutPixel(uint64_t X,uint64_t Y,uint32_t Color){
    if(X >= this->FrameBuf->Width || Y >= this->FrameBuf->Height)
        return;
    uint32_t *PPtr = (uint32_t*)this->FrameBuf->BaseAddress;
    PPtr[Y * this->FrameBuf->PixelsPerScanLine + X] = Color;
}

void BasicDraw::DrawHLine(uint64_t X, uint64_t Y, uint64_t Width, uint32_t Color) {
    if (Y >= FrameBuf->Height) return;
    uint64_t startX = X;
    uint64_t endX = (X + Width > FrameBuf->Width) ? FrameBuf->Width : X + Width;
    
    uint32_t* p = (uint32_t*)FrameBuf->BaseAddress + (Y * FrameBuf->PixelsPerScanLine);
    for(uint64_t i = startX;i < endX;i++)
        p[i] = Color;
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
        if(x0 >=0 && y0 >=0) PutPixel((uint64_t)x0, (uint64_t)y0, Color);
        if (x0 == x1 && y0 == y1) break;
        
        e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
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

void BasicDraw::DrawWindow(uint64_t X, uint64_t Y, uint64_t W, uint64_t H, const char* Title) {
    for (uint64_t i = 0; i < H; i++) {
        DrawHLine(X, Y + i, W, 0xC6C6C6);
    }

    uint64_t titleHeight = 25;
    for (uint64_t i = 0; i < titleHeight; i++) {
        DrawHLine(X + 2, Y + 2 + i, W - 4, 0x000080);
    }

    DrawHLine(X, Y, W, 0xFFFFFFFF);
    DrawVLine(X, Y, H, 0xFFFFFFFF);
    DrawHLine(X, Y + H - 1, W, 0x404040);
    DrawVLine(X + W - 1, Y, H, 0x404040);
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

// 辅助函数：计算定点数平方根（牛顿迭代法，避免浮点数开销）
static uint64_t IntegerSqrt(uint64_t n) {
    if (n <= 1) return n;
    uint64_t x0 = n / 2;
    uint64_t x1 = (x0 + n / x0) / 2;
    while (x1 < x0) {
        x0 = x1;
        x1 = (x0 + n / x0) / 2;
    }
    return x0;
}
// ===================== 简单伪随机数（无标准库依赖） =====================
static uint32_t g_seed = 123456789;

static uint32_t RandomU32() {
    g_seed = g_seed * 1103515245 + 12345;
    // 取高位，LCG 的高位比低位更随机
    return (g_seed >> 16) & 0x7FFF; 
}

// 返回 [min, max] 范围内的整数
static int RandomRange(int min, int max) {
    if (min > max) { int t = min; min = max; max = t; }
    uint32_t range = (uint32_t)(max - min + 1);
    return min + (int)(RandomU32() % range);
}

// 随机 0~255 颜色分量
static uint8_t RandomByte() {
    return (uint8_t)(RandomU32() & 0xFF);
}

// ===================== 扫描线填充三角形 =====================
void BasicDraw::FillTriangle(int x0, int y0, int x1, int y1, int x2, int y2, uint32_t color) {
    // 按 y 排序顶点 (y0 <= y1 <= y2)
    if (y0 > y1) { int tx = x0; x0 = x1; x1 = tx; int ty = y0; y0 = y1; y1 = ty; }
    if (y0 > y2) { int tx = x0; x0 = x2; x2 = tx; int ty = y0; y0 = y2; y2 = ty; }
    if (y1 > y2) { int tx = x1; x1 = x2; x2 = tx; int ty = y1; y1 = y2; y2 = ty; }

    // 高度为0直接画线
    if (y2 - y0 == 0) {
        DrawHLine(x0 < x1 ? (x0 < x2 ? x0 : x2) : (x1 < x2 ? x1 : x2), y0,
                  __builtin_fabs((x0 > x1 ? (x0 > x2 ? x0 : x2) : (x1 > x2 ? x1 : x2)) -
                      (x0 < x1 ? (x0 < x2 ? x0 : x2) : (x1 < x2 ? x1 : x2))) + 1, color);
        return;
    }

    int totalHeight = y2 - y0;
    for (int y = y0; y <= y2; y++) {
        int secondHalf = (y > y1 || y1 == y0) ? 1 : 0;
        int segmentHeight = secondHalf ? (y2 - y1) : (y1 - y0);
        if (segmentHeight == 0) continue; // 防止除零

        float alpha = (float)(y - y0) / totalHeight;
        float beta  = (float)(secondHalf ? (y - y1) : (y - y0)) / (segmentHeight > 0 ? segmentHeight : 1);

        int A = x0 + (int)((x2 - x0) * alpha);
        int B = secondHalf ? (x1 + (int)((x2 - x1) * beta)) : (x0 + (int)((x1 - x0) * beta));

        if (A > B) { int temp = A; A = B; B = temp; }

        // 边界修正，确保不超出屏幕
        if (y < 0 || y >= (int)FrameBuf->Height) continue;
        if (A >= (int)FrameBuf->Width) continue;
        if (B < 0) continue;
        if (A < 0) A = 0;
        if (B >= (int)FrameBuf->Width) B = (int)FrameBuf->Width - 1;

        DrawHLine((uint64_t)A, (uint64_t)y, (uint64_t)(B - A + 1), color);
    }
}
static int clamp(int v, int min, int max) {
    if (v < min) return min;
    if (v > max) return max;
    return v;
}
// 生成随机颜色（全不透明）
uint32_t RandomColor() {
    // 扩展色板 —— 20 种暗调高级色
    uint32_t palette[] = {
        // 冷色系
        0xFF1A1C3B, // 深海军蓝
        0xFF2A2E5A, // 暗蓝
        0xFF3A2E6B, // 深蓝紫 (原)
        0xFF4B3A7C, // 紫罗兰
        0xFF5B3A8C, // 紫 (原)
        0xFF1E3A5F, // 深海蓝
        0xFF1B4A6F, // 暗青
        0xFF2E5A7A, // 深青 (原)
        // 冷暖过渡
        0xFF5C4A6E, // 灰紫
        0xFF6B4A7A, // 淡紫
        0xFF7A4A5C, // 紫红
        0xFF8B4556, // 暗玫瑰
        // 暖色系
        0xFF9B4B83, // 品红 (原)
        0xFFB8457D, // 粉紫 (原)
        0xFFB84B4B, // 暗红
        0xFFC05A3A, // 砖红
        0xFFC06A2E, // 暗橙
        0xFFB07A25, // 暗琥珀
        0xFF8C6B3A, // 古铜
        0xFF6B5A4A  // 深褐
    };

    // 随机选中一个基础色
    uint32_t base = palette[RandomU32() % (sizeof(palette) / sizeof(palette[0]))];

    int r = (base >> 16) & 0xFF;
    int g = (base >> 8) & 0xFF;
    int b = base & 0xFF;

    // 轻微随机偏移，保留颜色间的微妙差异
    r = clamp(r + RandomRange(-10, 20), 0, 255);
    g = clamp(g + RandomRange(-10, 20), 0, 255);
    b = clamp(b + RandomRange(-10, 20), 0, 255);

    return (0xFF << 24) | (r << 16) | (g << 8) | b;
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

            // 限制调色板，避免过于刺眼的纯色
            uint32_t c1 = RandomColor();
            uint32_t c2 = RandomColor();
            float alpha = 0.7f;
            // 应用透明度（alpha 0~1，0全透，1全不透明）
            c1 = (uint32_t)(alpha * 255) << 24 | (c1 & 0x00FFFFFF);
            c2 = (uint32_t)(alpha * 255) << 24 | (c2 & 0x00FFFFFF);

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