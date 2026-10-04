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
    #define TTF_ATOMIC_FETCH_ADD(ptr, val) (*(ptr) += (val))
    #define TTF_ATOMIC_LOAD(ptr) (*(ptr))
#endif

// ==================== 字形图集几何 ====================
// 页大小 256x256 (64KB, 8bpp alpha)。细粒度页保证 LRU 重建后留有
// >=20% 预算余量, 不会出现 "重建后仍放不下 -> 再重建" 的抖动。
#define TTF_ATLAS_PAGE_W 256
#define TTF_ATLAS_PAGE_H 256

/* Glyph geometry, computed WITHOUT rasterizing. render_glyph() produces
   bitmaps whose width/height/off_x/off_y match these values exactly, so
   every measure pass is allocation-free, raster-free AND atlas-free
   (measuring never evicts hot glyphs). */
typedef struct {
    int32_t advance;       /* horizontal advance, px */
    int32_t off_x, off_y;  /* final placement offsets (post italic / downsample) */
    int32_t width, height; /* final bitmap dims (== render_glyph result) */
    int32_t ras_w, ras_h;  /* raster dims (oversampled, pre-italic) */
    bool    has_ink;
} TTF_GlyphGeo;

/* One cached glyph. page semantics:
     page >= 0 : packed in pages[page] at (x, y), row stride = page width
     page == -1 : no ink (space etc.), metrics only
     page == -2 : oversized transient (bigger than the whole byte budget),
                  pixels live in font->transient, NOT indexed in the hash */
typedef struct {
    int32_t codepoint;
    int32_t advance;
    int32_t off_x, off_y;
    int32_t page;
    int32_t x, y;
    int32_t w, h;
    uint64_t lru;          /* monotonic use stamp */
} TTF_AtlasGlyph;

/* Shelf packer page: glyphs are placed left-to-right; when a row is full
   the cursor wraps to a new row of the tallest glyph height seen. */
typedef struct {
    unsigned char* pixels;
    int32_t w, h;
    int32_t cursor_x, cursor_y;
    int32_t row_h;
} TTF_AtlasPage;

struct TTF_Font_Internal {
    stbtt_fontinfo info;
    unsigned char *data;
    float scale;
    int32_t pixel_height;
    int32_t ascent;
    int32_t descent;     /* stbtt convention: negative below baseline */
    int32_t line_gap;    /* typographic line gap, in pixels */
    int32_t line_height; /* ascent - descent + line_gap (+1 px guard) */
    bool is_initialized;

    /* ---- glyph atlas ---- */
    TTF_AtlasPage*  pages;                        int32_t page_count, page_cap;
    uint64_t        atlas_bytes, atlas_max_bytes;
    TTF_AtlasGlyph* glyphs;                       int32_t glyph_count, glyph_cap, glyph_max;
    int32_t*        hash;                         int32_t hash_size, hash_used;
    uint64_t        lru_stamp;
    TTF_Bitmap      transient;        /* owned pixels of the oversized glyph */
    TTF_AtlasGlyph  transient_glyph;
    unsigned char*  scratch;          /* TTF_GetGlyphBitmap copy-out buffer */
    size_t          scratch_cap;
    TTF_Bitmap      scratch_view;

    TTF_MUTEX_TYPE lock;

    int32_t oversampling;
    int32_t bold_strength;
    float italic_skew;

    unsigned long hit_count;
    unsigned long miss_count;
    unsigned long evict_count;
};

typedef struct TTF_Font_Internal TTF_Font;

typedef struct {
    int32_t codepoint;
    int32_t x_advance;
} TTF_RenderCmd;

static bool is_cjk_char(int32_t codepoint) {
    return (codepoint >= 0x4E00 && codepoint <= 0x9FFF) ||
           (codepoint >= 0x3400 && codepoint <= 0x4DBF) ||
           (codepoint >= 0x3040 && codepoint <= 0x30FF) ||
           (codepoint >= 0xAC00 && codepoint <= 0xD7AF) ||
           (codepoint >= 0x20000 && codepoint <= 0x2A6DF);
}

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

/* Skew failure is now FATAL for the render: the atlas trusts glyph_geo dims
   for placement/copy, so a silently unskewed bitmap with skewed metrics
   (legacy behavior) would smear neighboring rows. */
static bool apply_italic_skew(TTF_Bitmap *bmp, float skew) {
    if (skew <= 0.0f || !bmp->pixels) return true;
    int32_t new_w = bmp->width + (int32_t)(bmp->height * skew) + 1;
    unsigned char *new_pixels = (unsigned char*)TTF_MALLOC((size_t)new_w * bmp->height);
    if (!new_pixels) return false;
    TTF_MEMSET(new_pixels, 0, (size_t)new_w * bmp->height);

    for (int32_t y = 0; y < bmp->height; y++) {
        int32_t offset = (int32_t)((bmp->height - y) * skew);
        for (int32_t x = 0; x < bmp->width; x++) {
            new_pixels[y * new_w + x + offset] = bmp->pixels[y * bmp->width + x];
        }
    }
    TTF_FREE(bmp->pixels);
    bmp->pixels = new_pixels;
    bmp->width = new_w;
    return true;
}

