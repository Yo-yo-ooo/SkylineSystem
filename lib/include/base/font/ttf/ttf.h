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
    int32_t width;
    int32_t height;
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

TTF_Font* TTF_CreateFont(int32_t cache_capacity);
void TTF_DestroyFont(TTF_Font* font);

bool TTF_LoadFontFromMemory(TTF_Font *font, const unsigned char *data, size_t data_size, int32_t pixel_height);
void TTF_SetPixelHeight(TTF_Font *font, int32_t pixel_height);

// 设置超采样抗锯齿等级 (1=默认, 2/3/4=高清)
void TTF_SetOversampling(TTF_Font *font, int32_t oversampling);

// 设置合成样式 (粗体强度 0-3, 斜体倾斜度 0.0-0.5)
void TTF_SetFontStyle(TTF_Font *font, int32_t bold_strength, float italic_skew);

void TTF_GetTextSize(TTF_Font *font, const char *text, int32_t *out_width, int32_t *out_height);

TTF_Bitmap TTF_RenderChar(TTF_Font *font, int32_t codepoint, int32_t *out_x_offset, int32_t *out_y_offset);
TTF_Bitmap TTF_RenderText(TTF_Font *font, const char *text);
void TTF_FreeBitmap(TTF_Bitmap *bitmap);
void TTF_ClearGlyphCache(TTF_Font *font);

const TTF_Bitmap* TTF_GetGlyphBitmap(TTF_Font *font, int32_t codepoint, int32_t *out_advance, int32_t *out_off_x, int32_t *out_off_y);

bool TTF_RenderTextToBuffer(TTF_Font *font, const char *text, TTF_Bitmap *out_bmp);

// 多行文本渲染 (支持 CJK 自动断行)
TTF_Bitmap TTF_RenderTextMultiline(TTF_Font *font, const char *text, int32_t max_width, TTF_Align align, int32_t line_gap);

void TTF_GetCacheStats(TTF_Font *font, TTF_CacheStats *out_stats);

#ifdef __cplusplus
}
#endif

#endif // TTF_ENGINE_H