/* Host check: both on-demand TTF loaders must render byte-identical glyphs to
   the whole-file loader — including after the glyph window has been recycled
   many times over (phase 2 uses far more distinct CJK glyphs than fit).
 *
 * Build (from the repo root):
 *   gcc -O2 -std=gnu17 -Ilib/include -Iablib -Iablib/freestndchdrs/x86_64/include \
 *       -include res/scripts/test/ttf_ondemand/ttfprelude.h \
 *       -c lib/base/font/ttf.c -o /tmp/ttf_host.o
 *   gcc -O2 -std=gnu17 -Ilib/include -Iablib \
 *       -c res/scripts/test/ttf_ondemand/ttftest.c -o /tmp/ttftest.o
 *   gcc -O2 -o /tmp/ttftest.exe /tmp/ttftest.o /tmp/ttf_host.o
 *   ./ttftest.exe            (run from the repo root: uses a relative path)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <base/font/ttf/ttf.h>

static const char* kPath = "res/font/SourceHanSerifTC_Medium.ttf";

static const char* TEXT = "Skyline 天际线 OS ABC 123 测试";

static size_t bmp_bytes(const TTF_Bitmap* b) {
    return (size_t)(b->width > 0 ? b->width : 0) *
           (size_t)(b->height > 0 ? b->height : 0);
}

static int render_with(uint8_t (*loader)(TTF_Font**, const char*, int32_t, int32_t),
                       const char* what, const char* text, TTF_Bitmap* out) {
    TTF_Font* f = NULL;
    uint8_t rc = loader(&f, kPath, 20, 256);
    printf("[%-11s] rc=%u font=%p", what, rc, (void*)f);
    if (rc != 0 || !f) { printf("\n"); return 1; }
    if (!TTF_RenderTextToBuffer(f, text, out)) {
        printf("  render FAILED\n");
        return 1;
    }
    unsigned long cap = 0, res = 0, fet = 0, byt = 0;
    TTF_GetGlyfStats(f, &cap, &res, &fet, &byt);
    printf("  bitmap %dx%d (%zu bytes)", out->width, out->height, bmp_bytes(out));
    if (cap) printf("  glyfwin cap=%lu resident=%lu fetches=%lu read=%lu B", cap, res, fet, byt);
    printf("\n");
    TTF_DestroyFont(f);
    return 0;
}

static size_t compare(const char* tag, const TTF_Bitmap* a, const TTF_Bitmap* b) {
    if (a->width != b->width || a->height != b->height) {
        printf("  %s: SIZE MISMATCH %dx%d vs %dx%d\n",
               tag, a->width, a->height, b->width, b->height);
        return 1;
    }
    size_t n = bmp_bytes(a), diff = 0;
    for (size_t i = 0; i < n; i++) if (a->pixels[i] != b->pixels[i]) diff++;
    printf("  %s: identical=%s differing=%zu / %zu\n",
           tag, diff ? "NO" : "YES", diff, n);
    return diff;
}

int main(void) {
    /* ---- phase 1: short mixed string, all three loaders ---- */
    printf("== phase 1: \"%s\" ==\n", TEXT);
    TTF_Bitmap w, t, g;
    memset(&w, 0, sizeof(w)); memset(&t, 0, sizeof(t)); memset(&g, 0, sizeof(g));
    if (render_with(TTF_ReadFont,         "whole-file", TEXT, &w)) return 1;
    if (render_with(TTF_ReadFontLazy,     "table-lazy", TEXT, &t)) return 1;
    if (render_with(TTF_ReadFontGlyfLazy, "glyph-lazy", TEXT, &g)) return 1;

    size_t bad = 0;
    bad += compare("table-lazy vs whole-file", &w, &t);
    bad += compare("glyph-lazy vs whole-file", &w, &g);

    /* ---- phase 2: far more distinct glyphs than the window can hold,
            so the window must be recycled many times ---- */
    enum { NCHARS = 2000 };
    char* big = (char*)malloc((size_t)NCHARS * 4 + 1);
    if (!big) { printf("oom\n"); return 1; }
    size_t o = 0;
    for (int i = 0; i < NCHARS; i++) {
        unsigned cp = 0x4E00u + (unsigned)i;          /* CJK unified ideographs */
        big[o++] = (char)(0xE0 | (cp >> 12));
        big[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        big[o++] = (char)(0x80 | (cp & 0x3F));
        if ((i % 40) == 39) big[o++] = '\n';
    }
    big[o] = 0;

    printf("\n== phase 2: %d distinct CJK glyphs (window recycling) ==\n", NCHARS);
    TTF_Bitmap w2, g2;
    memset(&w2, 0, sizeof(w2)); memset(&g2, 0, sizeof(g2));
    if (render_with(TTF_ReadFont,         "whole-file", big, &w2)) { free(big); return 1; }
    if (render_with(TTF_ReadFontGlyfLazy, "glyph-lazy", big, &g2)) { free(big); return 1; }
    bad += compare("glyph-lazy vs whole-file", &w2, &g2);

    free(big);
    TTF_FreeBitmap(&w); TTF_FreeBitmap(&t); TTF_FreeBitmap(&g);
    TTF_FreeBitmap(&w2); TTF_FreeBitmap(&g2);
    printf("\nRESULT: %s\n", bad ? "MISMATCH" : "byte-identical");
    return bad ? 2 : 0;
}