static void downsample_bilinear(TTF_Bitmap *bmp, int32_t target_w, int32_t target_h) {
    if (bmp->width == target_w && bmp->height == target_h) return;
    if (target_w <= 0 || target_h <= 0) return;

    unsigned char *dst = (unsigned char*)TTF_MALLOC((size_t)target_w * target_h);
    if (!dst) return;   /* caller detects the dimension mismatch and bails */
    TTF_MEMSET(dst, 0, (size_t)target_w * target_h);

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

/* Caller holds font->lock. Pure metrics: no rasterization, no allocation,
   no atlas interaction. Mirrors the transform chain of render_glyph exactly
   (bold smears inside the existing box; italic widens and shifts;
   oversampling divides dims and offsets). */
static void glyph_geo(TTF_Font* font, int32_t codepoint, TTF_GlyphGeo* g) {
    int32_t advanceWidth, leftSideBearing;
    stbtt_GetCodepointHMetrics(&font->info, codepoint, &advanceWidth, &leftSideBearing);
    g->advance = (int32_t)(advanceWidth * font->scale);

    int32_t x0, y0, x1, y1;
    stbtt_GetCodepointBitmapBoxSubpixel(&font->info, codepoint,
                                        font->scale * font->oversampling,
                                        font->scale * font->oversampling,
                                        0.0f, 0.0f, &x0, &y0, &x1, &y1);

    int32_t w = x1 - x0, h = y1 - y0;
    g->ras_w = w; g->ras_h = h;
    g->has_ink = (w > 0 && h > 0);

    int32_t ox = x0, oy = y0;
    if (g->has_ink) {
        if (font->italic_skew > 0.0f) {
            int32_t shift = (int32_t)(h * font->italic_skew);
            w += shift + 1;
            ox = x0 - shift;
        }
        if (font->oversampling > 1) {
            w /= font->oversampling; h /= font->oversampling;
            ox /= font->oversampling; oy /= font->oversampling;
        }
    }
    g->width = w; g->height = h;
    g->off_x = ox; g->off_y = oy;
}

/* Caller holds font->lock. Rasterizes on demand. out->pixels is freshly
   allocated and OWNED BY THE CALLER. The returned bitmap's dims ALWAYS match
   g->width/g->height on success (post-processing failures bail out cleanly
   instead of storing a metrics/bitmap mismatch). */
static bool render_glyph(TTF_Font* font, int32_t codepoint, TTF_GlyphGeo* g, TTF_Bitmap* out) {
    glyph_geo(font, codepoint, g);

    out->pixels = NULL;
    out->width  = g->width;
    out->height = g->height;
    if (!g->has_ink) return true;          /* e.g. space: metrics only */

    int32_t w = g->ras_w, h = g->ras_h;
    size_t mem_size = (size_t)w * (size_t)h;
    if (mem_size / (size_t)w != (size_t)h) return false;

    unsigned char* px = (unsigned char*)TTF_MALLOC(mem_size);
    if (!px) return false;
    TTF_MEMSET(px, 0, mem_size);

    stbtt_MakeCodepointBitmapSubpixel(&font->info, px, w, h, w,
                                      font->scale * font->oversampling,
                                      font->scale * font->oversampling,
                                      0.0f, 0.0f, codepoint);

    TTF_Bitmap tmp;
    tmp.pixels = px; tmp.width = w; tmp.height = h;

    if (font->bold_strength > 0) {
        for (int32_t b = 1; b <= font->bold_strength; b++) {
            for (int32_t y = 0; y < h; y++) {
                for (int32_t x = b; x < w; x++) {
                    unsigned char val = tmp.pixels[y * w + x - b];
                    if (val > tmp.pixels[y * w + x]) tmp.pixels[y * w + x] = val;
                }
            }
        }
        for (int32_t b = 1; b <= font->bold_strength; b++) {
            for (int32_t y = b; y < h; y++) {
                for (int32_t x = 0; x < w; x++) {
                    unsigned char val = tmp.pixels[(y - b) * w + x];
                    if (val > tmp.pixels[y * w + x]) tmp.pixels[y * w + x] = val;
                }
            }
        }
    }

    if (font->italic_skew > 0.0f) {
        if (!apply_italic_skew(&tmp, font->italic_skew)) { TTF_FREE(tmp.pixels); return false; }
    }

    if (font->oversampling > 1) {
        int32_t tw = tmp.width / font->oversampling;
        int32_t th = tmp.height / font->oversampling;
        if (tw > 0 && th > 0) {
            downsample_bilinear(&tmp, tw, th);
            if (tmp.width != tw || tmp.height != th) {   /* downsample OOM */
                TTF_FREE(tmp.pixels);
                return false;
            }
        }
        /* tw/th == 0: glyph smaller than the oversampling factor. The bitmap
           stays full-resolution, but geo divides to 0 so the caller discards
           it (metrics-only entry) - legacy-compatible measurement. */
    }

    out->pixels = tmp.pixels;
    out->width  = tmp.width;
    out->height = tmp.height;
    return true;
}

// ---------------------------------------------------------------------------
// Atlas: open-addressing hash over glyph indices
// ---------------------------------------------------------------------------
static uint32_t atlas_hash_mix(int32_t cp, int32_t mask) {
    uint32_t h = (uint32_t)cp;
    h ^= h >> 16;
    h *= 2654435761u;
    h ^= h >> 16;
    return h & (uint32_t)mask;
}

static int32_t atlas_hash_find(const TTF_Font* f, int32_t cp) {
    if (!f->hash) return -1;
    int32_t mask = f->hash_size - 1;
    uint32_t h = atlas_hash_mix(cp, mask);
    while (f->hash[h] != -1) {
        if (f->glyphs[f->hash[h]].codepoint == cp) return f->hash[h];
        h = (h + 1) & (uint32_t)mask;
    }
    return -1;
}

/* cp must be handled by the caller (absent or overwrite-able). Returns false
   only on OOM, in which case the entry stays unindexed until the next LRU
   rebuild re-inserts every array entry (self-healing). */
static bool atlas_hash_insert(TTF_Font* f, int32_t cp, int32_t gi) {
    if ((uint64_t)(f->hash_used + 1) * 10 >= (uint64_t)f->hash_size * 7) {
        int32_t ns = f->hash_size * 2;
        int32_t* nt = (int32_t*)TTF_MALLOC(sizeof(int32_t) * ns);
        if (!nt) return false;
        for (int32_t i = 0; i < ns; i++) nt[i] = -1;
        int32_t* old = f->hash;
        int32_t old_size = f->hash_size;
        f->hash = nt; f->hash_size = ns; f->hash_used = 0;
        for (int32_t i = 0; i < old_size; i++) {
            int32_t e = old[i];
            if (e == -1) continue;
            uint32_t h = atlas_hash_mix(f->glyphs[e].codepoint, ns - 1);
            while (nt[h] != -1) h = (h + 1) & (uint32_t)(ns - 1);
            nt[h] = e;
            f->hash_used++;
        }
        TTF_FREE(old);
    }
    int32_t mask = f->hash_size - 1;
    uint32_t h = atlas_hash_mix(cp, mask);
    while (f->hash[h] != -1) {
        if (f->glyphs[f->hash[h]].codepoint == cp) { f->hash[h] = gi; return true; }
        h = (h + 1) & (uint32_t)mask;
    }
    f->hash[h] = gi;
    f->hash_used++;
    return true;
}

// ---------------------------------------------------------------------------
// Atlas: shelf-packed pages
// ---------------------------------------------------------------------------
static int32_t atlas_page_new(TTF_Font* f, int32_t w, int32_t h) {
    if (f->page_count == f->page_cap) {
        int32_t nc = f->page_cap ? f->page_cap * 2 : 4;
        TTF_AtlasPage* np = (TTF_AtlasPage*)TTF_REALLOC(f->pages, sizeof(TTF_AtlasPage) * nc);
        if (!np) return -1;
        f->pages = np;
        f->page_cap = nc;
    }
    unsigned char* px = (unsigned char*)TTF_MALLOC((size_t)w * (size_t)h);
    if (!px) return -1;
    TTF_MEMSET(px, 0, (size_t)w * (size_t)h);
    TTF_AtlasPage* p = &f->pages[f->page_count];
    p->pixels = px; p->w = w; p->h = h;
    p->cursor_x = 0; p->cursor_y = 0; p->row_h = 0;
    f->atlas_bytes += (uint64_t)w * (uint64_t)h;
    return f->page_count++;
}

/* Shelf placement with 1px gutters (gutter pixels stay 0: no bleed if the
   page is ever uploaded as a GPU texture). Writes the output x and y only
   on success. */
static bool atlas_page_place(TTF_AtlasPage* p, int32_t w, int32_t h, int32_t* ox, int32_t* oy) {
    if (w > p->w || h > p->h) return false;
    if (p->cursor_x + w > p->w) {              /* wrap to a new row */
        p->cursor_y += p->row_h + 1;
        p->cursor_x = 0;
        p->row_h = 0;
    }
    if (p->cursor_y + h > p->h) return false;
    *ox = p->cursor_x;
    *oy = p->cursor_y;
    p->cursor_x = *ox + w + 1;
    if (h > p->row_h) p->row_h = h;
    return true;
}

/* Try existing pages (newest first), then one new page within `budget`.
   Returns the page index (and writes g->x/g->y), or -1. */
static int32_t atlas_place_budget(TTF_Font* f, TTF_AtlasGlyph* g, uint64_t budget) {
    for (int32_t p = f->page_count - 1; p >= 0; p--) {
        if (atlas_page_place(&f->pages[p], g->w, g->h, &g->x, &g->y)) return p;
    }
    int32_t pw = TTF_ATLAS_PAGE_W, ph = TTF_ATLAS_PAGE_H;
    if (g->w > pw) pw = next_power_of_two(g->w);   /* oversized glyph: dedicated page */
    if (g->h > ph) ph = next_power_of_two(g->h);
    if (f->atlas_bytes + (uint64_t)pw * (uint64_t)ph > budget) return -1;
    int32_t np = atlas_page_new(f, pw, ph);
    if (np < 0) return -1;
    if (!atlas_page_place(&f->pages[np], g->w, g->h, &g->x, &g->y)) return -1;
    return np;
}

static void atlas_drop_all(TTF_Font* f) {
    for (int32_t i = 0; i < f->page_count; i++) TTF_FREE(f->pages[i].pixels);
    f->page_count = 0;
    f->atlas_bytes = 0;
    f->glyph_count = 0;
    if (f->hash) {
        for (int32_t i = 0; i < f->hash_size; i++) f->hash[i] = -1;
        f->hash_used = 0;
    }
}

/* --- tiny dependency-free heapsort (ascending) for LRU ordering --- */
static void sift_down_u64(uint64_t* a, int32_t root, int32_t n) {
    for (;;) {
        int32_t l = 2 * root + 1, r = l + 1, m = root;
        if (l < n && a[l] > a[m]) m = l;
        if (r < n && a[r] > a[m]) m = r;
        if (m == root) return;
        uint64_t t = a[root]; a[root] = a[m]; a[m] = t;
        root = m;
    }
}

/* LRU rebuild: keep the hottest entries (<= ~66% of the entry cap, page
   allocations capped at 80% of the byte budget so the triggering insert
   always finds room afterwards), re-pack them into fresh pages by direct
   pixel copy - NO re-rasterization - rebuild the hash, free the rest.
   Rare, bounded stall; keeps memory under atlas_max_bytes. */
static void atlas_evict_rebuild(TTF_Font* f) {
    TTF_ATOMIC_FETCH_ADD(&f->evict_count, 1);
    int32_t n = f->glyph_count;
    if (n <= 0) { atlas_drop_all(f); return; }

    uint64_t* keys = (uint64_t*)TTF_MALLOC((size_t)n * sizeof(uint64_t));
    if (!keys) { atlas_drop_all(f); return; }   /* OOM: drop everything */

    for (int32_t i = 0; i < n; i++)
        keys[i] = (f->glyphs[i].lru << 20) | (uint64_t)(uint32_t)i;  /* idx < 2^20 */

    for (int32_t i = n / 2 - 1; i >= 0; i--) sift_down_u64(keys, i, n);
    for (int32_t end = n - 1; end > 0; end--) {
        uint64_t t = keys[0]; keys[0] = keys[end]; keys[end] = t;
        sift_down_u64(keys, 0, end);
    }

    const int32_t  keep_entries = f->glyph_max - f->glyph_max / 3;              /* ~66% */
    const uint64_t page_budget  = f->atlas_max_bytes - f->atlas_max_bytes / 5;  /* 80% */

    TTF_AtlasPage*  old_pages = f->pages;    int32_t old_page_count = f->page_count, old_page_cap = f->page_cap;
    TTF_AtlasGlyph* old_glyphs = f->glyphs;  int32_t old_glyph_cap = f->glyph_cap;
    const uint64_t  old_bytes = f->atlas_bytes;

    f->pages = NULL; f->page_count = 0; f->page_cap = 0; f->atlas_bytes = 0;

    int32_t new_cap = old_glyph_cap > 64 ? old_glyph_cap : 64;
    TTF_AtlasGlyph* ng = (TTF_AtlasGlyph*)TTF_MALLOC((size_t)new_cap * sizeof(TTF_AtlasGlyph));
    if (!ng) {   /* OOM mid-rebuild: restore, then hard reset */
        f->pages = old_pages; f->page_count = old_page_count; f->page_cap = old_page_cap;
        f->glyphs = old_glyphs; f->glyph_cap = old_glyph_cap; f->atlas_bytes = old_bytes;
        TTF_FREE(keys);
        atlas_drop_all(f);
        return;
    }

    int32_t kept = 0;
    for (int32_t k = n - 1; k >= 0; k--) {     /* hottest first */
        if (kept >= keep_entries) break;
        int32_t idx = (int32_t)(keys[k] & 0xFFFFFu);
        TTF_AtlasGlyph g = old_glyphs[idx];
        if (g.w > 0 && g.h > 0 && g.page >= 0) {
            const TTF_AtlasPage* src_page = &old_pages[g.page];
            int32_t sx = g.x, sy = g.y;
            int32_t p = atlas_place_budget(f, &g, page_budget);
            if (p < 0) break;                  /* budget out: drop this + all colder */
            TTF_AtlasPage* dp = &f->pages[p];
            for (int32_t row = 0; row < g.h; row++)
                TTF_MEMCPY(dp->pixels + (size_t)(g.y + row) * (size_t)dp->w + (size_t)g.x,
                           src_page->pixels + (size_t)(sy + row) * (size_t)src_page->w + (size_t)sx,
                           (size_t)g.w);
            g.page = p;
        }
        ng[kept++] = g;
    }

    TTF_FREE(old_glyphs);
    f->glyphs = ng; f->glyph_cap = new_cap; f->glyph_count = kept;

    for (int32_t i = 0; i < f->hash_size; i++) f->hash[i] = -1;
    f->hash_used = 0;
    for (int32_t i = 0; i < kept; i++)
        atlas_hash_insert(f, ng[i].codepoint, i);

    for (int32_t i = 0; i < old_page_count; i++) TTF_FREE(old_pages[i].pixels);
    TTF_FREE(old_pages);
    TTF_FREE(keys);
}

/* Resolve a glyph's pixels. Returns NULL for ink-less glyphs. */
static const unsigned char* glyph_pixels(TTF_Font* f, const TTF_AtlasGlyph* g, int32_t* stride) {
    if (g->page >= 0) {
        const TTF_AtlasPage* p = &f->pages[g->page];
        *stride = p->w;
        return p->pixels + (size_t)g->y * (size_t)p->w + (size_t)g->x;
    }
    if (g->page == -2 && f->transient.pixels) {
        *stride = f->transient.width;
        return f->transient.pixels;
    }
    *stride = 0;
    return NULL;
}

/* The hot lookup. Caller holds font->lock and guarantees is_initialized.
   Returned pointer is valid until the next atlas_fetch on this font. */
static const TTF_AtlasGlyph* atlas_fetch(TTF_Font* f, int32_t cp) {
    int32_t gi = atlas_hash_find(f, cp);
    if (gi >= 0) {
        TTF_ATOMIC_FETCH_ADD(&f->hit_count, 1);
        f->glyphs[gi].lru = ++f->lru_stamp;
        return &f->glyphs[gi];
    }
    TTF_ATOMIC_FETCH_ADD(&f->miss_count, 1);

    if (f->glyph_count >= f->glyph_max)
        atlas_evict_rebuild(f);               /* keeps <= ~66% of entries */

    TTF_GlyphGeo geo;
    TTF_Bitmap ras;
    if (!render_glyph(f, cp, &geo, &ras)) return NULL;

    TTF_AtlasGlyph g;
    g.codepoint = cp;
    g.advance   = geo.advance;
    g.off_x = geo.off_x;  g.off_y = geo.off_y;
    g.w = geo.width;      g.h = geo.height;
    g.page = -1; g.x = 0; g.y = 0;
    g.lru = ++f->lru_stamp;

    if (g.w > 0 && g.h > 0 && ras.pixels) {
        int32_t p = atlas_place_budget(f, &g, f->atlas_max_bytes);
        if (p < 0) {
            atlas_evict_rebuild(f);
            p = atlas_place_budget(f, &g, f->atlas_max_bytes);
        }
        if (p >= 0) {
            TTF_AtlasPage* pg = &f->pages[p];
            for (int32_t row = 0; row < g.h; row++)
                TTF_MEMCPY(pg->pixels + (size_t)(g.y + row) * (size_t)pg->w + (size_t)g.x,
                           ras.pixels + (size_t)row * (size_t)ras.width,
                           (size_t)g.w);
            g.page = p;
            TTF_FREE(ras.pixels);
            ras.pixels = NULL;
        } else {
            /* Won't fit even after eviction (single glyph vs whole budget):
               serve from the transient slot, do NOT index it. */
            if (f->transient.pixels) TTF_FREE(f->transient.pixels);
            f->transient.pixels = ras.pixels;
            f->transient.width  = ras.width;
            f->transient.height = ras.height;
            g.page = -2;
            f->transient_glyph = g;
            return &f->transient_glyph;
        }
    } else if (ras.pixels) {
        /* ink rounded to zero at this size (tiny glyph + oversampling):
           metrics-only entry, discard the raster */
        TTF_FREE(ras.pixels);
        ras.pixels = NULL;
    }

    if (f->glyph_count == f->glyph_cap) {
        int32_t nc = f->glyph_cap ? f->glyph_cap * 2 : 64;
        TTF_AtlasGlyph* ng = (TTF_AtlasGlyph*)TTF_REALLOC(f->glyphs, sizeof(TTF_AtlasGlyph) * nc);
        if (!ng) return NULL;                 /* rendered but not retained */
        f->glyphs = ng;
        f->glyph_cap = nc;
    }
    f->glyphs[f->glyph_count] = g;
    atlas_hash_insert(f, cp, f->glyph_count); /* OOM: unindexed until next rebuild heals it */
    f->glyph_count++;
    return &f->glyphs[f->glyph_count - 1];
}

/* Max-composite blit from an atlas view (arbitrary stride) into a packed
   8bpp text bitmap. */
static void blit_glyph_max(const unsigned char* src, int32_t src_stride,
                           unsigned char* dst, int32_t dst_w, int32_t dst_h,
                           int32_t dx, int32_t dy, int32_t w, int32_t h) {
    for (int32_t yy = 0; yy < h; yy++) {
        int32_t py = dy + yy;
        if (py < 0 || py >= dst_h) continue;
        const unsigned char* srow = src + (size_t)yy * (size_t)src_stride;
        unsigned char* drow = dst + (size_t)py * (size_t)dst_w;
        for (int32_t xx = 0; xx < w; xx++) {
            int32_t px = dx + xx;
            if (px < 0 || px >= dst_w) continue;
            if (srow[xx] > drow[px]) drow[px] = srow[xx];
        }
    }
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
TTF_Font* TTF_CreateFont(int32_t cache_capacity) {
    /* cache_capacity = max retained glyphs. Atlas eviction is coarser than
       the old per-node LRU, so the default is raised 256 -> 2048. */
    if (cache_capacity <= 0) cache_capacity = 2048;
    if (cache_capacity < 256) cache_capacity = 256;
    if (cache_capacity > 65536) cache_capacity = 65536;

    TTF_Font* font = (TTF_Font*)TTF_MALLOC(sizeof(TTF_Font));
    if (!font) return NULL;
    TTF_MEMSET(font, 0, sizeof(TTF_Font));

    font->glyph_max = cache_capacity;

    /* Pixel budget: ~1KB per retained glyph, clamped to [512KB, 16MB].
       16px CJK glyphs (~300B ink) are entry-bound; large sizes become
       byte-bound earlier. Raise the capacity for big CJK sets. */
    uint64_t bytes = (uint64_t)cache_capacity * 1024ull;
    if (bytes < 512ull * 1024) bytes = 512ull * 1024;
    if (bytes > 16ull * 1024 * 1024) bytes = 16ull * 1024 * 1024;
    font->atlas_max_bytes = bytes;

    font->hash_size = next_power_of_two(cache_capacity * 2);
    font->hash = (int32_t*)TTF_MALLOC(sizeof(int32_t) * font->hash_size);
    if (!font->hash) { TTF_FREE(font); return NULL; }
    for (int32_t i = 0; i < font->hash_size; i++) font->hash[i] = -1;

    font->is_initialized = false;
    font->oversampling = 1;
    font->bold_strength = 0;
    font->italic_skew = 0.0f;
    TTF_MUTEX_INIT(font->lock);
    return font;
}

/* Drops every cached glyph (pages, entries, stats). Called on font reload and
   on parameter changes (pixel height / oversampling / style), because cached
   bitmaps are only valid for one parameter set. */
void TTF_ClearGlyphCache(TTF_Font *font) {
    if (!font) return;
    TTF_MUTEX_LOCK(font->lock);
    for (int32_t i = 0; i < font->page_count; i++) TTF_FREE(font->pages[i].pixels);
    font->page_count = 0;
    font->atlas_bytes = 0;
    font->glyph_count = 0;
    for (int32_t i = 0; i < font->hash_size; i++) font->hash[i] = -1;
    font->hash_used = 0;
    if (font->transient.pixels) {
        TTF_FREE(font->transient.pixels);
        font->transient.pixels = NULL;
        font->transient.width = 0;
        font->transient.height = 0;
    }
    font->hit_count = 0;
    font->miss_count = 0;
    font->evict_count = 0;
    TTF_MUTEX_UNLOCK(font->lock);
}

void TTF_DestroyFont(TTF_Font* font) {
    if (!font) return;
    TTF_ClearGlyphCache(font);
    TTF_MUTEX_DESTROY(font->lock);
    if (font->data) TTF_FREE(font->data);
    if (font->pages) TTF_FREE(font->pages);      /* page structs; pixels freed above */
    if (font->glyphs) TTF_FREE(font->glyphs);
    if (font->hash) TTF_FREE(font->hash);
    if (font->scratch) TTF_FREE(font->scratch);
    TTF_FREE(font);
}

void TTF_GetCacheStats(TTF_Font *font, TTF_CacheStats *out_stats) {
    if (!font || !out_stats) return;
    TTF_MUTEX_LOCK(font->lock);
    out_stats->hit_count = TTF_ATOMIC_LOAD(&font->hit_count);
    out_stats->miss_count = TTF_ATOMIC_LOAD(&font->miss_count);
    TTF_MUTEX_UNLOCK(font->lock);
}

void TTF_GetAtlasStats(TTF_Font *font, TTF_AtlasStats *out_stats) {
    if (!font || !out_stats) return;
    TTF_MUTEX_LOCK(font->lock);
    out_stats->hit_count = TTF_ATOMIC_LOAD(&font->hit_count);
    out_stats->miss_count = TTF_ATOMIC_LOAD(&font->miss_count);
    out_stats->evict_count = TTF_ATOMIC_LOAD(&font->evict_count);
    out_stats->page_count = font->page_count;
    out_stats->atlas_bytes = font->atlas_bytes;
    out_stats->atlas_max_bytes = font->atlas_max_bytes;
    out_stats->glyph_count = font->glyph_count;
    out_stats->glyph_max = font->glyph_max;
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
    font->ascent   = (int32_t)(ascent * font->scale);
    font->descent  = (int32_t)(descent * font->scale); /* negative */
    font->line_gap = (int32_t)(lineGap * font->scale);
    /* Typographic line spacing. Per-axis truncation can shave a pixel, so add
       one: a row box must be at least as tall as any glyph ink box, otherwise
       the next row's background erases the bottom edge of the row above. */
    font->line_height = font->ascent - font->descent + font->line_gap + 1;
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

// ---------------------------------------------------------------------------
// Measurement (pure metrics: no raster, no atlas)
// ---------------------------------------------------------------------------
void TTF_GetTextSize(TTF_Font *font, const char *text, int32_t *out_width, int32_t *out_height) {
    if (!font || !text || !font->is_initialized) return;
    int32_t len = (int32_t)TTF_STRLEN(text);
    int32_t x_pos = 0, max_x = 0, min_y = 0, max_y = 0, i = 0;

    TTF_MUTEX_LOCK(font->lock);
    while (i < len) {
        int32_t advance = 0;
        int32_t codepoint = utf8_to_codepoint(&text[i], len - i, &advance);
        TTF_GlyphGeo g;
        glyph_geo(font, codepoint, &g);
        int32_t top = font->ascent + g.off_y;
        int32_t bot = top + g.height;
        if (top < min_y) min_y = top;
        if (bot > max_y) max_y = bot;
        int32_t right = x_pos + g.off_x + g.width;
        if (right > max_x) max_x = right;
        x_pos += g.advance;
        if (i + advance < len) {
            int32_t next_advance = 0;
            int32_t next_cp = utf8_to_codepoint(&text[i + advance], len - (i + advance), &next_advance);
            int32_t kern = stbtt_GetCodepointKernAdvance(&font->info, codepoint, next_cp);
            x_pos += (int32_t)(kern * font->scale);
        }
        i += advance;
    }
    TTF_MUTEX_UNLOCK(font->lock);
    if (out_width) *out_width = max_x;
    if (out_height) *out_height = max_y - min_y;
}

/* Typographic line height (ascent - descent + line gap), in pixels. Text row
   layout MUST use this instead of the ink height of one run: ink height has no
   leading and lets adjacent rows clip each other's edges. */
int32_t TTF_GetLineHeight(TTF_Font *font) {
    if (!font || !font->is_initialized) return 0;
    return font->line_height;
}

// ---------------------------------------------------------------------------
// Per-glyph access
// ---------------------------------------------------------------------------
/* NOTE (lifetime): the returned pointer is valid only until the next TTF_*
   call on this font (atlas entries can be evicted by an LRU rebuild).
   Use TTF_RenderChar for an owned copy. */
const TTF_Bitmap* TTF_GetGlyphBitmap(TTF_Font *font, int32_t codepoint, int32_t *out_advance, int32_t *out_off_x, int32_t *out_off_y) {
    static TTF_Bitmap empty = {0};
    if (!font || !font->is_initialized) return NULL;

    TTF_MUTEX_LOCK(font->lock);
    const TTF_AtlasGlyph* g = atlas_fetch(font, codepoint);
    if (!g) { TTF_MUTEX_UNLOCK(font->lock); return NULL; }

    if (out_advance) *out_advance = g->advance;
    if (out_off_x) *out_off_x = g->off_x;
    if (out_off_y) *out_off_y = g->off_y;

    const TTF_Bitmap* result = &empty;
    if (g->page == -2) {
        result = &font->transient;              /* already packed, font-owned */
    } else if (g->w > 0 && g->h > 0) {
        int32_t stride = 0;
        const unsigned char* src = glyph_pixels(font, g, &stride);
        if (src) {
            size_t need = (size_t)g->w * (size_t)g->h;
            if (font->scratch_cap < need) {
                size_t nc = font->scratch_cap ? font->scratch_cap : 256;
                while (nc < need) nc *= 2;
                unsigned char* np = (unsigned char*)TTF_MALLOC(nc);
                if (np) {
                    TTF_FREE(font->scratch);
                    font->scratch = np;
                    font->scratch_cap = nc;
                }
            }
            if (font->scratch_cap >= need) {
                for (int32_t row = 0; row < g->h; row++)
                    TTF_MEMCPY(font->scratch + (size_t)row * (size_t)g->w,
                               src + (size_t)row * (size_t)stride, (size_t)g->w);
                font->scratch_view.pixels = font->scratch;
                font->scratch_view.width  = g->w;
                font->scratch_view.height = g->h;
                result = &font->scratch_view;
            }
        }
    }
    TTF_MUTEX_UNLOCK(font->lock);
    return result;
}

/* Returns an OWNED bitmap: release with TTF_FreeBitmap. */
TTF_Bitmap TTF_RenderChar(TTF_Font *font, int32_t codepoint, int32_t *out_x_offset, int32_t *out_y_offset) {
    TTF_Bitmap bmp = {0};
    if (!font || !font->is_initialized) return bmp;

    TTF_MUTEX_LOCK(font->lock);
    const TTF_AtlasGlyph* g = atlas_fetch(font, codepoint);
    if (g) {
        if (out_x_offset) *out_x_offset = g->off_x;
        if (out_y_offset) *out_y_offset = g->off_y;
        if (g->w > 0 && g->h > 0) {
            int32_t stride = 0;
            const unsigned char* src = glyph_pixels(font, g, &stride);
            if (src) {
                size_t mem = (size_t)g->w * (size_t)g->h;
                bmp.pixels = (unsigned char*)TTF_MALLOC(mem);
                if (bmp.pixels) {
                    for (int32_t row = 0; row < g->h; row++)
                        TTF_MEMCPY(bmp.pixels + (size_t)row * (size_t)g->w,
                                   src + (size_t)row * (size_t)stride, (size_t)g->w);
                    bmp.width = g->w;
                    bmp.height = g->h;
                }
            }
        }
    }
    TTF_MUTEX_UNLOCK(font->lock);
    return bmp;
}

// ---------------------------------------------------------------------------
// Text rendering (atlas -> packed 8bpp bitmap)
// ---------------------------------------------------------------------------
static void draw_line_to_buffer(TTF_Font *font, const char *text, int32_t len,
                                unsigned char *dst_pixels, int32_t dst_w, int32_t dst_h,
                                int32_t start_x, int32_t start_y, int32_t line_min_y) {
    int32_t x_pos = start_x;
    int32_t i = 0;
    TTF_MUTEX_LOCK(font->lock);
    while (i < len) {
        int32_t advance = 0;
        int32_t codepoint = utf8_to_codepoint(&text[i], len - i, &advance);
        const TTF_AtlasGlyph* g = atlas_fetch(font, codepoint);
        if (g) {
            if (g->w > 0 && g->h > 0) {
                int32_t stride = 0;
                const unsigned char* src = glyph_pixels(font, g, &stride);
                if (src) {
                    /* NOTE: x advances for ALL glyphs now, including spaces
                       (the legacy draw path skipped the advance when a glyph
                       had no pixels, collapsing spaces against the measured
                       line width). */
                    blit_glyph_max(src, stride, dst_pixels, dst_w, dst_h,
                                   x_pos + g->off_x,
                                   start_y + font->ascent + g->off_y - line_min_y,
                                   g->w, g->h);
                }
            }
            x_pos += g->advance;
        }
        if (i + advance < len) {
            int32_t next_advance = 0;
            int32_t next_cp = utf8_to_codepoint(&text[i + advance], len - (i + advance), &next_advance);
            int32_t kern = stbtt_GetCodepointKernAdvance(&font->info, codepoint, next_cp);
            x_pos += (int32_t)(kern * font->scale);
        }
        i += advance;
    }
    TTF_MUTEX_UNLOCK(font->lock);
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

    /* Pass 1 (measure): pure metrics - no rasterization, no atlas traffic. */
    TTF_MUTEX_LOCK(font->lock);
    while (i < len) {
        if (cmd_count >= cmd_capacity) {
            cmd_capacity *= 2;
            TTF_RenderCmd* new_cmds = (TTF_RenderCmd*)TTF_REALLOC(cmds, sizeof(TTF_RenderCmd) * cmd_capacity);
            if (!new_cmds) { TTF_MUTEX_UNLOCK(font->lock); TTF_FREE(cmds); return false; }
            cmds = new_cmds;
        }

        int32_t advance = 0;
        int32_t codepoint = utf8_to_codepoint(&text[i], len - i, &advance);
        TTF_RenderCmd* cmd = &cmds[cmd_count++];
        cmd->codepoint = codepoint;
        cmd->x_advance = 0;

        TTF_GlyphGeo g;
        glyph_geo(font, codepoint, &g);
        int32_t top = font->ascent + g.off_y;
        int32_t bot = top + g.height;
        if (top < min_y) min_y = top;
        if (bot > max_y) max_y = bot;
        int32_t right = x_pos + g.off_x + g.width;
        if (right > max_x) max_x = right;
        cmd->x_advance = g.advance;

        if (i + advance < len) {
            int32_t next_advance = 0;
            int32_t next_cp = utf8_to_codepoint(&text[i + advance], len - (i + advance), &next_advance);
            int32_t kern = stbtt_GetCodepointKernAdvance(&font->info, codepoint, next_cp);
            cmd->x_advance += (int32_t)(kern * font->scale);
        }
        x_pos += cmd->x_advance;
        i += advance;
    }
    TTF_MUTEX_UNLOCK(font->lock);

    int32_t text_width = max_x;
    int32_t text_height = max_y - min_y;
    if (text_width <= 0 || text_height <= 0) { TTF_FREE(cmds); return false; }

    // 如果传入的 buffer 不够大，重新分配
    if (!out_bmp->pixels || out_bmp->width < text_width || out_bmp->height < text_height) {
        if (out_bmp->pixels) TTF_FREE(out_bmp->pixels);
        size_t mem_size = (size_t)text_width * (size_t)text_height;
        if (mem_size / (size_t)text_width != (size_t)text_height) { TTF_FREE(cmds); return false; }
        out_bmp->pixels = (unsigned char*)TTF_MALLOC(mem_size);
        if (!out_bmp->pixels) { TTF_FREE(cmds); return false; }
    }

    out_bmp->width = text_width;
    out_bmp->height = text_height;
    TTF_MEMSET(out_bmp->pixels, 0, (size_t)text_width * text_height);

    /* Pass 2 (draw): one atlas fetch per glyph. Repeat characters inside
       the string hit the atlas after their first appearance. */
    TTF_MUTEX_LOCK(font->lock);
    int32_t x_pos2 = 0;
    for (int32_t k = 0; k < cmd_count; k++) {
        const TTF_AtlasGlyph* g = atlas_fetch(font, cmds[k].codepoint);
        if (g && g->w > 0 && g->h > 0) {
            int32_t stride = 0;
            const unsigned char* src = glyph_pixels(font, g, &stride);
            if (src) {
                blit_glyph_max(src, stride, out_bmp->pixels, text_width, text_height,
                               x_pos2 + g->off_x,
                               font->ascent + g->off_y - min_y,
                               g->w, g->h);
            }
        }
        x_pos2 += cmds[k].x_advance;
    }
    TTF_MUTEX_UNLOCK(font->lock);

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

    /* Measure phase: pure metrics via glyph_geo (no rasterization). */
    TTF_MUTEX_LOCK(font->lock);
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

            TTF_GlyphGeo g;
            glyph_geo(font, codepoint, &g);
            int32_t char_w = g.advance;

            if (max_width > 0 && x_pos + char_w > max_width) {
                if (is_punct_no_start(codepoint) && i > line_start) {
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

            int32_t top = font->ascent + g.off_y;
            int32_t bot = top + g.height;
            if (top < min_y) min_y = top;
            if (bot > max_y) max_y = bot;
            x_pos += char_w;
            if (codepoint == ' ') last_space = i;
            i += advance;
            line_end = i;
        }

        if (line_end > line_start) {
            if (line_count >= lines_capacity) {
                lines_capacity *= 2;
                LineInfo *new_lines = (LineInfo*)TTF_REALLOC(lines, sizeof(LineInfo) * lines_capacity);
                if (!new_lines) { TTF_MUTEX_UNLOCK(font->lock); TTF_FREE(lines); return final_bmp; }
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
    TTF_MUTEX_UNLOCK(font->lock);

    if (line_count == 0) { TTF_FREE(lines); return final_bmp; }
    total_height -= line_gap;

    final_bmp.width = max_width > 0 ? max_width : max_line_width;
    final_bmp.height = total_height;
    size_t mem_size = (size_t)final_bmp.width * (size_t)final_bmp.height;
    if (mem_size / (size_t)final_bmp.width != (size_t)final_bmp.height) { TTF_FREE(lines); final_bmp.width = 0; return final_bmp; }

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

uint8_t TTF_ReadFont(
    TTF_Font **out_font, const char* path,
    int32_t pixel_height, int32_t CacheCap
) {
    if (!out_font) return 254;
    /* CacheCap = max retained glyphs (atlas entries); pixel memory budget
       is derived from it (~1KB/glyph, clamped [512KB, 16MB]). */
    *out_font = TTF_CreateFont(CacheCap);
    if (!(*out_font)) return 1;

    FILE* fd = fopen(path, "r");
    if (fd == NULL) {
        TTF_DestroyFont(*out_font);
        *out_font = NULL;
        return 2;
    }

    uint64_t file_size = fsize(fd);
    if (file_size == 0) {
        fclose(fd);
        TTF_DestroyFont(*out_font);
        *out_font = NULL;
        return 3;
    }

    /* Read the file straight into the font's persistent data buffer: one
       allocation, no copy. */
    TTF_Font* rf = *out_font;
    rf->data = (unsigned char*)malloc(file_size);
    if (!rf->data) {
        fclose(fd);
        TTF_DestroyFont(rf);
        *out_font = NULL;
        return 4;
    }

    size_t read_bytes = fread(rf->data, 1, file_size, fd);
    fclose(fd);

    if (read_bytes != file_size) {
        free(rf->data); rf->data = NULL;
        TTF_DestroyFont(rf);
        *out_font = NULL;
        return 6;
    }

    int32_t foff = stbtt_GetFontOffsetForIndex(rf->data, 0);
    if (foff < 0 || !stbtt_InitFont(&rf->info, rf->data, foff)) {
        free(rf->data); rf->data = NULL;
        TTF_DestroyFont(rf);
        *out_font = NULL;
        return 5;
    }
    rf->is_initialized = true;
    TTF_SetPixelHeight(rf, pixel_height);
    TTF_SetOversampling(rf, 2);
    return 0;
}

void TTF_DrawText(
FrameBuffer *FB, TTF_Font *TTFFont,
int32_t x, int32_t y, const char* text, uint32_t color
) {
    if (!TTFFont || !FB || !FB->BaseAddress || !text || !TTFFont->is_initialized) return;

    /* Direct atlas -> framebuffer path: no intermediate text bitmap, no
       scratch allocation, no full-rect memset. Per-pixel work is done only
       where glyphs actually have ink; steady-state cost per character is one
       hash probe plus the ink-pixel blends. GUI text drawing is
       single-threaded; the font lock is held across both passes. */
    uint32_t* fb_ptr = (uint32_t*)FB->BaseAddress;
    int32_t fb_w = (int32_t)FB->Width;
    int32_t fb_h = (int32_t)FB->Height;
    int32_t fb_pitch = (int32_t)FB->PixelsPerScanLine;

    uint8_t cr = (color >> 16) & 0xFF;
    uint8_t cg = (color >> 8) & 0xFF;
    uint8_t cb = color & 0xFF;
    uint32_t opaque = 0xFF000000u | ((uint32_t)cr << 16) | ((uint32_t)cg << 8) | cb;

    TTF_MUTEX_LOCK(TTFFont->lock);

    int32_t len = (int32_t)TTF_STRLEN(text);

    /* Pass 1 (metrics only): min_y for pixel-exact vertical parity with the
       previous scratch-bitmap implementation (ink-top anchoring). */
    int32_t min_y = 0;
    for (int32_t i = 0; i < len; ) {
        int32_t advance = 0;
        int32_t cp = utf8_to_codepoint(&text[i], len - i, &advance);
        TTF_GlyphGeo g;
        glyph_geo(TTFFont, cp, &g);
        int32_t top = TTFFont->ascent + g.off_y;
        if (top < min_y) min_y = top;
        i += advance;
    }

    /* Pass 2: blit glyphs straight from the atlas into the framebuffer. */
    int32_t x_pos = 0;
    for (int32_t i = 0; i < len; ) {
        int32_t advance = 0;
        int32_t cp = utf8_to_codepoint(&text[i], len - i, &advance);
        const TTF_AtlasGlyph* gl = atlas_fetch(TTFFont, cp);
        if (gl && gl->w > 0 && gl->h > 0) {
            int32_t stride = 0;
            const unsigned char* src = glyph_pixels(TTFFont, gl, &stride);
            if (src) {
                int32_t draw_x = x + x_pos + gl->off_x;
                int32_t draw_y = y + TTFFont->ascent + gl->off_y - min_y;

                int32_t y0 = draw_y < 0 ? -draw_y : 0;
                int32_t y1 = gl->h; if (draw_y + y1 > fb_h) y1 = fb_h - draw_y;
                int32_t x0 = draw_x < 0 ? -draw_x : 0;
                int32_t x1 = gl->w; if (draw_x + x1 > fb_w) x1 = fb_w - draw_x;

                for (int32_t yy = y0; yy < y1; yy++) {
                    const unsigned char* srow = src + (size_t)yy * (size_t)stride;
                    uint32_t* drow = fb_ptr + (size_t)(draw_y + yy) * (size_t)fb_pitch;
                    for (int32_t xx = x0; xx < x1; xx++) {
                        uint8_t a = srow[xx];
                        if (a == 0) continue;
                        uint32_t* dp = &drow[draw_x + xx];
                        if (a == 255) { *dp = opaque; continue; }
                        uint32_t bg = *dp;
                        uint8_t inv = (uint8_t)(255 - a);
                        uint8_t mr = (uint8_t)(((uint32_t)cr * a + ((bg >> 16) & 0xFF) * inv + 128) >> 8);
                        uint8_t mg = (uint8_t)(((uint32_t)cg * a + ((bg >> 8) & 0xFF) * inv + 128) >> 8);
                        uint8_t mb = (uint8_t)(((uint32_t)cb * a + (bg & 0xFF) * inv + 128) >> 8);
                        *dp = 0xFF000000u | ((uint32_t)mr << 16) | ((uint32_t)mg << 8) | mb;
                    }
                }
            }
        }
        if (gl) x_pos += gl->advance;
        if (i + advance < len) {
            int32_t next_advance = 0;
            int32_t next_cp = utf8_to_codepoint(&text[i + advance], len - (i + advance), &next_advance);
            int32_t kern = stbtt_GetCodepointKernAdvance(&TTFFont->info, cp, next_cp);
            x_pos += (int32_t)(kern * TTFFont->scale);
        }
        i += advance;
    }

    TTF_MUTEX_UNLOCK(TTFFont->lock);
}