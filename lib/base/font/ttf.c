//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT
// ==================== 平台适配层 ====================
#ifndef TTF_MALLOC
#include <stdlib.h>
#define TTF_MALLOC(size) malloc(size)
#define TTF_REALLOC(ptr, size) realloc(ptr, size)
#define TTF_FREE(ptr) free(ptr)
#endif

#ifndef TTF_MEMSET
#include <string.h>
#define TTF_MEMSET(ptr, val, size) memset(ptr, val, size)
#endif

#ifndef TTF_MEMCPY
#include <string.h>
#define TTF_MEMCPY(dst, src, size) memcpy(dst, src, size)
#endif

#ifndef TTF_STRLEN
#include <string.h>
#define TTF_STRLEN(s) strlen(s)
#endif
#include <atomic/atomic.h>
#define TTF_MUTEX_TYPE atomic_flag
#define TTF_MUTEX_INIT(m)       atomic_clear(&(m), ATOMIC_RELEASE)
#define TTF_MUTEX_LOCK(m)       while(atomic_test_and_set(&(m), ATOMIC_ACQUIRE)){ __asm__ __volatile__("pause"); }
#define TTF_MUTEX_UNLOCK(m)     atomic_clear(&(m), ATOMIC_RELEASE)
#define TTF_MUTEX_DESTROY(m)

#define STB_TRUETYPE_IMPLEMENTATION
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wimplicit-function-declaration"
#include <base/font/ttf/stb_ttf.h>
#pragma GCC diagnostic pop
#include <base/font/ttf/ttf.h>

// ==================== 原子操作适配层 ====================
#if defined(__GNUC__) || defined(__clang__)
    #define TTF_ATOMIC_FETCH_ADD(ptr, val) atomic_fetch_add_n(ptr, val, ATOMIC_RELAXED)
    #define TTF_ATOMIC_LOAD(ptr) atomic_load_n(ptr, ATOMIC_RELAXED)
#else
    // 降级：非原子环境，直接操作（需由调用方 TTF_MUTEX 保证安全）
    #define TTF_ATOMIC_FETCH_ADD(ptr, val) (*(ptr) += (val))
    #define TTF_ATOMIC_LOAD(ptr) (*(ptr))
#endif

typedef struct {
    int32_t codepoint;
    int32_t advanceWidth;
    int32_t off_x;
    int32_t off_y;
    TTF_Bitmap bmp;
    int32_t next;
    int32_t lru_prev;
    int32_t lru_next;
} TTF_CacheNode;

struct TTF_Font_Internal {
    stbtt_fontinfo info;
    unsigned char *data; 
    float scale;
    int32_t pixel_height;
    int32_t ascent;
    bool is_initialized;

    int32_t cache_capacity;
    int32_t hash_size;
    TTF_CacheNode *cache_nodes;
    int32_t *hash_table;
    int32_t lru_head;
    int32_t lru_tail;
    int32_t free_list;

    TTF_MUTEX_TYPE lock;
    
    int32_t oversampling;
    int32_t bold_strength;
    float italic_skew;

    unsigned long hit_count;
    unsigned long miss_count;
};

typedef struct {
    int32_t codepoint;
    int32_t x_advance;
} TTF_RenderCmd;

// CJK 字符判断
static bool is_cjk_char(int32_t codepoint) {
    return (codepoint >= 0x4E00 && codepoint <= 0x9FFF) ||
           (codepoint >= 0x3400 && codepoint <= 0x4DBF) ||
           (codepoint >= 0x3040 && codepoint <= 0x30FF) ||
           (codepoint >= 0xAC00 && codepoint <= 0xD7AF) ||
           (codepoint >= 0x20000 && codepoint <= 0x2A6DF);
}

// CJK 标点避首规则判断
static bool is_punct_no_start(int32_t codepoint) {
    return codepoint == 0x3001 || codepoint == 0x3002 || 
           codepoint == 0xFF0C || codepoint == 0xFF1A || codepoint == 0xFF1B || 
           codepoint == 0xFF01 || codepoint == 0xFF1F || 
           codepoint == 0xFF09 || codepoint == 0x300D || codepoint == 0xFF5D || 
           codepoint == 0x300F || codepoint == 0x201D || codepoint == 0x2019;
}

static int32_t next_power_of_two(int32_t v) {
    if (v <= 0) return 16;
    v--;
    v |= v >> 1; v |= v >> 2; v |= v >> 4; v |= v >> 8; v |= v >> 16;
    return v + 1;
}

