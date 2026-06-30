#include <base/arch/x86_64/syscall.h>
#include <graphic/fb.h>

FrameBuffer GetFBInfo() {
    FrameBuffer fb;
    fb.BaseAddress = (void*)syscall(26,6,0,0,0,0,0);
    fb.BufferSize = syscall(26,6,0,1,0,0,0);
    fb.Height = syscall(26,6,0,2,0,0,0);
    fb.PixelsPerScanLine = syscall(26,6,0,3,0,0,0);
    fb.Width = syscall(26,6,0,4,0,0,0);
    return fb;
}

uint64_t MapFB() {
    return syscall(21,6,0,0,0,0,0);
}

void PutPixel(FrameBuffer *FB, uint64_t x, uint64_t y, Color color) {
    // 越界检查
    if (x >= FB->Width || y >= FB->Height) {
        return;
    }
    
    // 计算像素在显存中的地址
    // 注意：显存行与行之间可能有间隔，所以必须用 PixelsPerScanLine 而不是 Width
    uint32_t *pixel_ptr = (uint32_t *)FB->BaseAddress;
    uint64_t index = y * FB->PixelsPerScanLine + x;
    
    pixel_ptr[index] = color;
}

// 画实心矩形
void DrawFillRect(
    FrameBuffer *FB, uint64_t x, uint64_t y, 
    uint64_t width, uint64_t height, Color color
) {
    for (uint64_t current_y = 0; current_y < height; current_y++) {
        // 优化：直接计算每一行的起始地址进行写入，比每次调用 PutPixel 更快
        if (y + current_y >= FB->Height) break;
        
        uint32_t *pixel_ptr = (uint32_t *)FB->BaseAddress;
        uint64_t row_start_index = (y + current_y) * FB->PixelsPerScanLine + x;
        
        for (uint64_t current_x = 0; current_x < width; current_x++) {
            if (x + current_x >= FB->Width) break;
            pixel_ptr[row_start_index + current_x] = color;
        }
    }
}

// 画空心矩形 (线框)
void DrawRect(
    FrameBuffer *FB, uint64_t x, uint64_t y, 
    uint64_t width, uint64_t height, Color color
) {
    // 画上下两条线
    DrawFillRect(FB, x, y, width, 1, color);             // Top
    DrawFillRect(FB, x, y + height - 1, width, 1, color); // Bottom
    
    // 画左右两条线
    DrawFillRect(FB, x, y, 1, height, color);             // Left
    DrawFillRect(FB, x + width - 1, y, 1, height, color); // Right
}

void DrawLine(FrameBuffer *FB, int64_t x0, int64_t y0, int64_t x1, int64_t y1, Color color) {
    int64_t dx = x1 - x0;
    if (dx < 0) dx = -dx;
    
    int64_t dy = y1 - y0;
    if (dy < 0) dy = -dy;
    
    int64_t sx = (x0 < x1) ? 1 : -1;
    int64_t sy = (y0 < y1) ? 1 : -1;
    int64_t err = dx - dy;

    while (1) {
        PutPixel(FB, x0, y0, color);
        
        if (x0 == x1 && y0 == y1) break;
        
        int64_t e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            x0 += sx;
        }
        if (e2 < dx) {
            err += dx;
            y0 += sy;
        }
    }
}

void DrawCircle(
    FrameBuffer *FB, uint64_t center_x, 
    uint64_t center_y, uint64_t radius, Color color
) {
    if (radius == 0) {
        PutPixel(FB, center_x, center_y, color);
        return;
    }

    int64_t x = radius;
    int64_t y = 0;
    int64_t err = 0;

    while (x >= y) {
        // 绘制八对称点
        PutPixel(FB, center_x + x, center_y + y, color);
        PutPixel(FB, center_x + y, center_y + x, color);
        PutPixel(FB, center_y - y, center_y + x, color);
        PutPixel(FB, center_x - x, center_y + y, color);
        PutPixel(FB, center_x - x, center_y - y, color);
        PutPixel(FB, center_x - y, center_y - x, color);
        PutPixel(FB, center_x + y, center_y - x, color);
        PutPixel(FB, center_x + x, center_y - y, color);

        if (err <= 0) {
            y += 1;
            err += 2 * y + 1;
        }
        if (err > 0) {
            x -= 1;
            err -= 2 * x + 1;
        }
    }
}

void DrawBitmap(
    FrameBuffer *FB, uint64_t x, uint64_t y, 
    uint64_t img_width, uint64_t img_height, const uint32_t *image_data
) {
    for (uint64_t iy = 0; iy < img_height; iy++) {
        if (y + iy >= FB->Height) break;
        
        uint32_t *pixel_ptr = (uint32_t *)FB->BaseAddress;
        uint64_t fb_row_start = (y + iy) * FB->PixelsPerScanLine + x;
        uint64_t img_row_start = iy * img_width;
        
        for (uint64_t ix = 0; ix < img_width; ix++) {
            if (x + ix >= FB->Width) break;
            
            uint32_t color = image_data[img_row_start + ix];
            // 简单的透明度测试：如果颜色不是完全透明的，就画上去
            if ((color & 0xFF000000) != 0) { 
                pixel_ptr[fb_row_start + ix] = color;
            }
        }
    }
}

void ClearScreen(FrameBuffer *FB, Color color) {
    uint32_t *pixel_ptr = (uint32_t *)FB->BaseAddress;
    // 优化：直接遍历 PixelsPerScanLine * Height，避免嵌套循环的开销
    // 注意：这会覆盖 padding 区域，但这在清屏时是安全且更快的
    uint64_t total_pixels = FB->PixelsPerScanLine * FB->Height;
    
    for (uint64_t i = 0; i < total_pixels; i++) {
        pixel_ptr[i] = color;
    }
}