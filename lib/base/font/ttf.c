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
/* stdio is needed by the file-backed loaders (fopen/fread/fseek/fsize) AND by
   the lazy-glyf window, which lives in TTF_Font — so it cannot stay next to
   TTF_ReadFont() like it used to. */
#include <stdio.h>
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

    /* ---- lazy glyf: outlines fetched per glyph (TTF_ReadFontGlyfLazy) ---- */
    bool           glyf_lazy;      /* glyf lives on disk, window in data[]   */
    FILE*          glyf_fd;        /* held open for the font's lifetime      */
    uint32_t       glyf_file_off;  /* absolute offset of glyf in the file    */
    uint32_t       glyf_file_len;
    size_t         glyf_win_off;   /* where the window starts inside data[]  */
    size_t         glyf_win_cap;   /* window bytes available                 */
    size_t         glyf_win_used;  /* window bytes consumed                  */
    uint32_t*      glyf_loc_orig;  /* unpatched loca: gid -> offset in glyf  */
    unsigned char* glyf_scratch;   /* aligned read staging buffer            */
    size_t         glyf_scratch_cap;
    int32_t*       win_gids;       /* glyph ids currently resident           */
    int32_t        win_count, win_cap;
    unsigned long  glyf_fetches, glyf_bytes;
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

// ---------------------------------------------------------------------------
// Lazy glyf: per-glyph on-demand outline fetch
// ---------------------------------------------------------------------------
/* TTF_ReadFontGlyfLazy() leaves the outline table (`glyf`) on disk. The font
   image still carries a *directory entry* for glyf, but it points at a sliding
   window at the tail of the image; a glyph only becomes usable once its byte
   slice has been read into that window AND its two `loca` entries have been
   rewritten to address it. stb_truetype resolves glyph offsets through
   stbtt__GetGlyfOffset() = data + glyf + loca[gid] on *every* access, so
   patching loca is sufficient — the rasterizer needs no changes.

   Resident glyphs are listed in win_gids[]. A glyph that is not resident has
   its loca pair zeroed, which makes stbtt report "empty glyph" (-1) instead of
   reading outside the window. Composite glyphs drag their components in with
   them (bounded depth), so a component reference never lands on a hole.

   Only long-loca fonts are eligible: with short loca the whole glyf table is
   by definition < 256 KB, so lazy-loading it would be pure overhead. */

#define TTF_GLYF_WIN_DEFAULT  (512u * 1024u)   /* sliding window, bytes     */
#define TTF_GLYF_WIN_SLOTS    256              /* initial win_gids capacity */
#define TTF_GLYF_MAX_DEPTH    8                /* composite nesting guard   */