static int32_t utf8_to_codepoint(const char *str, int32_t str_len, int32_t *advance) {
    if (str_len <= 0) { *advance = 0; return 0xFFFD; }
    unsigned char c = (unsigned char)str[0];
    if (c < 0x80) { *advance = 1; return c; }
    else if ((c >> 5) == 0x06) {
        if (str_len < 2 || ((unsigned char)str[1] & 0xC0) != 0x80) goto invalid;
        *advance = 2; return ((c & 0x1F) << 6) | ((unsigned char)str[1] & 0x3F);
    } else if ((c >> 4) == 0x0E) {
        if (str_len < 3 || ((unsigned char)str[1] & 0xC0) != 0x80 || ((unsigned char)str[2] & 0xC0) != 0x80) goto invalid;
        *advance = 3; return ((c & 0x0F) << 12) | (((unsigned char)str[1] & 0x3F) << 6) | ((unsigned char)str[2] & 0x3F);
    } else if ((c >> 3) == 0x1E) {
        if (str_len < 4 || ((unsigned char)str[1] & 0xC0) != 0x80 || 
            ((unsigned char)str[2] & 0xC0) != 0x80 || ((unsigned char)str[3] & 0xC0) != 0x80) goto invalid;
        *advance = 4;
        return ((c & 0x07) << 18) | (((unsigned char)str[1] & 0x3F) << 12) |
               (((unsigned char)str[2] & 0x3F) << 6) | ((unsigned char)str[3] & 0x3F);
    }
invalid:
    *advance = 1; return 0xFFFD; 
}

static void cache_init(TTF_Font* font) {
    font->lru_head = -1; font->lru_tail = -1; font->free_list = 0;
    for (int32_t i = 0; i < font->hash_size; i++) font->hash_table[i] = -1;
    for (int32_t i = 0; i < font->cache_capacity; i++) {
        font->cache_nodes[i].lru_prev = -1;
        font->cache_nodes[i].lru_next = i + 1;
        font->cache_nodes[i].next = -1;
        font->cache_nodes[i].codepoint = -1;
        font->cache_nodes[i].bmp.pixels = NULL;
    }
    font->cache_nodes[font->cache_capacity - 1].lru_next = -1;
}

static void cache_unlink_lru(TTF_Font* font, int32_t idx) {
    TTF_CacheNode* node = &font->cache_nodes[idx];
    if (node->lru_prev != -1) font->cache_nodes[node->lru_prev].lru_next = node->lru_next;
    else font->lru_head = node->lru_next;
    if (node->lru_next != -1) font->cache_nodes[node->lru_next].lru_prev = node->lru_prev;
    else font->lru_tail = node->lru_prev;
    node->lru_prev = -1; node->lru_next = -1;
}

static void cache_push_front_lru(TTF_Font* font, int32_t idx) {
    TTF_CacheNode* node = &font->cache_nodes[idx];
    node->lru_prev = -1; node->lru_next = font->lru_head;
    if (font->lru_head != -1) font->cache_nodes[font->lru_head].lru_prev = idx;
    else font->lru_tail = idx;
    font->lru_head = idx;
}

static uint32_t get_hash(int32_t cp, int32_t hash_size) { return ((uint32_t)cp) & (hash_size - 1); }

static void hash_insert(TTF_Font* font, int32_t cp, int32_t idx) {
    uint32_t hash = get_hash(cp, font->hash_size);
    font->cache_nodes[idx].codepoint = cp;
    font->cache_nodes[idx].next = font->hash_table[hash];
    font->hash_table[hash] = idx;
}

static int32_t hash_find(TTF_Font* font, int32_t cp) {
    uint32_t hash = get_hash(cp, font->hash_size);
    int32_t idx = font->hash_table[hash];
    while (idx != -1) {
        if (font->cache_nodes[idx].codepoint == cp) return idx;
        idx = font->cache_nodes[idx].next;
    }
    return -1;
}

static void hash_remove(TTF_Font* font, int32_t cp) {
    uint32_t hash = get_hash(cp, font->hash_size);
    int32_t* p_idx = &font->hash_table[hash];
    while (*p_idx != -1) {
        if (font->cache_nodes[*p_idx].codepoint == cp) {
            int32_t target_idx = *p_idx;
            *p_idx = font->cache_nodes[target_idx].next;
            font->cache_nodes[target_idx].next = -1; 
            return;
        }
        p_idx = &font->cache_nodes[*p_idx].next;
    }
}

