#ifndef TTF_ENGINE_H
#define TTF_ENGINE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <graphic/fb.h>

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
typedef struct {
    unsigned long hit_count;      /* 图集命中(免光栅) */
    unsigned long miss_count;     /* 首次见到的字形(光栅化+入集) */
    unsigned long evict_count;    /* LRU 重建次数 */
    int32_t       page_count;     /* 图集页数 */
    unsigned long atlas_bytes;    /* 当前图集像素内存 */
    unsigned long atlas_max_bytes;
    int32_t       glyph_count;    /* 已缓存字形条目 */
    int32_t       glyph_max;
} TTF_AtlasStats;

// ==================== API ====================
void TTF_GetAtlasStats(TTF_Font *font, TTF_AtlasStats *out_stats);

TTF_Font* TTF_CreateFont(int32_t cache_capacity);
void TTF_DestroyFont(TTF_Font* font);

bool TTF_LoadFontFromMemory(TTF_Font *font, const unsigned char *data, size_t data_size, int32_t pixel_height);
void TTF_SetPixelHeight(TTF_Font *font, int32_t pixel_height);

// 设置超采样抗锯齿等级 (1=默认, 2/3/4=高清)
void TTF_SetOversampling(TTF_Font *font, int32_t oversampling);

// 设置合成样式 (粗体强度 0-3, 斜体倾斜度 0.0-0.5)
void TTF_SetFontStyle(TTF_Font *font, int32_t bold_strength, float italic_skew);

void TTF_GetTextSize(TTF_Font *font, const char *text, int32_t *out_width, int32_t *out_height);
int32_t TTF_GetLineHeight(TTF_Font *font);

TTF_Bitmap TTF_RenderChar(TTF_Font *font, int32_t codepoint, int32_t *out_x_offset, int32_t *out_y_offset);
TTF_Bitmap TTF_RenderText(TTF_Font *font, const char *text);
void TTF_FreeBitmap(TTF_Bitmap *bitmap);
void TTF_ClearGlyphCache(TTF_Font *font);

const TTF_Bitmap* TTF_GetGlyphBitmap(TTF_Font *font, int32_t codepoint, int32_t *out_advance, int32_t *out_off_x, int32_t *out_off_y);

bool TTF_RenderTextToBuffer(TTF_Font *font, const char *text, TTF_Bitmap *out_bmp);

// 多行文本渲染 (支持 CJK 自动断行)
TTF_Bitmap TTF_RenderTextMultiline(TTF_Font *font, const char *text, int32_t max_width, TTF_Align align, int32_t line_gap);

void TTF_GetCacheStats(TTF_Font *font, TTF_CacheStats *out_stats);

uint8_t TTF_ReadFont(
    TTF_Font **out_font, const char* path, 
    int32_t pixel_height, int32_t CacheCap
);

/* On-demand variant: reads the sfnt directory and fetches only the tables the
   rasterizer uses (cmap/glyf/head/hhea/hmtx/loca/maxp), one seek+read each,
   instead of copying the whole file. Falls back to TTF_ReadFont() for TTC
   collections or a missing table. See ttf.c for what this does and does not
   save (glyf dominates CJK fonts). */
uint8_t TTF_ReadFontLazy(
    TTF_Font **out_font, const char* path,
    int32_t pixel_height, int32_t CacheCap
);

/* Glyph-granular on-demand variant. Like TTF_ReadFontLazy() it keeps only the
   small tables resident, but `glyf` (the outline data, ~96% of a CJK font)
   stays on disk too: the font image carries a glyf *directory entry* that
   points at a sliding window, and one glyph slice is read + its two `loca`
   entries rewritten whenever that glyph is first measured or rasterized.
   Composite glyphs pull their components in with them. Resident memory drops
   from "whole file" to "small tables + window" (SourceHanSerifTC: ~30 MB ->
   ~1.5 MB). Degrades to TTF_ReadFontLazy() for TTC, short-loca or odd fonts. */
uint8_t TTF_ReadFontGlyfLazy(
    TTF_Font **out_font, const char* path,
    int32_t pixel_height, int32_t CacheCap
);

/* Observability for the glyph window (all zeros for non-lazy fonts):
   resident = bytes of outline currently held, cap = window size,
   fetches = slice reads issued, since_load = bytes read from the file. */
void TTF_GetGlyfStats(TTF_Font *font, unsigned long *out_cap,
                      unsigned long *out_resident, unsigned long *out_fetches,
                      unsigned long *out_bytes);

void TTF_DrawText(
    FrameBuffer *FB, TTF_Font *TTFFont,
    int32_t x, int32_t y, const char* text, uint32_t color
);

#ifdef __cplusplus
}
#endif

#endif // TTF_ENGINE_H