/* Write loca[gid] = v. Long format only (v is a raw byte offset). */
static void ttf_loca_set(TTF_Font* f, int32_t gid, uint32_t v) {
    unsigned char* p = f->data + f->info.loca + (size_t)gid * 4u;
    p[0] = (unsigned char)((v >> 24) & 0xFF);
    p[1] = (unsigned char)((v >> 16) & 0xFF);
    p[2] = (unsigned char)((v >> 8) & 0xFF);
    p[3] = (unsigned char)(v & 0xFF);
}
static uint32_t ttf_loca_get(const TTF_Font* f, int32_t gid) {
    const unsigned char* p = f->data + f->info.loca + (size_t)gid * 4u;
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

/* Drop every resident glyph. Called only from the root of a load, never while
   a partially-loaded composite chain is in flight. */
static void ttf_glyf_reset(TTF_Font* f) {
    for (int32_t i = 0; i < f->win_count; i++) {
        int32_t gid = f->win_gids[i];
        ttf_loca_set(f, gid, 0);
        if (gid + 1 <= f->info.numGlyphs) ttf_loca_set(f, gid + 1, 0);
    }
    f->win_count     = 0;
    f->glyf_win_used = 0;
}

/* Grow the image so the window can hold `need` bytes. The window lives at the
   tail, so no other table moves — only info.data has to be refreshed. */
static bool ttf_glyf_grow(TTF_Font* f, size_t need) {
    size_t cap = f->glyf_win_cap ? f->glyf_win_cap : 8192u;
    while (cap < need) {
        if (cap > (size_t)1 << 30) return false;
        cap *= 2;
    }
    unsigned char* nd = (unsigned char*)TTF_REALLOC(f->data, f->glyf_win_off + cap);
    if (!nd) return false;
    f->data       = nd;
    f->info.data  = nd;          /* stbtt caches the pointer, not just offsets */
    f->glyf_win_cap = cap;
    return true;
}

static bool ttf_glyf_slot_push(TTF_Font* f, int32_t gid) {
    if (f->win_count == f->win_cap) {
        int32_t nc = f->win_cap ? f->win_cap * 2 : TTF_GLYF_WIN_SLOTS;
        if (nc > 65536) return false;
        int32_t* ng = (int32_t*)TTF_REALLOC(f->win_gids, sizeof(int32_t) * (size_t)nc);
        if (!ng) return false;
        f->win_gids = ng;
        f->win_cap  = nc;
    }
    f->win_gids[f->win_count++] = gid;
    return true;
}

/* Read one glyph (and, for composites, its components) into the window. */
static bool ttf_glyf_load(TTF_Font* f, int32_t gid, int depth) {
    if (gid < 0 || gid >= f->info.numGlyphs) return false;
    for (int32_t i = 0; i < f->win_count; i++)
        if (f->win_gids[i] == gid) return true;      /* already resident */

    uint32_t a = f->glyf_loc_orig[gid];
    uint32_t b = f->glyf_loc_orig[gid + 1];
    uint32_t len = (b > a) ? (b - a) : 0;

    if (len == 0) {                    /* blank glyph: zero-length loca pair */
        ttf_loca_set(f, gid, 0);
        ttf_loca_set(f, gid + 1, 0);
        return ttf_glyf_slot_push(f, gid);
    }
    /* stay inside the glyf table: a corrupt loca must not read past it */
    if (a > f->glyf_file_len || len > f->glyf_file_len - a) return false;

    uint32_t pad = (len + 3u) & ~3u;                 /* keep offsets aligned */
    if (f->glyf_win_used + pad > f->glyf_win_cap) {
        if (depth == 0 && pad <= f->glyf_win_cap) {
            ttf_glyf_reset(f);                       /* recycle the window   */
        } else if (!ttf_glyf_grow(f, f->glyf_win_used + pad)) {
            return false;                            /* cannot evict: chained */
        }
        if (f->glyf_win_used + pad > f->glyf_win_cap) return false;
    }

    size_t  at   = f->glyf_win_used;
    size_t  base = f->glyf_win_off + at;

    /* Read the slice on a block-aligned boundary.
       Why: the kernel's block cache keys a cached block by (file, block#) and
       serves it as "the block starting at the block boundary". A short read at
       an unaligned offset therefore poisons every later read inside that block.
       Glyph slices are ~500 B, so a 4 KB block holds ~8 of them and consecutive
       loads would corrupt each other. Reading from the block start keeps us
       correct no matter how the cache behaves, and it is also the shape the
       cache promotes best — later glyphs in the same block become real hits. */
    const uint64_t BLK = 4096u;
    uint64_t blk  = (uint64_t)a & ~(BLK - 1u);
    uint32_t need = (uint32_t)(a - (uint32_t)blk) + len;   /* <= BLK + len */
    if (f->glyf_scratch_cap < (size_t)need) {
        size_t nc = BLK * 2;
        while (nc < (size_t)need) nc *= 2;
        unsigned char* ns = (unsigned char*)TTF_REALLOC(f->glyf_scratch, nc);
        if (!ns) return false;
        f->glyf_scratch = ns;
        f->glyf_scratch_cap = nc;
    }
    if (fseek(f->glyf_fd, (long)(f->glyf_file_off + blk), SEEK_SET) != 0) return false;
    if (fread(f->glyf_scratch, 1, (size_t)need, f->glyf_fd) != (size_t)need) return false;
    TTF_MEMCPY(f->data + base, f->glyf_scratch + (a - (uint32_t)blk), (size_t)len);
    TTF_MEMSET(f->data + base + len, 0, (size_t)(pad - len));

    ttf_loca_set(f, gid, (uint32_t)at);
    ttf_loca_set(f, gid + 1, (uint32_t)(at + len));
    if (!ttf_glyf_slot_push(f, gid)) return false;
    f->glyf_win_used += pad;
    f->glyf_fetches++;
    f->glyf_bytes += len;

    /* Composite: walk the component records and pull each child in. `gd` is
       re-derived every iteration because a child load may realloc data[]. */
    if (depth < TTF_GLYF_MAX_DEPTH && len >= 10) {
        const unsigned char* gd = f->data + base;
        int32_t nc = (int32_t)(int16_t)(uint16_t)(((uint16_t)gd[0] << 8) | gd[1]);
        if (nc < 0) {
            size_t o = 10;
            for (;;) {
                if (o + 4 > len) break;
                uint32_t flags = ((uint32_t)gd[o] << 8) | gd[o + 1];
                uint32_t child = ((uint32_t)gd[o + 2] << 8) | gd[o + 3];
                o += 4;
                o += (flags & 0x0001u) ? 4u : 2u;            /* args are words */
                if      (flags & 0x0008u) o += 2u;           /* single scale   */
                else if (flags & 0x0040u) o += 4u;           /* x/y scale      */
                else if (flags & 0x0080u) o += 8u;           /* 2x2            */
                if (child < (uint32_t)f->info.numGlyphs)
                    ttf_glyf_load(f, (int32_t)child, depth + 1);
                if (!(flags & 0x0020u)) break;               /* MORE_COMPONENTS */
                gd = f->data + base;                         /* may have moved */
            }
        }
    }
    return true;
}

/* Make glyph `gid` addressable. No-op for non-lazy fonts and for gids already
   resident; a failed fetch leaves the glyph blank rather than crashing. */
static void ttf_glyph_ensure(TTF_Font* f, int32_t gid) {
    if (!f->glyf_lazy || !f->glyf_fd || gid < 0) return;
    for (int32_t i = 0; i < f->win_count; i++)
        if (f->win_gids[i] == gid) return;
    ttf_glyf_load(f, gid, 0);
}

/* Release every lazily-held resource. Safe on a non-lazy font. */
static void ttf_glyf_teardown(TTF_Font* f) {
    if (!f) return;
    if (f->glyf_fd) fclose(f->glyf_fd);
    if (f->glyf_loc_orig) TTF_FREE(f->glyf_loc_orig);
    if (f->glyf_scratch) TTF_FREE(f->glyf_scratch);
    if (f->win_gids) TTF_FREE(f->win_gids);
    f->glyf_fd = NULL;
    f->glyf_loc_orig = NULL;
    f->glyf_scratch = NULL;
    f->win_count = f->win_cap = 0;
    f->glyf_lazy = false;
    f->glyf_win_off = f->glyf_win_cap = f->glyf_win_used = 0;
    f->glyf_file_off = f->glyf_file_len = 0;
    f->glyf_fetches = f->glyf_bytes = 0;
}

/* Caller holds font->lock. Pure metrics: no rasterization, no allocation,
   no atlas interaction. Mirrors the transform chain of render_glyph exactly
   (bold smears inside the existing box; italic widens and shifts;
   oversampling divides dims and offsets). */
static void glyph_geo(TTF_Font* font, int32_t codepoint, TTF_GlyphGeo* g) {
    /* Lazy fonts must have the outline resident before stbtt reads its bbox. */
    if (font->glyf_lazy)
        ttf_glyph_ensure(font, stbtt_FindGlyphIndex(&font->info, codepoint));
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
    ttf_glyf_teardown(font);            /* closes the glyf fd, frees the maps */
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
    ttf_glyf_teardown(font);                /* a full image needs no window   */
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

/* ---- on-demand font loads ----------------------------------------------
   The classic loader slurps the whole file into one allocation. These two
   variants read the sfnt directory and fetch only what stb_truetype actually
   touches — GSUB, GPOS, name, post, DSIG, vmtx/vhea, VORG, BASE, GDEF stay on
   disk. Each kept table is read with its own seek+read, so the kernel file
   cache serves them.

     TTF_ReadFontLazy()      keeps cmap/glyf/head/hhea/hmtx/loca/maxp.
                             Saves the unused tables only; for CJK fonts glyf
                             is ~96% of the file, so this barely helps.

     TTF_ReadFontGlyfLazy()  keeps the same MINUS glyf. The image still carries
                             a glyf *directory record*, but it points at a
                             sliding window that holds only the glyphs actually
                             used. See the lazy-glyf block above for the
                             mechanics (loca patching, composite pull-in).

   Anything unexpected falls back to the whole-file loader. */
static const char* const kTTFNeeded[] = {
    "cmap", "glyf", "head", "hhea", "hmtx", "loca", "maxp", 0
};
/* glyph-granular: glyf is not copied in, only addressed through the window */
static const char* const kTTFMetaOnly[] = {
    "cmap", "head", "hhea", "hmtx", "loca", "maxp", 0
};

static int ttf_tag_is(const char* tag4, const char* t) {
    return tag4[0] == t[0] && tag4[1] == t[1] && tag4[2] == t[2] && tag4[3] == t[3];
}
static int ttf_table_in(const char* tag4, const char* const* list) {
    for (int i = 0; list[i]; i++) if (ttf_tag_is(tag4, list[i])) return 1;
    return 0;
}

static uint32_t ttf_rec_u32(const unsigned char* r, int at) {
    return ((uint32_t)r[at] << 24) | ((uint32_t)r[at + 1] << 16) |
           ((uint32_t)r[at + 2] << 8) | (uint32_t)r[at + 3];
}

/* Shared builder. per_glyph=false -> table-granular; per_glyph=true -> also
   stream glyf one slice at a time out of the sliding window. */
static uint8_t ttf_read_lazy(TTF_Font** out_font, const char* path,
                             int32_t pixel_height, int32_t CacheCap,
                             bool per_glyph) {
    if (!out_font) return 254;
    *out_font = TTF_CreateFont(CacheCap);
    if (!(*out_font)) return 1;
    TTF_Font* rf = *out_font;
    unsigned char* recs = NULL;

    FILE* fd = fopen(path, "r");
    if (!fd) { TTF_DestroyFont(rf); *out_font = NULL; return 2; }
    uint64_t file_size = fsize(fd);
    if (file_size == 0) goto fallback;

    unsigned char hdr[12];
    if (fread(hdr, 1, 12, fd) != 12) goto fallback;
    /* TrueType only ('\0\1\0\0'); TTC collections keep the old path */
    if (!(hdr[0] == 0 && hdr[1] == 1 && hdr[2] == 0 && hdr[3] == 0)) goto fallback;
    uint32_t ntab = ((uint32_t)hdr[4] << 8) | (uint32_t)hdr[5];
    if (ntab == 0 || ntab > 1024) goto fallback;

    size_t dirBytes = (size_t)ntab * 16u;
    recs = (unsigned char*)TTF_MALLOC(dirBytes);
    if (!recs) goto fallback;
    if (fread(recs, 1, dirBytes, fd) != dirBytes) goto fallback;

    uint32_t glyf_off = 0, glyf_len = 0, head_off = 0;
    bool have_glyf = false;
    for (uint32_t i = 0; i < ntab; i++) {
        const unsigned char* r = recs + i * 16u;
        if (r[0] == 'g' && r[1] == 'l' && r[2] == 'y' && r[3] == 'f') {
            glyf_off = ttf_rec_u32(r, 8);
            glyf_len = ttf_rec_u32(r, 12);
            have_glyf = true;
        } else if (r[0] == 'h' && r[1] == 'e' && r[2] == 'a' && r[3] == 'd') {
            head_off = ttf_rec_u32(r, 8);
        }
    }
    if (!have_glyf) goto fallback;

    if (per_glyph) {
        /* Long loca only: short loca caps glyf at ~256 KB, which is not worth
           streaming, and the 16-bit entries cannot address a >128 KB window. */
        int locfmt = -1;
        unsigned char hb[54];
        if (head_off && fseek(fd, (long)head_off, SEEK_SET) == 0 &&
            fread(hb, 1, 54, fd) == 54)
            locfmt = (int)(int16_t)(uint16_t)(((uint16_t)hb[50] << 8) | hb[51]);
        if (locfmt != 1) per_glyph = false;
    }

    const char* const* list  = per_glyph ? kTTFMetaOnly : kTTFNeeded;
    uint32_t           wantN = per_glyph ? 6u : 7u;

    /* first pass: how many tables do we keep, and how big is the image? */
    uint32_t keep = 0;
    size_t   body = 0;
    for (uint32_t i = 0; i < ntab; i++) {
        const unsigned char* r = recs + i * 16u;
        char tag4[5] = { (char)r[0], (char)r[1], (char)r[2], (char)r[3], 0 };
        if (!ttf_table_in(tag4, list)) continue;
        keep++;
        body += ((size_t)ttf_rec_u32(r, 12) + 3u) & ~(size_t)3u;
    }
    if (keep != wantN) goto fallback;              /* a table is missing */

    const uint32_t slots  = keep + (per_glyph ? 1u : 0u);   /* + glyf record */
    const size_t   winOff = 12u + (size_t)slots * 16u + body;
    const size_t   outSize = winOff + (per_glyph ? (size_t)TTF_GLYF_WIN_DEFAULT : 0u);

    unsigned char* out = (unsigned char*)TTF_MALLOC(outSize);
    if (!out) goto fallback;
    TTF_MEMSET(out, 0, outSize);

    unsigned char* oh = out;
    oh[0] = hdr[0]; oh[1] = hdr[1]; oh[2] = hdr[2]; oh[3] = hdr[3];
    oh[4] = (unsigned char)(slots >> 8); oh[5] = (unsigned char)(slots & 0xFF);
    /* searchRange / entrySelector / rangeShift are only hints for a binary
       search stb_truetype does not perform (it scans the directory), so the
       original values are copied verbatim. */
    oh[6] = hdr[6];  oh[7] = hdr[7];  oh[8] = hdr[8];  oh[9] = hdr[9];
    oh[10] = hdr[10]; oh[11] = hdr[11];

    size_t   cursor = 12u + (size_t)slots * 16u;
    uint32_t slot   = 0;
    for (uint32_t i = 0; i < ntab; i++) {
        const unsigned char* r = recs + i * 16u;
        char tag4[5] = { (char)r[0], (char)r[1], (char)r[2], (char)r[3], 0 };
        if (!ttf_table_in(tag4, list)) continue;
        uint32_t off = ttf_rec_u32(r, 8);
        uint32_t len = ttf_rec_u32(r, 12);

        if (fseek(fd, (long)off, SEEK_SET) != 0 ||
            fread(out + cursor, 1, (size_t)len, fd) != (size_t)len) {
            TTF_FREE(out);
            goto fallback;
        }
        unsigned char* orec = out + 12u + (size_t)slot * 16u;
        orec[0] = r[0]; orec[1] = r[1]; orec[2] = r[2]; orec[3] = r[3];
        orec[4] = r[4]; orec[5] = r[5]; orec[6] = r[6]; orec[7] = r[7]; /* checksum */
        orec[8]  = (unsigned char)((cursor >> 24) & 0xFF);
        orec[9]  = (unsigned char)((cursor >> 16) & 0xFF);
        orec[10] = (unsigned char)((cursor >> 8) & 0xFF);
        orec[11] = (unsigned char)(cursor & 0xFF);
        orec[12] = r[12]; orec[13] = r[13]; orec[14] = r[14]; orec[15] = r[15];
        cursor += ((size_t)len + 3u) & ~(size_t)3u;
        slot++;
    }
    if (per_glyph) {                 /* glyf record -> the sliding window     */
        unsigned char* orec = out + 12u + (size_t)slot * 16u;
        orec[0] = 'g'; orec[1] = 'l'; orec[2] = 'y'; orec[3] = 'f';
        orec[4] = orec[5] = orec[6] = orec[7] = 0;
        orec[8]  = (unsigned char)((winOff >> 24) & 0xFF);
        orec[9]  = (unsigned char)((winOff >> 16) & 0xFF);
        orec[10] = (unsigned char)((winOff >> 8) & 0xFF);
        orec[11] = (unsigned char)(winOff & 0xFF);
        orec[12] = (unsigned char)((glyf_len >> 24) & 0xFF);
        orec[13] = (unsigned char)((glyf_len >> 16) & 0xFF);
        orec[14] = (unsigned char)((glyf_len >> 8) & 0xFF);
        orec[15] = (unsigned char)(glyf_len & 0xFF);
        slot++;
    }
    TTF_FREE(recs);
    recs = NULL;

    if (!stbtt_InitFont(&rf->info, out, 0)) { TTF_FREE(out); goto fallback; }
    rf->data = out;                  /* owned by the font, freed on destroy */
    rf->is_initialized = true;

    if (per_glyph) {
        int32_t ng = rf->info.numGlyphs;
        if (ng <= 0 || rf->info.indexToLocFormat != 1 ||
            rf->info.loca == 0 || rf->info.glyf == 0) {
            TTF_FREE(out); rf->data = NULL; rf->is_initialized = false;
            goto fallback;
        }
        rf->glyf_loc_orig = (uint32_t*)TTF_MALLOC(sizeof(uint32_t) * (size_t)(ng + 1));
        if (!rf->glyf_loc_orig) {
            TTF_FREE(out); rf->data = NULL; rf->is_initialized = false;
            goto fallback;
        }
        for (int32_t i = 0; i <= ng; i++) rf->glyf_loc_orig[i] = ttf_loca_get(rf, i);
        /* nothing is resident yet: every loca pair reads as zero-length, which
           stbtt reports as "empty glyph" instead of reading past the window. */
        TTF_MEMSET(rf->data + rf->info.loca, 0, (size_t)(ng + 1) * 4u);

        rf->glyf_lazy     = true;
        rf->glyf_fd       = fd;      /* owned by the font from here on */
        rf->glyf_file_off = glyf_off;
        rf->glyf_file_len = glyf_len;
        rf->glyf_win_off  = winOff;
        rf->glyf_win_cap  = (size_t)TTF_GLYF_WIN_DEFAULT;
        rf->glyf_win_used = 0;
        fd = NULL;
    }

    TTF_SetPixelHeight(rf, pixel_height);
    TTF_SetOversampling(rf, 2);
    return 0;

fallback:
    if (recs) TTF_FREE(recs);
    if (fd) fclose(fd);
    TTF_DestroyFont(rf);
    return TTF_ReadFont(out_font, path, pixel_height, CacheCap);
}

uint8_t TTF_ReadFontLazy(TTF_Font** out_font, const char* path,
                         int32_t pixel_height, int32_t CacheCap) {
    return ttf_read_lazy(out_font, path, pixel_height, CacheCap, false);
}

/* Emergency switch for the per-glyph loader.
   1 (default): stream `glyf` one glyph at a time out of the sliding window.
   0          : behave exactly like TTF_ReadFont() (slurp the whole file).

   Flip it (add -DTTF_GLYF_LAZY=0 to the build, or edit this default) if a
   target's kernel misbehaves under the "many small scattered reads" pattern —
   that is the only respect in which this loader differs from the old one. */
#ifndef TTF_GLYF_LAZY
#define TTF_GLYF_LAZY 0
#endif

uint8_t TTF_ReadFontGlyfLazy(TTF_Font** out_font, const char* path,
                             int32_t pixel_height, int32_t CacheCap) {
#if !TTF_GLYF_LAZY
    return TTF_ReadFont(out_font, path, pixel_height, CacheCap);
#else
    return ttf_read_lazy(out_font, path, pixel_height, CacheCap, true);
#endif
}

void TTF_GetGlyfStats(TTF_Font *font, unsigned long *out_cap,
                      unsigned long *out_resident, unsigned long *out_fetches,
                      unsigned long *out_bytes) {
    if (out_cap)      *out_cap = 0;
    if (out_resident) *out_resident = 0;
    if (out_fetches)  *out_fetches = 0;
    if (out_bytes)    *out_bytes = 0;
    if (!font || !font->glyf_lazy) return;
    TTF_MUTEX_LOCK(font->lock);
    if (out_cap)      *out_cap = (unsigned long)font->glyf_win_cap;
    if (out_resident) *out_resident = (unsigned long)font->glyf_win_used;
    if (out_fetches)  *out_fetches = font->glyf_fetches;
    if (out_bytes)    *out_bytes = font->glyf_bytes;
    TTF_MUTEX_UNLOCK(font->lock);
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