static void apply_italic_skew(TTF_Bitmap *bmp, float skew) {
    if (skew <= 0.0f || !bmp->pixels) return;
    int32_t new_w = bmp->width + (int32_t)(bmp->height * skew) + 1;
    unsigned char *new_pixels = (unsigned char*)TTF_MALLOC(new_w * bmp->height);
    if (!new_pixels) return;
    TTF_MEMSET(new_pixels, 0, new_w * bmp->height);

    for (int32_t y = 0; y < bmp->height; y++) {
        int32_t offset = (int32_t)((bmp->height - y) * skew);
        for (int32_t x = 0; x < bmp->width; x++) {
            new_pixels[y * new_w + x + offset] = bmp->pixels[y * bmp->width + x];
        }
    }
    TTF_FREE(bmp->pixels);
    bmp->pixels = new_pixels;
    bmp->width = new_w;
}

static void downsample_bilinear(TTF_Bitmap *bmp, int32_t target_w, int32_t target_h) {
    if (bmp->width == target_w && bmp->height == target_h) return;
    if (target_w <= 0 || target_h <= 0) return;

    unsigned char *dst = TTF_MALLOC(target_w * target_h);
    if (!dst) return;
    TTF_MEMSET(dst, 0, target_w * target_h);
    
    float scale_x = (float)bmp->width / target_w;
    float scale_y = (float)bmp->height / target_h;
    
    for (int32_t y = 0; y < target_h; y++) {
        float fy = y * scale_y;
        int32_t y0 = (int32_t)fy;
        int32_t y1 = y0 + 1;
        float dy = fy - y0;
        for (int32_t x = 0; x < target_w; x++) {
            float fx = x * scale_x;
            int32_t x0 = (int32_t)fx;
            int32_t x1 = x0 + 1;
            float dx = fx - x0;
            
            unsigned char c00 = bmp->pixels[y0 * bmp->width + x0];
            unsigned char c01 = (x1 < bmp->width) ? bmp->pixels[y0 * bmp->width + x1] : 0;
            unsigned char c10 = (y1 < bmp->height) ? bmp->pixels[y1 * bmp->width + x0] : 0;
            unsigned char c11 = (x1 < bmp->width && y1 < bmp->height) ? bmp->pixels[y1 * bmp->width + x1] : 0;
            
            float val = c00*(1-dx)*(1-dy) + c01*dx*(1-dy) + c10*(1-dx)*dy + c11*dx*dy;
            dst[y * target_w + x] = (unsigned char)(val + 0.5f);
        }
    }
    TTF_FREE(bmp->pixels);
    bmp->pixels = dst;
    bmp->width = target_w;
    bmp->height = target_h;
}

