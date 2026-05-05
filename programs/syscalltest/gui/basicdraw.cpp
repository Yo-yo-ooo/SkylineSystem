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
    
    float scaleX = (float)W / 1920.0f;
    float scaleY = (float)H / 1080.0f;
    float scale = (scaleX + scaleY) / 2.0f;

    uint64_t cx = W / 2;
    uint64_t cy = H / 2;
    
    // 预计算部分常数（提高一点点性能）
    float maxDist = sqrt((float)(cx*cx) + (float)(cy*cy));

    // ========== 1. 模拟 Win11 Bloom 的彩色背景 ==========
    for (uint64_t y = 0; y < H; y++) {
        for (uint64_t x = 0; x < W; x++) {
            float dx = (float)x - (float)cx;
            float dy = (float)y - (float)cy;
            float dist = sqrt(dx*dx + dy*dy);
            float angle = atan2(dy, dx);
            
            // --- 核心改动 1: 把黑白切割改为平滑过渡 ---
            // 原代码: pattern > 0 ? 0xFF : 0x00  (很硬)
            float pattern = sin(dist * 0.02f + angle * 4.0f); 
            // 平滑灰度: 将 [-1,1] 映射到 [0,1]
            float smoothing = (pattern + 1.0f) * 0.5f; 

            // --- 核心改动 2: 从纯灰度变成 Win11 彩色 ---
            // 根据角度分配不同的色调：品红 -> 紫色 -> 青色 -> 粉红
            float hueAngle = angle + 1.57f; // 偏移角度让配色更自然
            
            // 用 sin 生成柔和的 RGB 分量
            float r = sin(hueAngle) * 0.5f + 0.5f;
            float g = sin(hueAngle + 2.094f) * 0.5f + 0.5f; // +120度
            float b = sin(hueAngle + 4.189f) * 0.5f + 0.5f; // +240度
            
            // 混合：让灰度值决定彩色亮度 (越亮的地方彩色越饱和)
            uint8_t finalR = (uint8_t)(smoothing * r * 255.0f);
            uint8_t finalG = (uint8_t)(smoothing * g * 255.0f);
            uint8_t finalB = (uint8_t)(smoothing * b * 255.0f);

            // --- 核心改动 3: 中心发光 + 正圆边缘渐暗 ---
            float edgeFade = dist / (maxDist * 0.85f); // 85% 距离处开始渐黑
            if (edgeFade > 1.0f) edgeFade = 1.0f;
            
            // 让中心区域更亮 (发光效果)
            float glow = 1.0f - (dist / (maxDist * 0.3f)); 
            if (glow < 0.0f) glow = 0.0f;
            
            // 合成最终亮度 (边缘渐暗 + 中心发光)
            float finalBrightness = (1.0f - edgeFade) + glow * 0.5f;
            if (finalBrightness > 1.0f) finalBrightness = 1.0f;

            finalR = (uint8_t)(finalR * finalBrightness);
            finalG = (uint8_t)(finalG * finalBrightness);
            finalB = (uint8_t)(finalB * finalBrightness);

            // 输出颜色 (0xFF 是不透明 + RGB)
            PutPixel(x, y, 0xFF000000 | (finalR << 16) | (finalG << 8) | finalB);
        }
    }
}