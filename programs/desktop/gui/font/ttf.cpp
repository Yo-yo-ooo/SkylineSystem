#include <base/font/ttf/ttf.h>
#include <stdio.h>
#include <stdlib.h>
#include <gui/fb.h>
// 全局或特定进程的字体指针
TTF_Font* g_system_font = NULL;

// 初始化并加载系统字体
bool init_system_font(const char* path, int32_t pixel_height) {
    // 1. 创建字体对象，分配 512 个字形的缓存空间
    g_system_font = TTF_CreateFont(512);
    if (!g_system_font) return false;

    // 2. 打开文件系统中的 .ttf 文件 (利用你写的 VFS)
    // 假设内核中有 kopen/kread/kclose 接口
    FILE* fd = fopen(path, O_RDONLY);
    if (fd == nullptr) {
        TTF_DestroyFont(g_system_font);
        return false;
    }

    // 3. 获取文件大小 (利用你 FSOPS 里的 fsize 接口)
    uint64_t file_size = fsize(fd);
    if (file_size == 0) {
        fclose(fd);
        TTF_DestroyFont(g_system_font);
        return false;
    }

    // 4. 分配内存并读取文件内容 (利用你写的无锁 malloc)
    unsigned char* font_data = (unsigned char*)malloc(file_size);
    if (!font_data) {
        fclose(fd);
        TTF_DestroyFont(g_system_font);
        return false;
    }

    fread(fd, font_data, file_size);
    fclose(fd);

    // 5. 将内存数据交给 TTF 库解析
    bool success = TTF_LoadFontFromMemory(g_system_font, font_data, file_size, pixel_height);
    
    // 注意：TTF_LoadFontFromMemory 内部会 memcpy 一份数据 (font->data)
    // 所以这里的 font_data 读取完成后可以直接释放掉
    free(font_data);

    if (!success) {
        TTF_DestroyFont(g_system_font);
        g_system_font = NULL;
        return false;
    }

    //开启抗锯齿超采样和加粗
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
        // 将灰度图 bmp.pixels 混合到你的 Framebuffer (显存) 中
        // x, y 是起始坐标，color 是字体颜色 (RGB)
        ///framebuffer_draw_bitmap_alpha(x, y, bmp.width, bmp.height, bmp.pixels, color);
        //FB->BaseAddress[x * y] = color;
        
        // 渲染完务必释放位图内存
        TTF_FreeBitmap(&bmp);
    }
}