static TTF_CacheNode* get_glyph(TTF_Font* font, int32_t codepoint) {
    if (!font->is_initialized) return NULL;
    TTF_MUTEX_LOCK(font->lock);

    int32_t idx = hash_find(font, codepoint);
    if (idx != -1) {
        cache_unlink_lru(font, idx);
        cache_push_front_lru(font, idx);
        // 修复 1: 使用统一原子宏进行无锁化自增
        TTF_ATOMIC_FETCH_ADD(&font->hit_count, 1);
        TTF_CacheNode* node = &font->cache_nodes[idx];
        TTF_MUTEX_UNLOCK(font->lock);
        return node;
    }
    
    TTF_ATOMIC_FETCH_ADD(&font->miss_count, 1);

    if (font->free_list != -1) {
        idx = font->free_list;
        font->free_list = font->cache_nodes[idx].lru_next;
    } else {
        idx = font->lru_tail;
        if (idx == -1) { TTF_MUTEX_UNLOCK(font->lock); return NULL; }
        cache_unlink_lru(font, idx);
        hash_remove(font, font->cache_nodes[idx].codepoint);
        if (font->cache_nodes[idx].bmp.pixels) {
            TTF_FREE(font->cache_nodes[idx].bmp.pixels);
            font->cache_nodes[idx].bmp.pixels = NULL;
        }
    }
    
    TTF_CacheNode* node = &font->cache_nodes[idx];
    node->next = -1; 
    node->codepoint = codepoint;
    
    int32_t advanceWidth, leftSideBearing;
    stbtt_GetCodepointHMetrics(&font->info, codepoint, &advanceWidth, &leftSideBearing);
    node->advanceWidth = (int32_t)(advanceWidth * font->scale);
    
    int32_t x0, y0, x1, y1;
    float subpix_shift = 0.0f;
    stbtt_GetCodepointBitmapBoxSubpixel(&font->info, codepoint, font->scale * font->oversampling, 
                                        font->scale * font->oversampling, subpix_shift, subpix_shift, &x0, &y0, &x1, &y1);
    node->off_x = x0; node->off_y = y0;
    
    int32_t w = x1 - x0; int32_t h = y1 - y0;
    node->bmp.width = w; node->bmp.height = h;
    
    if (w > 0 && h > 0) {
        size_t mem_size = (size_t)w * (size_t)h;
        if (mem_size / (size_t)w != (size_t)h) goto fail;
        
        node->bmp.pixels = (unsigned char*)TTF_MALLOC(mem_size);
        if (!node->bmp.pixels) goto fail;
        
        TTF_MEMSET(node->bmp.pixels, 0, mem_size);
        
        stbtt_MakeCodepointBitmapSubpixel(&font->info, node->bmp.pixels, w, h, w, 
                                          font->scale * font->oversampling, font->scale * font->oversampling, 
                                          subpix_shift, subpix_shift, codepoint);

        if (font->bold_strength > 0) {
            for (int32_t b = 1; b <= font->bold_strength; b++) {
                for (int32_t y = 0; y < h; y++) {
                    for (int32_t x = b; x < w; x++) {
                        unsigned char val = node->bmp.pixels[y * w + x - b];
                        if (val > node->bmp.pixels[y * w + x]) node->bmp.pixels[y * w + x] = val;
                    }
                }
            }
            for (int32_t b = 1; b <= font->bold_strength; b++) {
                for (int32_t y = b; y < h; y++) {
                    for (int32_t x = 0; x < w; x++) {
                        unsigned char val = node->bmp.pixels[(y - b) * w + x];
                        if (val > node->bmp.pixels[y * w + x]) node->bmp.pixels[y * w + x] = val;
                    }
                }
            }
        }

        if (font->italic_skew > 0.0f) {
            apply_italic_skew(&node->bmp, font->italic_skew);
            node->off_x -= (int32_t)(node->bmp.height * font->italic_skew);
        }

        if (font->oversampling > 1) {
            int32_t target_w = node->bmp.width / font->oversampling;
            int32_t target_h = node->bmp.height / font->oversampling;
            downsample_bilinear(&node->bmp, target_w, target_h);
            node->off_x /= font->oversampling;
            node->off_y /= font->oversampling;
        }
    } else {
        node->bmp.pixels = NULL;
    }
    
    hash_insert(font, codepoint, idx);
    cache_push_front_lru(font, idx);
    TTF_MUTEX_UNLOCK(font->lock);
    return node;

fail:
    node->bmp.pixels = NULL; node->bmp.width = 0; node->bmp.height = 0;
    node->next = -1; node->lru_prev = -1;
    node->lru_next = font->free_list;
    font->free_list = idx;
    TTF_MUTEX_UNLOCK(font->lock);
    return NULL;
}

// ==================== 外部 API 实现 ====================

TTF_Font* TTF_CreateFont(int32_t cache_capacity) {
    if (cache_capacity <= 0) cache_capacity = 256;
    TTF_Font* font = (TTF_Font*)TTF_MALLOC(sizeof(TTF_Font));
    if (!font) return NULL;
    TTF_MEMSET(font, 0, sizeof(TTF_Font));
    
    font->cache_capacity = cache_capacity;
    font->hash_size = next_power_of_two(cache_capacity * 2);
    font->cache_nodes = (TTF_CacheNode*)TTF_MALLOC(sizeof(TTF_CacheNode) * font->cache_capacity);
    font->hash_table = (int32_t*)TTF_MALLOC(sizeof(int32_t) * font->hash_size);
    
    if (!font->cache_nodes || !font->hash_table) {
        if (font->cache_nodes) TTF_FREE(font->cache_nodes);
        if (font->hash_table) TTF_FREE(font->hash_table);
        TTF_FREE(font);
        return NULL;
    }
    
    font->is_initialized = false;
    font->oversampling = 1;
    font->bold_strength = 0;
    font->italic_skew = 0.0f;
    cache_init(font);
    TTF_MUTEX_INIT(font->lock);
    return font;
}

void TTF_ClearGlyphCache(TTF_Font *font) {
    if (!font) return;
    TTF_MUTEX_LOCK(font->lock);
    for (int32_t i = 0; i < font->cache_capacity; i++) {
        if (font->cache_nodes[i].bmp.pixels) {
            TTF_FREE(font->cache_nodes[i].bmp.pixels);
            font->cache_nodes[i].bmp.pixels = NULL;
        }
    }
    cache_init(font);
    font->hit_count = 0;
    font->miss_count = 0;
    TTF_MUTEX_UNLOCK(font->lock);
}

void TTF_DestroyFont(TTF_Font* font) {
    if (!font) return;
    TTF_ClearGlyphCache(font);
    TTF_MUTEX_DESTROY(font->lock);
    if (font->data) TTF_FREE(font->data);
    if (font->cache_nodes) TTF_FREE(font->cache_nodes);
    if (font->hash_table) TTF_FREE(font->hash_table);
    TTF_FREE(font);
}

