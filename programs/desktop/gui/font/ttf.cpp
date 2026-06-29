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

// 在桌面上绘制文字的示例
void draw_text_example(FrameBuffer *FB,int32_t x, int32_t y, const char* text, uint32_t color) {
    if (!g_system_font) return;

    // 渲染文本到位图
    TTF_Bitmap bmp = TTF_RenderText(g_system_font, text);
    if (bmp.pixels) {
        // 将灰度图 bmp.pixels 混合到 Framebuffer (显存) 中
        // x, y 是起始坐标，color 是字体颜色 (RGB)
        ///framebuffer_draw_bitmap_alpha(x, y, bmp.width, bmp.height, bmp.pixels, color);
        //FB->BaseAddress[x * y] = color;
        
        // 渲染完务必释放位图内存
        TTF_FreeBitmap(&bmp);
    }
}