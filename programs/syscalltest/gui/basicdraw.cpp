#include <gui/basicdraw.h>
#include <math.h>
/* float sqrt(float x){
    float buf;
    __asm__ __volatile__ ("sqrtss %1, %0" : "=x"(buf) : "x"(x));
    return buf;
}
 */

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

/* float sin(float x){
    float ans;
    asm volatile("fsin" : "=t"(ans) : "0"(x));
    return ans;

}

float cos(float x){
#ifndef __x86_64__
    return sin(90.0 + x * 180 / pi);
#else
    float ans;
    asm volatile("fcos" : "=t"(ans) : "0"(x));
    return ans;
#endif
} */
// 替代 float atan2f(float y, float x)
float atan2(float y, float x) {
    float res;
    __asm__ __volatile__ (
        "flds %1\n"    // ST0 = x
        "flds %0\n"    // ST1 = y
        "fpatan\n"     // ST0 = atan2(ST1,ST0) = atan2(y,x)
        "fsts %2\n"    // 存结果
        : "+m"(y), "+m"(x), "=m"(res)
        :
        : "memory"
    );
    return res;
}

void BasicDraw::DrawProportionalUI() {
    uint64_t W = FrameBuf->Width;
    uint64_t H = FrameBuf->Height;
    if (W == 0 || H == 0) return;

    uint64_t cx = W / 2;
    uint64_t cy = H / 2;
    float maxDist = sqrt((float)(cx*cx) + (float)(cy*cy));

    // 循环绘制每一个像素
    for (uint64_t y = 0; y < H; y++) {
        for (uint64_t x = 0; x < W; x++) {
            float dx = (float)x - (float)cx;
            float dy = (float)y - (float)cy;
            float dist = sqrt(dx*dx + dy*dy);
            float angle = atan2(dy, dx); // 当前像素相对于中心的角度

            // 距离中心的比例 (0~1)
            float t = dist / maxDist;

            // ---- 核心颜色计算公式 ----
            // 利用角度偏移产生 "花瓣" 形状的流线感
            // 这里混合了 3 种 Win11 常用主色：深青蓝、品红、暖金
            float r, g, b;

            // 1. 基底：深蓝紫色 (基础深色背景)
            float baseR = 0.05f; 
            float baseG = 0.05f;
            float baseB = 0.10f;

            // 2. 流线 1 (品红/粉)：cos(angle * 2) 会产生对称的两片花瓣，分别从中心指向两侧
            float flow1 = cos(angle * 2.0f + 0.5f) * 0.5f + 0.5f;
            flow1 *= (1.0f - t) * (1.0f - t); // 越靠近中心越亮
            r += flow1 * 0.5f; 
            b += flow1 * 0.4f;

            // 3. 流线 2 (亮青/蓝)：偏移角度使得另一片花瓣
            float flow2 = cos(angle * 2.0f - 1.2f) * 0.5f + 0.5f;
            flow2 *= (1.0f - t) * 0.8f;
            b += flow2 * 0.6f; 
            g += flow2 * 0.4f;

            // 4. 流线 3 (暖金/散射光芒)：在更外围提供补充色
            float flow3 = sin(angle * 3.0f) * 0.5f + 0.5f;
            flow3 *= t; // 在边缘更加明显
            r += flow3 * 0.15f;
            g += flow3 * 0.10f;

            // 5. 中央光晕 (始终加亮中心，模仿 Win11 的聚光效果)
            float glow = 1.0f - t;
            if (glow < 0.0f) glow = 0.0f;
            glow = glow * glow * 0.3f; // 中心高强度发光
            r += glow * 0.3f;
            g += glow * 0.3f;
            b += glow * 0.3f;

            // 6. 边缘衰减 (让四周最终变成均匀的深色)
            float edgeFade = 1.0f - t * 1.2f;
            if (edgeFade < 0.0f) edgeFade = 0.0f;
            r = r * edgeFade;
            g = g * edgeFade;
            b = b * edgeFade;

            // 限制范围并转换为 0~255
            if (r > 1.0f) r = 1.0f;
            if (g > 1.0f) g = 1.0f;
            if (b > 1.0f) b = 1.0f;
            
            uint8_t R = (uint8_t)(r * 255.0f);
            uint8_t G = (uint8_t)(g * 255.0f);
            uint8_t B = (uint8_t)(b * 255.0f);

            // 最终颜色 (0xFF 是不透明)
            PutPixel(x, y, 0xFF000000 | (R << 16) | (G << 8) | B);
        }
    }
}