void TTF_GetCacheStats(TTF_Font *font, TTF_CacheStats *out_stats) {
    if (!font || !out_stats) return;
    TTF_MUTEX_LOCK(font->lock);
    // 修复 1: 使用统一原子宏安全读取
    out_stats->hit_count = TTF_ATOMIC_LOAD(&font->hit_count);
    out_stats->miss_count = TTF_ATOMIC_LOAD(&font->miss_count);
    TTF_MUTEX_UNLOCK(font->lock);
}

bool TTF_LoadFontFromMemory(TTF_Font *font, const unsigned char *data, size_t data_size, int32_t pixel_height) {
    if (!font || !data || data_size == 0) return false;
    if (font->is_initialized) TTF_ClearGlyphCache(font);
    if (font->data) { TTF_FREE(font->data); font->data = NULL; }

    font->data = (unsigned char*)TTF_MALLOC(data_size);
    if (!font->data) return false;
    TTF_MEMCPY(font->data, data, data_size);

    int32_t offset = stbtt_GetFontOffsetForIndex(font->data, 0);
    if (!stbtt_InitFont(&font->info, font->data, offset)) {
        TTF_FREE(font->data); font->data = NULL; font->is_initialized = false; return false;
    }
    font->is_initialized = true;
    TTF_SetPixelHeight(font, pixel_height);
    return true;
}

void TTF_SetPixelHeight(TTF_Font *font, int32_t pixel_height) {
    if (!font || !font->is_initialized || pixel_height <= 0) return;
    if (font->pixel_height != pixel_height) TTF_ClearGlyphCache(font);
    font->pixel_height = pixel_height;
    font->scale = stbtt_ScaleForPixelHeight(&font->info, (float)pixel_height);
    int32_t ascent, descent, lineGap;
    stbtt_GetFontVMetrics(&font->info, &ascent, &descent, &lineGap);
    font->ascent = (int32_t)(ascent * font->scale);
}

void TTF_SetOversampling(TTF_Font *font, int32_t oversampling) {
    if (!font || oversampling < 1 || oversampling > 4) return;
    if (font->oversampling != oversampling) {
        TTF_ClearGlyphCache(font);
        font->oversampling = oversampling;
    }
}

void TTF_SetFontStyle(TTF_Font *font, int32_t bold_strength, float italic_skew) {
    if (!font) return;
    bold_strength = bold_strength > 3 ? 3 : (bold_strength < 0 ? 0 : bold_strength);
    italic_skew = italic_skew > 0.5f ? 0.5f : (italic_skew < 0.0f ? 0.0f : italic_skew);
    
    if (font->bold_strength != bold_strength || font->italic_skew != italic_skew) {
        TTF_ClearGlyphCache(font);
        font->bold_strength = bold_strength;
        font->italic_skew = italic_skew;
    }
}

void TTF_GetTextSize(TTF_Font *font, const char *text, int32_t *out_width, int32_t *out_height) {
    if (!font || !text || !font->is_initialized) return;
    int32_t len = (int32_t)TTF_STRLEN(text);
    int32_t x_pos = 0, max_x = 0, min_y = 0, max_y = 0, i = 0;

    while (i < len) {
        int32_t advance = 0;
        int32_t codepoint = utf8_to_codepoint(&text[i], len - i, &advance);
        TTF_CacheNode* node = get_glyph(font, codepoint);
        if (node) {
            int32_t top = font->ascent + node->off_y;
            int32_t bot = top + node->bmp.height;
            if (top < min_y) min_y = top;
            if (bot > max_y) max_y = bot;
            int32_t right = x_pos + node->off_x + node->bmp.width;
            if (right > max_x) max_x = right;
            x_pos += node->advanceWidth;
        }
        if (i + advance < len) {
            int32_t next_advance = 0;
            int32_t next_cp = utf8_to_codepoint(&text[i + advance], len - (i + advance), &next_advance);
            int32_t kern = stbtt_GetCodepointKernAdvance(&font->info, codepoint, next_cp);
            x_pos += (int32_t)(kern * font->scale);
        }
        i += advance;
    }
    if (out_width) *out_width = max_x;
    if (out_height) *out_height = max_y - min_y;
}

const TTF_Bitmap* TTF_GetGlyphBitmap(TTF_Font *font, int32_t codepoint, int32_t *out_advance, int32_t *out_off_x, int32_t *out_off_y) {
    TTF_CacheNode* node = get_glyph(font, codepoint);
    if (!node) return NULL;
    if (out_advance) *out_advance = node->advanceWidth;
    if (out_off_x) *out_off_x = node->off_x;
    if (out_off_y) *out_off_y = node->off_y;
    return &node->bmp;
}

