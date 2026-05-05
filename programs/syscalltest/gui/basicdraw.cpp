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
    uint64_t w = FrameBuf->Width;
    uint64_t h = FrameBuf->Height;

    float fw = (float)w;
    float fh = (float)h;

    for (uint64_t y = 0; y < h; y++) {
        uint32_t* row = (uint32_t*)FrameBuf->BaseAddress + (y * FrameBuf->PixelsPerScanLine);
        float fy = (float)y / fh;   // 0~1

        for (uint64_t x = 0; x < w; x++) {
            float fx = (float)x / fw;   // 0~1

            // ====== 1. 深蓝背景色 ======
            float r = 0.02f;
            float g = 0.08f;
            float b = 0.20f;

            // ====== 2. 主球体（圆心偏左、偏上） ======
            // 球心坐标
            float cx = 0.30f, cy = 0.40f;
            float radius = 0.70f;

            float dx = fx - cx;
            float dy = fy - cy;
            float dist = sqrt(dx*dx + dy*dy);
            float dist_normal = dist / radius;  // 越靠近0在球心

            // 球体范围（0-1之间，外围为0）
            float sphere = 1.0f - (dist / radius);
            if (sphere < 0.0f) sphere = 0.0f;
            float sphere_smooth = sphere * sphere; // 让衰减平滑

            // 球体颜色（中心亮蓝 -> 边缘深蓝紫）
            float blue_r = 0.05f + 0.35f * sphere_smooth;
            float blue_g = 0.10f + 0.55f * sphere_smooth;
            float blue_b = 0.40f + 0.50f * sphere_smooth;

            // 混合球体到背景
            r = r + (blue_r - r) * sphere_smooth * 0.95f;
            g = g + (blue_g - g) * sphere_smooth * 0.95f;
            b = b + (blue_b - b) * sphere_smooth * 0.95f;

            // ====== 3. 右侧紫红色发光 (球体右侧和右下) ======
            float purple_cx = 0.85f, purple_cy = 0.70f;
            float purple_radius = 0.60f;
            
            float pdx = fx - purple_cx;
            float pdy = fy - purple_cy;
            float pdist = sqrt(pdx*pdx + pdy*pdy);
            float pdist_norm = pdist / purple_radius;
            float purple = 1.0f - pdist_norm;
            if (purple < 0.0f) purple = 0.0f;
            purple = purple * purple * 1.2f; // 强发光

            float purple_r = 0.55f + 0.40f * purple;
            float purple_g = 0.05f + 0.30f * purple;
            float purple_b = 0.45f + 0.40f * purple;

            // 紫色叠加，外部影响更大（让球体边缘带紫）
            float purpleFactor = (1.0f - sphere_smooth * 0.8f) * purple * 0.8f;
            r = r + (purple_r - r) * purpleFactor;
            g = g + (purple_g - g) * purpleFactor;
            b = b + (purple_b - b) * purpleFactor;

            // ====== 4. 左侧青蓝环境光 ======
            float tealFactor = (1.0f - fx) * 0.25f * (1.0f - (fy - 0.5f)*(fy - 0.5f)*3.0f);
            if (tealFactor < 0.0f) tealFactor = 0.0f;
            r += 0.05f * tealFactor;
            g += 0.25f * tealFactor;
            b += 0.35f * tealFactor;

            // ====== 5. 底部亮边（粉白霓虹高光） ======
            float glowY = 0.92f;
            float glowX = 0.35f;  // 底部发光大概在水平这个位置
            float glowWidth = 0.45f;
            
            float gdx = fx - glowX;
            float gdy = fy - glowY;
            float gdist = sqrt(gdx*gdx + gdy*gdy);
            float glow = 1.0f - gdist / glowWidth;
            if (glow < 0.0f) glow = 0.0f;
            glow = glow * glow * 2.5f;

            // 粉白颜色
            float glow_r = 0.95f * glow;
            float glow_g = 0.70f * glow;
            float glow_b = 0.80f * glow;

/*             // 叠加底部发光的（注意只影响底部和中间区域）
            float glowFactor = glow * (1.0f - fy*0.5f);
            if (glowFactor > 1.0f) glowFactor = 1.0f;
            r = r + (glow_r - r) * glowFactor;
            g = g + (glow_g - g) * glowFactor;
            b = b + (glow_b - b) * glowFactor; */

            // ====== 6. 上方微弱的暗色柔和边缘 ======
            float topFade = fy * fy * 0.2f;
            r -= 0.02f * topFade;
            g -= 0.05f * topFade;
            b -= 0.10f * topFade;

            // ====== 7. 保护边界 [0,1] ======
            if (r < 0.0f) r = 0.0f;  if (r > 1.0f) r = 1.0f;
            if (g < 0.0f) g = 0.0f;  if (g > 1.0f) g = 1.0f;
            if (b < 0.0f) b = 0.0f;  if (b > 1.0f) b = 1.0f;

            // ====== 8. 转成 ARGB 写入 ======
            uint8_t rr = (uint8_t)(r * 255.0f);
            uint8_t gg = (uint8_t)(g * 255.0f);
            uint8_t bb = (uint8_t)(b * 255.0f);
            row[x] = (0xFF << 24) | (rr << 16) | (gg << 8) | bb;
        }
    }
}