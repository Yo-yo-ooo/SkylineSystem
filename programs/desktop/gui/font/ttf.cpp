#include <base/font/ttf/ttf.h>
#include <stdio.h>
#include <stdlib.h>
#include <gui/fb.h>
// 全局或特定进程的字体指针
TTF_Font* g_system_font = NULL;

// 初始化并加载系统字体
bool init_system_font(const char* path, int32_t pixel_height) {
    g_system_font = TTF_CreateFont(512);
    if (!g_system_font) return false;

    FILE* fd = fopen(path, O_RDONLY);
    if (fd == nullptr) {
        TTF_DestroyFont(g_system_font);
        return false;
    }

    uint64_t file_size = fsize(fd);
    if (file_size == 0) {
        fclose(fd);
        TTF_DestroyFont(g_system_font);
        return false;
    }

    unsigned char* font_data = (unsigned char*)malloc(file_size);
    if (!font_data) {
        fclose(fd);
        TTF_DestroyFont(g_system_font);
        return false;
    }

    fread(font_data, file_size,1,fd);
    fclose(fd);

    bool success = TTF_LoadFontFromMemory(g_system_font, font_data, file_size, pixel_height);
    
    free(font_data);

    if (!success) {
        TTF_DestroyFont(g_system_font);
        g_system_font = NULL;
        return false;
    }

    TTF_SetOversampling(g_system_font, 2); // 2x2 超采样
    // TTF_SetFontStyle(g_system_font, 1, 0.2f); // 轻微加粗和斜体

    return true;
}

void draw_text_example(FrameBuffer *FB, int32_t x, int32_t y, const char* text, uint32_t color) {
    if (!g_system_font || !FB || !FB->BaseAddress) return;

    // 渲染文本到位图
    TTF_Bitmap bmp = TTF_RenderText(g_system_font, text);
    if (bmp.pixels && bmp.width > 0 && bmp.height > 0) {
        
        uint32_t* fb_ptr = (uint32_t*)FB->BaseAddress;
        int32_t fb_w = (int32_t)FB->Width;
        int32_t fb_h = (int32_t)FB->Height;
        // 必须使用 PixelsPerScanLine，因为硬件行宽可能大于分辨率宽度
        int32_t fb_pitch = (int32_t)FB->PixelsPerScanLine; 

        // 提取目标颜色 RGB 分量
        uint8_t src_r = (color >> 16) & 0xFF;
        uint8_t src_g = (color >> 8) & 0xFF;
        uint8_t src_b = color & 0xFF;

        // 注意修复了 for 循环的语法
        for (uint64_t y2 = 0; y2 < bmp.height; y2++) {
            for (uint64_t x2 = 0; x2 < bmp.width; x2++) {
                
                // 1. 计算屏幕上的绝对坐标
                int32_t px = x + x2;
                int32_t py = y + y2;

                // 2. 严格边界裁剪 (防止写溢出显存导致 Triple Fault)
                if (px < 0 || px >= fb_w || py < 0 || py >= fb_h) continue;

                // 3. 从灰度图中读取 Alpha 透明度 (0~255)
                uint8_t alpha = bmp.pixels[y2 * bmp.width + x2];
                
                // 完全透明的像素直接跳过，不读写显存，提升性能
                if (alpha == 0) continue;

                // 4. 计算显存线性地址
                uint32_t* dst_pixel = &fb_ptr[py * fb_pitch + px];

                // 完全不透明，直接覆盖
                if (alpha == 255) {
                    *dst_pixel = 0xFF000000 | color; // 补齐 Alpha 通道为 255
                    continue;
                }

                // 5. 执行 Alpha 混合 (Src * A + Dst * (1-A)) / 255
                uint32_t bg = *dst_pixel;
                uint8_t bg_r = (bg >> 16) & 0xFF;
                uint8_t bg_g = (bg >> 8) & 0xFF;
                uint8_t bg_b = bg & 0xFF;

                // 使用快速整数近似除以 255: (val + 128) >> 8
                uint8_t mix_r = (src_r * alpha + bg_r * (255 - alpha) + 128) >> 8;
                uint8_t mix_g = (src_g * alpha + bg_g * (255 - alpha) + 128) >> 8;
                uint8_t mix_b = (src_b * alpha + bg_b * (255 - alpha) + 128) >> 8;

                *dst_pixel = 0xFF000000 | (mix_r << 16) | (mix_g << 8) | mix_b;
            }
        }
        
        // 渲染完务必释放位图内存
        TTF_FreeBitmap(&bmp);
    }
}