TTF_Bitmap TTF_RenderChar(TTF_Font *font, int32_t codepoint, int32_t *out_x_offset, int32_t *out_y_offset) {
    TTF_Bitmap bmp = {0};
    int32_t off_x, off_y, advance;
    const TTF_Bitmap* ref = TTF_GetGlyphBitmap(font, codepoint, &advance, &off_x, &off_y);
    if (!ref) return bmp;
    if (out_x_offset) *out_x_offset = off_x;
    if (out_y_offset) *out_y_offset = off_y;

    bmp.width = ref->width; bmp.height = ref->height;
    if (bmp.width > 0 && bmp.height > 0 && ref->pixels) {
        size_t mem_size = (size_t)bmp.width * (size_t)bmp.height;
        bmp.pixels = (unsigned char*)TTF_MALLOC(mem_size);
        if (bmp.pixels) TTF_MEMCPY(bmp.pixels, ref->pixels, mem_size);
        else { bmp.width = 0; bmp.height = 0; }
    }
    return bmp;
}

static void draw_line_to_buffer(TTF_Font *font, const char *text, int32_t len, 
                                unsigned char *dst_pixels, int32_t dst_w, int32_t dst_h, 
                                int32_t start_x, int32_t start_y, int32_t line_min_y) {
    int32_t x_pos = start_x;
    int32_t i = 0;
    while (i < len) {
        int32_t advance = 0;
        int32_t codepoint = utf8_to_codepoint(&text[i], len - i, &advance);
        TTF_CacheNode* node = get_glyph(font, codepoint);
        if (node && node->bmp.pixels) {
            int32_t draw_x = x_pos + node->off_x;
            int32_t draw_y = start_y + font->ascent + node->off_y - line_min_y;
            for (int32_t y = 0; y < node->bmp.height; y++) {
                for (int32_t x = 0; x < node->bmp.width; x++) {
                    int32_t px = draw_x + x; int32_t py = draw_y + y;
                    if (px >= 0 && px < dst_w && py >= 0 && py < dst_h) {
                        int32_t idx = py * dst_w + px;
                        int32_t c_idx = y * node->bmp.width + x;
                        if (node->bmp.pixels[c_idx] > dst_pixels[idx]) {
                            dst_pixels[idx] = node->bmp.pixels[c_idx];
                        }
                    }
                }
            }
            x_pos += node->advanceWidth;
        }
        if (i + advance < len) {
            int32_t next_advance = 0;
            int32_t next_cp = utf8_to_codepoint(&text[i + advance], len - (i + advance), &next_advance);
            int32_t kern = stbtt_GetCodepointKernAdvance(&font->info, codepoint, next_cp);
            x_pos += (int32_t)(kern * font->scale);
        }
        i += advance;
    }
}

