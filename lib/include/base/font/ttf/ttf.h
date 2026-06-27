#ifndef TTF_ENGINE_H
#define TTF_ENGINE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif



// ==================== 核心数据结构 ====================

typedef struct {
    unsigned char *pixels;
    int width;
    int height;
} TTF_Bitmap;

typedef struct TTF_Font_Internal TTF_Font;

typedef enum {
    TTF_ALIGN_LEFT = 0,
    TTF_ALIGN_CENTER,
    TTF_ALIGN_RIGHT
} TTF_Align;

typedef struct {
    unsigned long hit_count;
    unsigned long miss_count;
} TTF_CacheStats;

// ==================== API ====================

TTF_Font* TTF_CreateFont(int cache_capacity);
void TTF_DestroyFont(TTF_Font* font);

bool TTF_LoadFontFromMemory(TTF_Font *font, const unsigned char *data, size_t data_size, int pixel_height);
void TTF_SetPixelHeight(TTF_Font *font, int pixel_height);

// [新增] 设置超采样抗锯齿等级 (1=默认, 2/3/4=高清)
void TTF_SetOversampling(TTF_Font *font, int oversampling);

// [新增] 设置合成样式 (粗体强度 0-3, 斜体倾斜度 0.0-0.5)
void TTF_SetFontStyle(TTF_Font *font, int bold_strength, float italic_skew);

void TTF_GetTextSize(TTF_Font *font, const char *text, int *out_width, int *out_height);

TTF_Bitmap TTF_RenderChar(TTF_Font *font, int codepoint, int *out_x_offset, int *out_y_offset);
TTF_Bitmap TTF_RenderText(TTF_Font *font, const char *text);
void TTF_FreeBitmap(TTF_Bitmap *bitmap);
void TTF_ClearGlyphCache(TTF_Font *font);

const TTF_Bitmap* TTF_GetGlyphBitmap(TTF_Font *font, int codepoint, int *out_advance, int *out_off_x, int *out_off_y);

bool TTF_RenderTextToBuffer(TTF_Font *font, const char *text, TTF_Bitmap *out_bmp);

// 多行文本渲染 (支持 CJK 自动断行)
TTF_Bitmap TTF_RenderTextMultiline(TTF_Font *font, const char *text, int max_width, TTF_Align align, int line_gap);

void TTF_GetCacheStats(TTF_Font *font, TTF_CacheStats *out_stats);

#ifdef __cplusplus
}
#endif

#endif // TTF_ENGINE_H