bool TTF_RenderTextToBuffer(TTF_Font *font, const char *text, TTF_Bitmap *out_bmp) {
    if (!font || !text || !out_bmp || !font->is_initialized) return false;
    int32_t len = (int32_t)TTF_STRLEN(text);
    if (len == 0) return false;

    int32_t cmd_capacity = 256;
    TTF_RenderCmd* cmds = (TTF_RenderCmd*)TTF_MALLOC(sizeof(TTF_RenderCmd) * cmd_capacity);
    if (!cmds) return false;

    int32_t cmd_count = 0;
    int32_t x_pos = 0, max_x = 0, min_y = 0, max_y = 0;
    int32_t i = 0;

    while (i < len) {
        if (cmd_count >= cmd_capacity) {
            cmd_capacity *= 2;
            TTF_RenderCmd* new_cmds = (TTF_RenderCmd*)TTF_REALLOC(cmds, sizeof(TTF_RenderCmd) * cmd_capacity);
            if (!new_cmds) { TTF_FREE(cmds); return false; }
            cmds = new_cmds;
        }

        int32_t advance = 0;
        int32_t codepoint = utf8_to_codepoint(&text[i], len - i, &advance);
        TTF_RenderCmd* cmd = &cmds[cmd_count++];
        cmd->codepoint = codepoint;
        cmd->x_advance = 0;

        TTF_CacheNode* node = get_glyph(font, codepoint);
        if (node) {
            int32_t top = font->ascent + node->off_y;
            int32_t bot = top + node->bmp.height;
            if (top < min_y) min_y = top;
            if (bot > max_y) max_y = bot;
            int32_t right = x_pos + node->off_x + node->bmp.width;
            if (right > max_x) max_x = right;
            cmd->x_advance = node->advanceWidth;
        }

        if (i + advance < len) {
            int32_t next_advance = 0;
            int32_t next_cp = utf8_to_codepoint(&text[i + advance], len - (i + advance), &next_advance);
            int32_t kern = stbtt_GetCodepointKernAdvance(&font->info, codepoint, next_cp);
            cmd->x_advance += (int32_t)(kern * font->scale);
        }
        x_pos += cmd->x_advance;
        i += advance;
    }

    int32_t text_width = max_x;
    int32_t text_height = max_y - min_y;
    if (text_width <= 0 || text_height <= 0) { TTF_FREE(cmds); return false; }

    if (!out_bmp->pixels || out_bmp->width < text_width || out_bmp->height < text_height) {
        if (out_bmp->pixels) TTF_FREE(out_bmp->pixels);
        size_t mem_size = (size_t)text_width * (size_t)text_height;
        if (mem_size / (size_t)text_width != (size_t)text_height) { TTF_FREE(cmds); return false; }
        out_bmp->pixels = (unsigned char*)TTF_MALLOC(mem_size);
        if (!out_bmp->pixels) { TTF_FREE(cmds); return false; }
        out_bmp->width = text_width; out_bmp->height = text_height;
    }
    TTF_MEMSET(out_bmp->pixels, 0, (size_t)text_width * text_height);

    int32_t x_pos2 = 0;
    for (int32_t k = 0; k < cmd_count; k++) {
        TTF_CacheNode* node = get_glyph(font, cmds[k].codepoint);
        if (node && node->bmp.pixels) {
            int32_t draw_x = x_pos2 + node->off_x;
            int32_t draw_y = font->ascent + node->off_y - min_y;
            for (int32_t y = 0; y < node->bmp.height; y++) {
                for (int32_t x = 0; x < node->bmp.width; x++) {
                    int32_t px = draw_x + x; int32_t py = draw_y + y;
                    if (px >= 0 && px < text_width && py >= 0 && py < text_height) {
                        int32_t idx = py * text_width + px;
                        int32_t c_idx = y * node->bmp.width + x;
                        if (node->bmp.pixels[c_idx] > out_bmp->pixels[idx]) {
                            out_bmp->pixels[idx] = node->bmp.pixels[c_idx];
                        }
                    }
                }
            }
        }
        x_pos2 += cmds[k].x_advance;
    }

    TTF_FREE(cmds);
    return true;
}

TTF_Bitmap TTF_RenderText(TTF_Font *font, const char *text) {
    TTF_Bitmap bmp = {0};
    TTF_RenderTextToBuffer(font, text, &bmp);
    return bmp;
}

void TTF_FreeBitmap(TTF_Bitmap *bitmap) {
    if (bitmap && bitmap->pixels) {
        TTF_FREE(bitmap->pixels);
        bitmap->pixels = NULL; bitmap->width = 0; bitmap->height = 0;
    }
}

TTF_Bitmap TTF_RenderTextMultiline(TTF_Font *font, const char *text, int32_t max_width, TTF_Align align, int32_t line_gap) {
    TTF_Bitmap final_bmp = {0};
    if (!font || !text || !font->is_initialized) return final_bmp;

    int32_t len = (int32_t)TTF_STRLEN(text);
    if (len == 0) return final_bmp;

    int32_t lines_capacity = 16;
    int32_t line_count = 0;
    typedef struct { int32_t start; int32_t end; int32_t width; int32_t height; int32_t min_y; } LineInfo;
    LineInfo *lines = (LineInfo*)TTF_MALLOC(sizeof(LineInfo) * lines_capacity);
    if (!lines) return final_bmp;

    int32_t i = 0;
    int32_t total_height = 0;
    int32_t max_line_width = 0;

    while (i < len) {
        int32_t line_start = i;
        int32_t line_end = i;
        int32_t last_space = -1;
        int32_t x_pos = 0;
        int32_t min_y = 0, max_y = 0;

        while (i < len) {
            int32_t advance = 0;
            int32_t codepoint = utf8_to_codepoint(&text[i], len - i, &advance);
            
            if (codepoint == '\n') {
                line_end = i;
                i += advance;
                break;
            }

            TTF_CacheNode* node = get_glyph(font, codepoint);
            int32_t char_w = node ? node->advanceWidth : 0;

            if (max_width > 0 && x_pos + char_w > max_width) {
                if (is_punct_no_start(codepoint) && i > line_start) {
                    // 强制将标点留在本行
                } else {
                    if (last_space != -1) {
                        line_end = last_space;
                        i = last_space + 1;
                        break;
                    } else if (is_cjk_char(codepoint) && i > line_start) {
                        line_end = i;
                        break;
                    }
                    line_end = i;
                    break;
                }
            }

            if (node) {
                int32_t top = font->ascent + node->off_y;
                int32_t bot = top + node->bmp.height;
                if (top < min_y) min_y = top;
                if (bot > max_y) max_y = bot;
            }
            x_pos += char_w;
            if (codepoint == ' ') last_space = i;
            i += advance;
            line_end = i;
        }

        if (line_end > line_start) {
            if (line_count >= lines_capacity) {
                lines_capacity *= 2;
                LineInfo *new_lines = (LineInfo*)TTF_REALLOC(lines, sizeof(LineInfo) * lines_capacity);
                if (!new_lines) { TTF_FREE(lines); return final_bmp; }
                lines = new_lines;
            }
            lines[line_count].start = line_start;
            lines[line_count].end = line_end;
            lines[line_count].width = x_pos;
            lines[line_count].height = max_y - min_y;
            lines[line_count].min_y = min_y;
            if (x_pos > max_line_width) max_line_width = x_pos;
            total_height += (max_y - min_y) + line_gap;
            line_count++;
        }
    }

    if (line_count == 0) { TTF_FREE(lines); return final_bmp; }
    total_height -= line_gap;

    final_bmp.width = max_width > 0 ? max_width : max_line_width;
    final_bmp.height = total_height;
    size_t mem_size = (size_t)final_bmp.width * final_bmp.height;
    if (mem_size / (size_t)final_bmp.width != (size_t)final_bmp.height) { TTF_FREE(lines); final_bmp.width=0; return final_bmp; }
    
    final_bmp.pixels = (unsigned char*)TTF_MALLOC(mem_size);
    if (!final_bmp.pixels) { TTF_FREE(lines); final_bmp.width = 0; final_bmp.height = 0; return final_bmp; }
    TTF_MEMSET(final_bmp.pixels, 0, mem_size);

    int32_t y_cursor = 0;
    for (int32_t l = 0; l < line_count; l++) {
        int32_t line_len = lines[l].end - lines[l].start;
        int32_t x_offset = 0;
        if (align == TTF_ALIGN_CENTER) x_offset = (final_bmp.width - lines[l].width) / 2;
        else if (align == TTF_ALIGN_RIGHT) x_offset = final_bmp.width - lines[l].width;

        draw_line_to_buffer(font, text + lines[l].start, line_len, 
                            final_bmp.pixels, final_bmp.width, final_bmp.height, 
                            x_offset, y_cursor, lines[l].min_y);
        
        y_cursor += lines[l].height + line_gap;
    }

    TTF_FREE(lines);
    return final_bmp;
}
#include <stdio.h>
#include <base/arch/x86_64/syscall.h>

// 初始化并加载系统字体
uint8_t TTF_ReadFont(TTF_Font *TTFFont,const char* path, int32_t pixel_height,int32_t CacheCap) {
    TTFFont = TTF_CreateFont(CacheCap);
    if (!TTFFont) return 1;

    FILE* fd = fopen(path, "r");
    if (fd == NULL) {
        TTF_DestroyFont(TTFFont);
        syscall(24, (long)"FAULT!2", 7, 0, 0, 0, 0);
        return 2;
    }

    uint64_t file_size = fsize(fd);
    if (file_size == 0) {
        fclose(fd);
        TTF_DestroyFont(TTFFont);
        syscall(24, (long)"FAULT3", 7, 0, 0, 0, 0);
        return 3;
    }

    unsigned char* font_data = (unsigned char*)malloc(file_size);
    if (!font_data) {
        fclose(fd);
        TTF_DestroyFont(TTFFont);
        syscall(24, (long)"FAULT!4", 7, 0, 0, 0, 0);
        return 4;
    }

    fread(font_data, file_size,1,fd);
    fclose(fd);

    bool success = TTF_LoadFontFromMemory(TTFFont, font_data, file_size, pixel_height);
    
    free(font_data);

    if (!success) {
        TTF_DestroyFont(TTFFont);
        TTFFont = NULL;
            
        return 5;
    }

    TTF_SetOversampling(TTFFont, 2); // 2x2 超采样
    // TTF_SetFontStyle(g_system_font, 1, 0.2f); // 轻微加粗和斜体

    return 0;
}

void TTF_DrawText(
FrameBuffer *FB, TTF_Font *TTFFont,
int32_t x, int32_t y, const char* text, uint32_t color
) {
    if (!TTFFont || !FB || !FB->BaseAddress) return;

    // 渲染文本到位图
    TTF_Bitmap bmp = TTF_RenderText(TTFFont, text);
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
        for (int32_t y2 = 0; y2 < bmp.height; y2++) {
            for (int32_t x2 = 0; x2 < bmp.width; x2++) {
                
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