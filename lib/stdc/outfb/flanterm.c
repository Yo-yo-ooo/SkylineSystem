#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <graphic/fb.h>
#include <base/font/ttf/ttf.h>
#include <graphic/flanterm.h>

#ifndef likely
#define likely(x)   __builtin_expect(!!(x), 1)
#endif
#ifndef unlikely
#define unlikely(x) __builtin_expect(!!(x), 0)
#endif

#pragma region "Color Table"
static const uint32_t col256[] = {
    0x000000, 0x00005f, 0x000087, 0x0000af, 0x0000d7, 0x0000ff, 0x005f00, 0x005f5f,
    0x005f87, 0x005faf, 0x005fd7, 0x005fff, 0x008700, 0x00875f, 0x008787, 0x0087af,
    0x0087d7, 0x0087ff, 0x00af00, 0x00af5f, 0x00af87, 0x00afaf, 0x00afd7, 0x00afff,
    0x00d700, 0x00d75f, 0x00d787, 0x00d7af, 0x00d7d7, 0x00d7ff, 0x00ff00, 0x00ff5f,
    0x00ff87, 0x00ffaf, 0x00ffd7, 0x00ffff, 0x5f0000, 0x5f005f, 0x5f0087, 0x5f00af,
    0x5f00d7, 0x5f00ff, 0x5f5f00, 0x5f5f5f, 0x5f5f87, 0x5f5faf, 0x5f5fd7, 0x5f5fff,
    0x5f8700, 0x5f875f, 0x5f8787, 0x5f87af, 0x5f87d7, 0x5f87ff, 0x5faf00, 0x5faf5f,
    0x5faf87, 0x5fafaf, 0x5fafd7, 0x5fafff, 0x5fd700, 0x5fd75f, 0x5fd787, 0x5fd7af,
    0x5fd7d7, 0x5fd7ff, 0x5fff00, 0x5fff5f, 0x5fff87, 0x5fffaf, 0x5fffd7, 0x5fffff,
    0x870000, 0x87005f, 0x870087, 0x8700af, 0x8700d7, 0x8700ff, 0x875f00, 0x875f5f,
    0x875f87, 0x875faf, 0x875fd7, 0x875fff, 0x878700, 0x87875f, 0x878787, 0x8787af,
    0x8787d7, 0x8787ff, 0x87af00, 0x87af5f, 0x87af87, 0x87afaf, 0x87afd7, 0x87afff,
    0x87d700, 0x87d75f, 0x87d787, 0x87d7af, 0x87d7d7, 0x87d7ff, 0x87ff00, 0x87ff5f,
    0x87ff87, 0x87ffaf, 0x87ffd7, 0x87ffff, 0xaf0000, 0xaf005f, 0xaf0087, 0xaf00af,
    0xaf00d7, 0xaf00ff, 0xaf5f00, 0xaf5f5f, 0xaf5f87, 0xaf5faf, 0xaf5fd7, 0xaf5fff,
    0xaf8700, 0xaf875f, 0xaf8787, 0xaf87af, 0xaf87d7, 0xaf87ff, 0xafaf00, 0xafaf5f,
    0xafaf87, 0xafafaf, 0xafafd7, 0xafafff, 0xafd700, 0xafd75f, 0xafd787, 0xafd7af,
    0xafd7d7, 0xafd7ff, 0xafff00, 0xafff5f, 0xafff87, 0xafffaf, 0xafffd7, 0xafffff,
    0xd70000, 0xd7005f, 0xd70087, 0xd700af, 0xd700d7, 0xd700ff, 0xd75f00, 0xd75f5f,
    0xd75f87, 0xd75faf, 0xd75fd7, 0xd75fff, 0xd78700, 0xd7875f, 0xd78787, 0xd787af,
    0xd787d7, 0xd787ff, 0xd7af00, 0xd7af5f, 0xd7af87, 0xd7afaf, 0xd7afd7, 0xd7afff,
    0xd7d700, 0xd7d75f, 0xd7d787, 0xd7d7af, 0xd7d7d7, 0xd7d7ff, 0xd7ff00, 0xd7ff5f,
    0xd7ff87, 0xd7ffaf, 0xd7ffd7, 0xd7ffff, 0xff0000, 0xff005f, 0xff0087, 0xff00af,
    0xff00d7, 0xff00ff, 0xff5f00, 0xff5f5f, 0xff5f87, 0xff5faf, 0xff5fd7, 0xff5fff,
    0xff8700, 0xff875f, 0xff8787, 0xff87af, 0xff87d7, 0xff87ff, 0xffaf00, 0xffaf5f,
    0xffaf87, 0xffafaf, 0xffafd7, 0xffafff, 0xffd700, 0xffd75f, 0xffd787, 0xffd7af,
    0xffd7d7, 0xffd7ff, 0xffff00, 0xffff5f, 0xffff87, 0xffffaf, 0xffffd7, 0xffffff,
    0x080808, 0x121212, 0x1c1c1c, 0x262626, 0x303030, 0x3a3a3a, 0x444444, 0x4e4e4e,
    0x585858, 0x626262, 0x6c6c6c, 0x767676, 0x808080, 0x8a8a8a, 0x949494, 0x9e9e9e,
    0xa8a8a8, 0xb2b2b2, 0xbcbcbc, 0xc6c6c6, 0xd0d0d0, 0xdadada, 0xe4e4e4, 0xeeeeee
};
#pragma endregion

#define CHARSET_DEFAULT 0
#define CHARSET_DEC_SPECIAL 1

#pragma region "Helpers"
static inline uint32_t convert_colour(uint32_t rgb) {
    return rgb & 0x00FFFFFF;
}

// 极其优化的 draw_rect，使用 memset 和 memcpy 代替逐像素填充
static void draw_rect(FrameBuffer *fb, int x, int y, int w, int h, uint32_t color) {
    if (unlikely(!fb || !fb->BaseAddress)) return;
    uint32_t *fb_ptr = (uint32_t *)fb->BaseAddress;
    int fb_w = (int)fb->Width;
    int fb_h = (int)fb->Height;
    int pitch = (int)fb->PixelsPerScanLine;

    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > fb_w) w = fb_w - x;
    if (y + h > fb_h) h = fb_h - y;
    if (unlikely(w <= 0 || h <= 0)) return;

    uint32_t *row_start = fb_ptr + (size_t)y * pitch + x;
    size_t row_bytes = (size_t)w * 4;

    uint8_t c0 = color & 0xFF;
    uint8_t c1 = (color >> 8) & 0xFF;
    uint8_t c2 = (color >> 16) & 0xFF;
    uint8_t c3 = (color >> 24) & 0xFF;

    // 如果 4 个字节相同，直接用 memset (SIMD 加速)
    if (c0 == c1 && c1 == c2 && c2 == c3) {
        memset(row_start, c0, row_bytes);
    } else {
        // 填充第一行
        uint32_t *p = row_start;
        uint32_t *end = row_start + w;
        while (p < end) *p++ = color;
    }

    // 使用 memcpy 将第一行数据克隆到后续所有行 (SIMD 加速)
    for (int py = 1; py < h; py++) {
        memcpy(row_start + (size_t)py * pitch, row_start, row_bytes);
    }
}

// 用 SIMD 优化的指数倍增法填充内存
static void fill_grid_fast(struct flanterm_ttf_char *grid, struct flanterm_ttf_char *val, size_t count) {
    if (count == 0) return;
    grid[0] = *val;
    size_t filled = 1;
    while (filled < count) {
        size_t to_copy = filled < (count - filled) ? filled : (count - filled);
        memcpy(&grid[filled], &grid[0], to_copy * sizeof(struct flanterm_ttf_char));
        filled += to_copy;
    }
}
#pragma endregion

#pragma region "Internal Terminal Ops"
static void term_save_state(struct flanterm_context *ctx) {
    ctx->saved_text_fg = ctx->text_fg;
    ctx->saved_text_bg = ctx->text_bg;
    ctx->saved_state_cursor_x = ctx->cursor_x;
    ctx->saved_state_cursor_y = ctx->cursor_y;
}

static void term_restore_state(struct flanterm_context *ctx) {
    ctx->text_fg = ctx->saved_text_fg;
    ctx->text_bg = ctx->saved_text_bg;
    ctx->cursor_x = ctx->saved_state_cursor_x;
    ctx->cursor_y = ctx->saved_state_cursor_y;
}

static void term_swap_palette(struct flanterm_context *ctx) {
    uint32_t tmp = ctx->text_bg;
    ctx->text_bg = ctx->text_fg;
    ctx->text_fg = tmp;
}

static void term_scroll(struct flanterm_context *ctx) {
    size_t top = ctx->scroll_top_margin;
    size_t bottom = ctx->scroll_bottom_margin > ctx->rows ? ctx->rows : ctx->scroll_bottom_margin;
    if (bottom <= top) return;
    
    size_t rows_to_move = bottom - top - 1;
    if (likely(rows_to_move > 0)) {
        // 使用 memmove 一次性滚动整个块
        memmove(&ctx->grid[top * ctx->cols], &ctx->grid[(top + 1) * ctx->cols], rows_to_move * ctx->cols * sizeof(struct flanterm_ttf_char));
    }
    
    struct flanterm_ttf_char empty = {' ', ctx->text_fg, ctx->text_bg};
    // 使用快速填充清空最后一行
    fill_grid_fast(&ctx->grid[(bottom - 1) * ctx->cols], &empty, ctx->cols);
    
    // 标记脏行
    memset(&ctx->dirty_rows[top], 1, (bottom - top) * sizeof(bool));
}

static void term_revscroll(struct flanterm_context *ctx) {
    size_t top = ctx->scroll_top_margin;
    size_t bottom = ctx->scroll_bottom_margin > ctx->rows ? ctx->rows : ctx->scroll_bottom_margin;
    if (bottom <= top) return;
    
    size_t rows_to_move = bottom - top - 1;
    if (likely(rows_to_move > 0)) {
        memmove(&ctx->grid[(top + 1) * ctx->cols], &ctx->grid[top * ctx->cols], rows_to_move * ctx->cols * sizeof(struct flanterm_ttf_char));
    }
    
    struct flanterm_ttf_char empty = {' ', ctx->text_fg, ctx->text_bg};
    fill_grid_fast(&ctx->grid[top * ctx->cols], &empty, ctx->cols);
    
    memset(&ctx->dirty_rows[top], 1, (bottom - top) * sizeof(bool));
}

static void term_clear(struct flanterm_context *ctx, bool move) {
    struct flanterm_ttf_char empty = {' ', ctx->text_fg, ctx->text_bg};
    // 极速填充整个 grid
    fill_grid_fast(ctx->grid, &empty, ctx->rows * ctx->cols);
    
    // memset 标记所有行脏
    memset(ctx->dirty_rows, 1, ctx->rows * sizeof(bool));
    
    if (likely(move)) {
        ctx->cursor_x = 0;
        ctx->cursor_y = 0;
    }
}

static void term_set_cursor_pos(struct flanterm_context *ctx, size_t x, size_t y) {
    if (unlikely(x >= ctx->cols)) x = ctx->cols - 1;
    if (unlikely(y >= ctx->rows)) y = ctx->rows - 1;
    ctx->cursor_x = x;
    ctx->cursor_y = y;
}

static void term_get_cursor_pos(struct flanterm_context *ctx, size_t *x, size_t *y) {
    *x = ctx->cursor_x;
    *y = ctx->cursor_y;
}

static void term_move_character(struct flanterm_context *ctx, size_t new_x, size_t new_y, size_t old_x, size_t old_y) {
    if (unlikely(old_x >= ctx->cols || old_y >= ctx->rows || new_x >= ctx->cols || new_y >= ctx->rows)) return;
    ctx->grid[new_y * ctx->cols + new_x] = ctx->grid[old_y * ctx->cols + old_x];
    ctx->dirty_rows[new_y] = true;
    ctx->dirty_rows[old_y] = true;
}

static void term_set_text_fg(struct flanterm_context *ctx, size_t fg) { ctx->text_fg = ctx->ansi_colours[fg]; }
static void term_set_text_bg(struct flanterm_context *ctx, size_t bg) { ctx->text_bg = ctx->ansi_colours[bg]; }
static void term_set_text_fg_bright(struct flanterm_context *ctx, size_t fg) { ctx->text_fg = ctx->ansi_bright_colours[fg]; }
static void term_set_text_bg_bright(struct flanterm_context *ctx, size_t bg) { ctx->text_bg = ctx->ansi_bright_colours[bg]; }
static void term_set_text_fg_rgb(struct flanterm_context *ctx, uint32_t fg) { ctx->text_fg = convert_colour(fg); }
static void term_set_text_bg_rgb(struct flanterm_context *ctx, uint32_t bg) { ctx->text_bg = convert_colour(bg); }
static void term_set_text_fg_default(struct flanterm_context *ctx) { ctx->text_fg = ctx->default_fg; }
static void term_set_text_bg_default(struct flanterm_context *ctx) { ctx->text_bg = 0xFFFFFFFF; }
static void term_set_text_fg_default_bright(struct flanterm_context *ctx) { ctx->text_fg = 0x00AAAAAA; }
static void term_set_text_bg_default_bright(struct flanterm_context *ctx) { ctx->text_bg = 0x00555555; }

static void term_raw_putchar(struct flanterm_context *ctx, uint8_t c) {
    if (unlikely(ctx->cursor_x >= ctx->cols && (ctx->cursor_y < ctx->scroll_bottom_margin - 1 || ctx->scroll_enabled))) {
        ctx->cursor_x = 0;
        ctx->cursor_y++;
        if (unlikely(ctx->cursor_y == ctx->scroll_bottom_margin)) {
            ctx->cursor_y--;
            term_scroll(ctx);
        }
    }
    struct flanterm_ttf_char *ch = &ctx->grid[ctx->cursor_y * ctx->cols + ctx->cursor_x];
    ch->c = c;
    ch->fg = ctx->text_fg;
    ch->bg = ctx->text_bg;
    ctx->dirty_rows[ctx->cursor_y] = true;
    ctx->cursor_x++;
}
#pragma endregion

#pragma region "Rendering"
static void term_double_buffer_flush(struct flanterm_context *ctx) {
    if (likely(ctx->cursor_enabled)) {
        if (unlikely(ctx->old_cursor_y < ctx->rows)) ctx->dirty_rows[ctx->old_cursor_y] = true;
        if (unlikely(ctx->cursor_y < ctx->rows)) ctx->dirty_rows[ctx->cursor_y] = true;
    }

    for (size_t y = 0; y < ctx->rows; y++) {
        if (unlikely(!ctx->dirty_rows[y])) continue;
        
        int y_pos = ctx->margin + y * ctx->char_height;
        
        // 1. 绘制背景 (合并相同颜色的连续块，极大降低 draw_rect 调用开销)
        size_t x = 0;
        while (x < ctx->cols) {
            uint32_t bg = ctx->grid[y * ctx->cols + x].bg;
            if (unlikely(bg == 0xFFFFFFFF)) bg = 0x000000; 
            
            size_t run_len = 1;
            // 使用 memcmp 快速比较结构体中的 bg 字段
            while (x + run_len < ctx->cols && ctx->grid[y * ctx->cols + x + run_len].bg == bg) {
                run_len++;
            }
            
            draw_rect(ctx->fb, ctx->margin + x * ctx->char_width, y_pos, run_len * ctx->char_width, ctx->char_height, bg);
            x += run_len;
        }
        
        // 2. 渲染前景文字 (按相同颜色分段)
        char utf8_buf[1024];
        int utf8_len = 0;
        uint32_t curr_fg = ctx->grid[y * ctx->cols].fg;
        size_t x_start = 0;
        
        for (size_t x = 0; x < ctx->cols; x++) {
            struct flanterm_ttf_char *ch = &ctx->grid[y * ctx->cols + x];
            
            if (unlikely(ch->fg != curr_fg || utf8_len > 1000)) {
                if (likely(utf8_len > 0)) {
                    utf8_buf[utf8_len] = '\0';
                    TTF_DrawText(ctx->fb, ctx->font, ctx->margin + x_start * ctx->char_width, y_pos, utf8_buf, curr_fg);
                    utf8_len = 0;
                }
                curr_fg = ch->fg;
                x_start = x;
            }
            
            utf8_buf[utf8_len++] = ch->c;
        }
        
        if (likely(utf8_len > 0)) {
            utf8_buf[utf8_len] = '\0';
            TTF_DrawText(ctx->fb, ctx->font, ctx->margin + x_start * ctx->char_width, y_pos, utf8_buf, curr_fg);
        }
        
        ctx->dirty_rows[y] = false;
    }
    
    // 3. 绘制光标
    if (likely(ctx->cursor_enabled)) {
        int cx = ctx->margin + ctx->cursor_x * ctx->char_width;
        int cy = ctx->margin + ctx->cursor_y * ctx->char_height + ctx->char_height - 4;
        draw_rect(ctx->fb, cx, cy, ctx->char_width, 3, ctx->text_fg);
    }
    
    ctx->old_cursor_x = ctx->cursor_x;
    ctx->old_cursor_y = ctx->cursor_y;
}

static void term_full_refresh(struct flanterm_context *ctx) {
    draw_rect(ctx->fb, 0, 0, ctx->screen_width, ctx->screen_height, 0x00000000);
    memset(ctx->dirty_rows, 1, ctx->rows * sizeof(bool));
    term_double_buffer_flush(ctx);
}
#pragma endregion

#pragma region "ANSI Parser"
void flanterm_context_reinit(struct flanterm_context *ctx) {
    ctx->tab_size = 8;
    ctx->autoflush = true;
    ctx->cursor_enabled = true;
    ctx->scroll_enabled = true;
    ctx->control_sequence = false;
    ctx->escape = false;
    ctx->osc = false;
    ctx->osc_escape = false;
    ctx->rrr = false;
    ctx->discard_next = false;
    ctx->bold = false;
    ctx->bg_bold = false;
    ctx->reverse_video = false;
    ctx->dec_private = false;
    ctx->insert_mode = false;
    ctx->unicode_remaining = 0;
    ctx->g_select = 0;
    ctx->charsets[0] = CHARSET_DEFAULT;
    ctx->charsets[1] = CHARSET_DEC_SPECIAL;
    ctx->current_charset = 0;
    ctx->escape_offset = 0;
    ctx->esc_values_i = 0;
    ctx->saved_cursor_x = 0;
    ctx->saved_cursor_y = 0;
    ctx->current_primary = (size_t)-1;
    ctx->current_bg = (size_t)-1;
    ctx->scroll_top_margin = 0;
    ctx->scroll_bottom_margin = ctx->rows;
    ctx->oob_output = FLANTERM_OOB_OUTPUT_ONLCR;
}

static void flanterm_putchar(struct flanterm_context *ctx, uint8_t c);

void flanterm_write(struct flanterm_context *ctx, const char *buf, size_t count) {
    for (size_t i = 0; i < count; i++) {
        flanterm_putchar(ctx, buf[i]);
    }
    if (likely(ctx->autoflush)) {
        term_double_buffer_flush(ctx);
    }
}

static void sgr(struct flanterm_context *ctx) {
    size_t i = 0;
    if (unlikely(!ctx->esc_values_i)) goto def;

    for (; i < ctx->esc_values_i; i++) {
        size_t offset;
        if (unlikely(ctx->esc_values[i] == 0)) {
def:
            if (unlikely(ctx->reverse_video)) { ctx->reverse_video = false; term_swap_palette(ctx); }
            ctx->bold = false; ctx->bg_bold = false;
            ctx->current_primary = (size_t)-1; ctx->current_bg = (size_t)-1;
            term_set_text_bg_default(ctx); term_set_text_fg_default(ctx);
            continue;
        } else if (unlikely(ctx->esc_values[i] == 1)) {
            ctx->bold = true;
            if (likely(ctx->current_primary != (size_t)-1)) {
                if (likely(!ctx->reverse_video)) term_set_text_fg_bright(ctx, ctx->current_primary);
                else term_set_text_bg_bright(ctx, ctx->current_primary);
            } else {
                if (likely(!ctx->reverse_video)) term_set_text_fg_default_bright(ctx);
                else term_set_text_bg_default_bright(ctx);
            }
            continue;
        } else if (unlikely(ctx->esc_values[i] == 5)) {
            ctx->bg_bold = true;
            if (likely(ctx->current_bg != (size_t)-1)) {
                if (likely(!ctx->reverse_video)) term_set_text_bg_bright(ctx, ctx->current_bg);
                else term_set_text_fg_bright(ctx, ctx->current_bg);
            } else {
                if (likely(!ctx->reverse_video)) term_set_text_bg_default_bright(ctx);
                else term_set_text_fg_default_bright(ctx);
            }
            continue;
        } else if (unlikely(ctx->esc_values[i] == 22)) {
            ctx->bold = false;
            if (likely(ctx->current_primary != (size_t)-1)) {
                if (likely(!ctx->reverse_video)) term_set_text_fg(ctx, ctx->current_primary);
                else term_set_text_bg(ctx, ctx->current_primary);
            } else {
                if (likely(!ctx->reverse_video) ) term_set_text_fg_default(ctx);
                else term_set_text_bg_default(ctx);
            }
            continue;
        } else if (unlikely(ctx->esc_values[i] == 25)) {
            ctx->bg_bold = false;
            if (likely(ctx->current_bg != (size_t)-1)) {
                if (likely(!ctx->reverse_video)) term_set_text_bg(ctx, ctx->current_bg);
                else term_set_text_fg(ctx, ctx->current_bg);
            } else {
                if (likely(!ctx->reverse_video)) term_set_text_bg_default(ctx);
                else term_set_text_fg_default(ctx);
            }
            continue;
        } else if (likely(ctx->esc_values[i] >= 30 && ctx->esc_values[i] <= 37)) {
            offset = 30;
            ctx->current_primary = ctx->esc_values[i] - offset;
            if (unlikely(ctx->reverse_video)) goto set_bg;
set_fg:
            if ((ctx->bold && !ctx->reverse_video) || (ctx->bg_bold && ctx->reverse_video)) {
                term_set_text_fg_bright(ctx, ctx->esc_values[i] - offset);
            } else {
                term_set_text_fg(ctx, ctx->esc_values[i] - offset);
            }
            continue;
        } else if (likely(ctx->esc_values[i] >= 40 && ctx->esc_values[i] <= 47)) {
            offset = 40;
            ctx->current_bg = ctx->esc_values[i] - offset;
            if (unlikely(ctx->reverse_video)) goto set_fg;
set_bg:
            if ((ctx->bold && ctx->reverse_video) || (ctx->bg_bold && !ctx->reverse_video)) {
                term_set_text_bg_bright(ctx, ctx->esc_values[i] - offset);
            } else {
                term_set_text_bg(ctx, ctx->esc_values[i] - offset);
            }
            continue;
        } else if (likely(ctx->esc_values[i] >= 90 && ctx->esc_values[i] <= 97)) {
            offset = 90;
            ctx->current_primary = ctx->esc_values[i] - offset;
            if (unlikely(ctx->reverse_video)) goto set_bg_bright;
set_fg_bright:
            term_set_text_fg_bright(ctx, ctx->esc_values[i] - offset);
            continue;
        } else if (likely(ctx->esc_values[i] >= 100 && ctx->esc_values[i] <= 107)) {
            offset = 100;
            ctx->current_bg = ctx->esc_values[i] - offset;
            if (unlikely(ctx->reverse_video)) goto set_fg_bright;
set_bg_bright:
            term_set_text_bg_bright(ctx, ctx->esc_values[i] - offset);
            continue;
        } else if (unlikely(ctx->esc_values[i] == 39)) {
            ctx->current_primary = (size_t)-1;
            if (unlikely(ctx->reverse_video)) term_swap_palette(ctx);
            if (likely(!ctx->bold)) term_set_text_fg_default(ctx);
            else term_set_text_fg_default_bright(ctx);
            if (unlikely(ctx->reverse_video)) term_swap_palette(ctx);
            continue;
        } else if (unlikely(ctx->esc_values[i] == 49)) {
            ctx->current_bg = (size_t)-1;
            if (unlikely(ctx->reverse_video)) term_swap_palette(ctx);
            if (likely(!ctx->bg_bold)) term_set_text_bg_default(ctx);
            else term_set_text_bg_default_bright(ctx);
            if (unlikely(ctx->reverse_video)) term_swap_palette(ctx);
            continue;
        } else if (unlikely(ctx->esc_values[i] == 7)) {
            if (likely(!ctx->reverse_video)) { ctx->reverse_video = true; term_swap_palette(ctx); }
            continue;
        } else if (unlikely(ctx->esc_values[i] == 27)) {
            if (unlikely(ctx->reverse_video)) { ctx->reverse_video = false; term_swap_palette(ctx); }
            continue;
        } else if (unlikely(ctx->esc_values[i] == 38 || ctx->esc_values[i] == 48)) {
            bool fg = ctx->esc_values[i] == 38;
            i++;
            if (unlikely(i >= ctx->esc_values_i)) break;
            switch (ctx->esc_values[i]) {
                case 2: {
                    if (unlikely(i + 3 >= ctx->esc_values_i)) goto out;
                    uint32_t rgb_value = (ctx->esc_values[i + 1] << 16) | (ctx->esc_values[i + 2] << 8) | ctx->esc_values[i + 3];
                    i += 3;
                    (fg ? term_set_text_fg_rgb : term_set_text_bg_rgb)(ctx, rgb_value);
                    break;
                }
                case 5: {
                    if (unlikely(i + 1 >= ctx->esc_values_i)) goto out;
                    uint32_t col = ctx->esc_values[i + 1];
                    i++;
                    if (likely(col < 8)) (fg ? term_set_text_fg : term_set_text_bg)(ctx, col);
                    else if (likely(col < 16)) (fg ? term_set_text_fg_bright : term_set_text_bg_bright)(ctx, col - 8);
                    else if (likely(col < 256)) {
                        uint32_t rgb_value = col256[col - 16];
                        (fg ? term_set_text_fg_rgb : term_set_text_bg_rgb)(ctx, rgb_value);
                    }
                    break;
                }
                default: continue;
            }
        }
    }
out:;
}

static void dec_private_parse(struct flanterm_context *ctx, uint8_t c) {
    ctx->dec_private = false;
    if (unlikely(!ctx->esc_values_i)) return;
    bool set;
    switch (c) {
        case 'h': set = true; break;
        case 'l': set = false; break;
        default: return;
    }
    switch (ctx->esc_values[0]) {
        case 25:
            ctx->cursor_enabled = set;
            return;
    }
    if (likely(ctx->callback != NULL)) {
        ctx->callback(ctx, FLANTERM_CB_DEC, ctx->esc_values_i, (uintptr_t)ctx->esc_values, c);
    }
}

static void linux_private_parse(struct flanterm_context *ctx) {
    if (unlikely(!ctx->esc_values_i)) return;
    if (likely(ctx->callback != NULL)) {
        ctx->callback(ctx, FLANTERM_CB_LINUX, ctx->esc_values_i, (uintptr_t)ctx->esc_values, 0);
    }
}

static void mode_toggle(struct flanterm_context *ctx, uint8_t c) {
    if (unlikely(!ctx->esc_values_i)) return;
    bool set;
    switch (c) {
        case 'h': set = true; break;
        case 'l': set = false; break;
        default: return;
    }
    switch (ctx->esc_values[0]) {
        case 4:
            ctx->insert_mode = set; return;
    }
    if (likely(ctx->callback != NULL)) {
        ctx->callback(ctx, FLANTERM_CB_MODE, ctx->esc_values_i, (uintptr_t)ctx->esc_values, c);
    }
}

static void osc_parse(struct flanterm_context *ctx, uint8_t c) {
    if (unlikely(ctx->osc_escape && c == '\\')) goto cleanup;
    ctx->osc_escape = false;
    switch (c) {
        case 0x1b:
            ctx->osc_escape = true;
            return;
        case '\a':
        default:
            break;
    }
cleanup:
    ctx->osc_escape = false;
    ctx->osc = false;
    ctx->escape = false;
}

static void control_sequence_parse(struct flanterm_context *ctx, uint8_t c) {
    if (unlikely(ctx->escape_offset == 2)) {
        switch (c) {
            case '[':
                ctx->discard_next = true;
                goto cleanup;
            case '?':
                ctx->dec_private = true;
                return;
        }
    }

    if (likely(c >= '0' && c <= '9')) {
        if (unlikely(ctx->esc_values_i == FLANTERM_MAX_ESC_VALUES)) return;
        ctx->rrr = true;
        ctx->esc_values[ctx->esc_values_i] *= 10;
        ctx->esc_values[ctx->esc_values_i] += c - '0';
        return;
    }

    if (unlikely(ctx->rrr == true)) {
        ctx->esc_values_i++;
        ctx->rrr = false;
        if (c == ';') return;
    } else if (unlikely(c == ';')) {
        if (unlikely(ctx->esc_values_i == FLANTERM_MAX_ESC_VALUES)) return;
        ctx->esc_values[ctx->esc_values_i] = 0;
        ctx->esc_values_i++;
        return;
    }

    size_t esc_default;
    switch (c) {
        case 'J': case 'K': case 'q':
            esc_default = 0; break;
        default:
            esc_default = 1; break;
    }

    for (size_t i = ctx->esc_values_i; i < FLANTERM_MAX_ESC_VALUES; i++) {
        ctx->esc_values[i] = esc_default;
    }

    if (unlikely(ctx->dec_private == true)) {
        dec_private_parse(ctx, c);
        goto cleanup;
    }

    bool r = ctx->scroll_enabled;
    ctx->scroll_enabled = false;
    size_t x, y;
    term_get_cursor_pos(ctx, &x, &y);

    switch (c) {
        case 'F':
            x = 0;
            // FALLTHRU
        case 'A': {
            if (unlikely(ctx->esc_values[0] > y)) ctx->esc_values[0] = y;
            size_t orig_y = y;
            size_t dest_y = y - ctx->esc_values[0];
            bool will_be_in_scroll_region = false;
            if ((ctx->scroll_top_margin >= dest_y && ctx->scroll_top_margin <= orig_y)
             || (ctx->scroll_bottom_margin >= dest_y && ctx->scroll_bottom_margin <= orig_y)) {
                will_be_in_scroll_region = true;
            }
            if (unlikely(will_be_in_scroll_region && dest_y < ctx->scroll_top_margin)) {
                dest_y = ctx->scroll_top_margin;
            }
            term_set_cursor_pos(ctx, x, dest_y);
            break;
        }
        case 'E':
            x = 0;
            // FALLTHRU
        case 'e':
        case 'B': {
            if (unlikely(y + ctx->esc_values[0] > ctx->rows - 1)) ctx->esc_values[0] = (ctx->rows - 1) - y;
            size_t orig_y = y;
            size_t dest_y = y + ctx->esc_values[0];
            bool will_be_in_scroll_region = false;
            if ((ctx->scroll_top_margin >= orig_y && ctx->scroll_top_margin <= dest_y)
             || (ctx->scroll_bottom_margin >= orig_y && ctx->scroll_bottom_margin <= dest_y)) {
                will_be_in_scroll_region = true;
            }
            if (unlikely(will_be_in_scroll_region && dest_y >= ctx->scroll_bottom_margin)) {
                dest_y = ctx->scroll_bottom_margin - 1;
            }
            term_set_cursor_pos(ctx, x, dest_y);
            break;
        }
        case 'a':
        case 'C':
            if (unlikely(x + ctx->esc_values[0] > ctx->cols - 1)) ctx->esc_values[0] = (ctx->cols - 1) - x;
            term_set_cursor_pos(ctx, x + ctx->esc_values[0], y);
            break;
        case 'D':
            if (unlikely(ctx->esc_values[0] > x)) ctx->esc_values[0] = x;
            term_set_cursor_pos(ctx, x - ctx->esc_values[0], y);
            break;
        case 'c':
            if (likely(ctx->callback != NULL)) ctx->callback(ctx, FLANTERM_CB_PRIVATE_ID, 0, 0, 0);
            break;
        case 'd':
            ctx->esc_values[0] -= 1;
            if (unlikely(ctx->esc_values[0] >= ctx->rows)) ctx->esc_values[0] = ctx->rows - 1;
            term_set_cursor_pos(ctx, x, ctx->esc_values[0]);
            break;
        case 'G':
        case '`':
            ctx->esc_values[0] -= 1;
            if (unlikely(ctx->esc_values[0] >= ctx->cols)) ctx->esc_values[0] = ctx->cols - 1;
            term_set_cursor_pos(ctx, ctx->esc_values[0], y);
            break;
        case 'H':
        case 'f':
            if (likely(ctx->esc_values[0] != 0)) ctx->esc_values[0]--;
            if (likely(ctx->esc_values[1] != 0)) ctx->esc_values[1]--;
            if (unlikely(ctx->esc_values[1] >= ctx->cols)) ctx->esc_values[1] = ctx->cols - 1;
            if (unlikely(ctx->esc_values[0] >= ctx->rows)) ctx->esc_values[0] = ctx->rows - 1;
            term_set_cursor_pos(ctx, ctx->esc_values[1], ctx->esc_values[0]);
            break;
        case 'M': {
            size_t count = unlikely(ctx->esc_values[0] > ctx->rows) ? ctx->rows : ctx->esc_values[0];
            for (size_t i = 0; i < count; i++) term_scroll(ctx);
            break;
        }
        case 'L': {
            size_t old_scroll_top_margin = ctx->scroll_top_margin;
            ctx->scroll_top_margin = y;
            size_t count = unlikely(ctx->esc_values[0] > ctx->rows) ? ctx->rows : ctx->esc_values[0];
            for (size_t i = 0; i < count; i++) term_revscroll(ctx);
            ctx->scroll_top_margin = old_scroll_top_margin;
            break;
        }
        case 'n':
            switch (ctx->esc_values[0]) {
                case 5:
                    if (likely(ctx->callback != NULL)) ctx->callback(ctx, FLANTERM_CB_STATUS_REPORT, 0, 0, 0);
                    break;
                case 6:
                    if (likely(ctx->callback != NULL)) ctx->callback(ctx, FLANTERM_CB_POS_REPORT, x + 1, y + 1, 0);
                    break;
            }
            break;
        case 'q':
            if (likely(ctx->callback != NULL)) ctx->callback(ctx, FLANTERM_CB_KBD_LEDS, ctx->esc_values[0], 0, 0);
            break;
        case 'J':
            switch (ctx->esc_values[0]) {
                case 0: {
                    size_t rows_remaining = ctx->rows - (y + 1);
                    size_t cols_diff = ctx->cols - (x + 1);
                    size_t to_clear = rows_remaining * ctx->cols + cols_diff + 1;
                    for (size_t i = 0; i < to_clear; i++) term_raw_putchar(ctx, ' ');
                    term_set_cursor_pos(ctx, x, y);
                    break;
                }
                case 1: {
                    term_set_cursor_pos(ctx, 0, 0);
                    bool b = false;
                    for (size_t yc = 0; yc < ctx->rows; yc++) {
                        for (size_t xc = 0; xc < ctx->cols; xc++) {
                            term_raw_putchar(ctx, ' ');
                            if (unlikely(xc == x && yc == y)) {
                                term_set_cursor_pos(ctx, x, y);
                                b = true;
                                break;
                            }
                        }
                        if (unlikely(b == true)) break;
                    }
                    break;
                }
                case 2:
                case 3:
                    term_clear(ctx, false);
                    break;
            }
            break;
        case '@':
            for (size_t i = ctx->cols - 1; ; i--) {
                term_move_character(ctx, i + ctx->esc_values[0], y, i, y);
                term_set_cursor_pos(ctx, i, y);
                term_raw_putchar(ctx, ' ');
                if (unlikely(i == x)) break;
            }
            term_set_cursor_pos(ctx, x, y);
            break;
        case 'P':
            for (size_t i = x + ctx->esc_values[0]; i < ctx->cols; i++)
                term_move_character(ctx, i - ctx->esc_values[0], y, i, y);
            term_set_cursor_pos(ctx, ctx->cols - ctx->esc_values[0], y);
            // FALLTHRU
        case 'X': {
            size_t count = unlikely(ctx->esc_values[0] > ctx->cols) ? ctx->cols : ctx->esc_values[0];
            for (size_t i = 0; i < count; i++) term_raw_putchar(ctx, ' ');
            term_set_cursor_pos(ctx, x, y);
            break;
        }
        case 'm':
            sgr(ctx);
            break;
        case 's':
            term_get_cursor_pos(ctx, &ctx->saved_cursor_x, &ctx->saved_cursor_y);
            break;
        case 'u':
            term_set_cursor_pos(ctx, ctx->saved_cursor_x, ctx->saved_cursor_y);
            break;
        case 'K':
            switch (ctx->esc_values[0]) {
                case 0: {
                    for (size_t i = x; i < ctx->cols; i++) term_raw_putchar(ctx, ' ');
                    term_set_cursor_pos(ctx, x, y);
                    break;
                }
                case 1: {
                    term_set_cursor_pos(ctx, 0, y);
                    for (size_t i = 0; i < x; i++) term_raw_putchar(ctx, ' ');
                    break;
                }
                case 2: {
                    term_set_cursor_pos(ctx, 0, y);
                    for (size_t i = 0; i < ctx->cols; i++) term_raw_putchar(ctx, ' ');
                    term_set_cursor_pos(ctx, x, y);
                    break;
                }
            }
            break;
        case 'r':
            if (unlikely(ctx->esc_values[0] == 0)) ctx->esc_values[0] = 1;
            if (unlikely(ctx->esc_values[1] == 0)) ctx->esc_values[1] = 1;
            ctx->scroll_top_margin = 0;
            ctx->scroll_bottom_margin = ctx->rows;
            if (likely(ctx->esc_values_i > 0)) ctx->scroll_top_margin = ctx->esc_values[0] - 1;
            if (likely(ctx->esc_values_i > 1)) ctx->scroll_bottom_margin = ctx->esc_values[1];
            if (unlikely(ctx->scroll_top_margin >= ctx->rows
             || ctx->scroll_bottom_margin > ctx->rows
             || ctx->scroll_top_margin >= (ctx->scroll_bottom_margin - 1))) {
                ctx->scroll_top_margin = 0;
                ctx->scroll_bottom_margin = ctx->rows;
            }
            term_set_cursor_pos(ctx, 0, 0);
            break;
        case 'l':
        case 'h':
            mode_toggle(ctx, c);
            break;
        case ']':
            linux_private_parse(ctx);
            break;
    }

    ctx->scroll_enabled = r;

cleanup:
    ctx->control_sequence = false;
    ctx->escape = false;
}

static void restore_state(struct flanterm_context *ctx) {
    ctx->bold = ctx->saved_state_bold;
    ctx->bg_bold = ctx->saved_state_bg_bold;
    ctx->reverse_video = ctx->saved_state_reverse_video;
    ctx->current_charset = ctx->saved_state_current_charset;
    ctx->current_primary = ctx->saved_state_current_primary;
    ctx->current_bg = ctx->saved_state_current_bg;
    term_restore_state(ctx);
}

static void save_state(struct flanterm_context *ctx) {
    term_save_state(ctx);
    ctx->saved_state_bold = ctx->bold;
    ctx->saved_state_bg_bold = ctx->bg_bold;
    ctx->saved_state_reverse_video = ctx->reverse_video;
    ctx->saved_state_current_charset = ctx->current_charset;
    ctx->saved_state_current_primary = ctx->current_primary;
    ctx->saved_state_current_bg = ctx->current_bg;
}

static void escape_parse(struct flanterm_context *ctx, uint8_t c) {
    ctx->escape_offset++;
    if (unlikely(ctx->osc == true)) {
        osc_parse(ctx, c);
        return;
    }
    if (unlikely(ctx->control_sequence == true)) {
        control_sequence_parse(ctx, c);
        return;
    }
    size_t x, y;
    term_get_cursor_pos(ctx, &x, &y);
    switch (c) {
        case ']':
            ctx->osc_escape = false;
            ctx->osc = true;
            return;
        case '[':
            for (size_t i = 0; i < FLANTERM_MAX_ESC_VALUES; i++) ctx->esc_values[i] = 0;
            ctx->esc_values_i = 0;
            ctx->rrr = false;
            ctx->control_sequence = true;
            return;
        case '7':
            save_state(ctx);
            break;
        case '8':
            restore_state(ctx);
            break;
        case 'c':
            flanterm_context_reinit(ctx);
            term_clear(ctx, true);
            break;
        case 'D':
            if (unlikely(y == ctx->scroll_bottom_margin - 1)) {
                term_scroll(ctx);
                term_set_cursor_pos(ctx, x, y);
            } else {
                term_set_cursor_pos(ctx, x, y + 1);
            }
            break;
        case 'E':
            if (unlikely(y == ctx->scroll_bottom_margin - 1)) {
                term_scroll(ctx);
                term_set_cursor_pos(ctx, 0, y);
            } else {
                term_set_cursor_pos(ctx, 0, y + 1);
            }
            break;
        case 'M':
            if (unlikely(y == ctx->scroll_top_margin)) {
                term_revscroll(ctx);
                term_set_cursor_pos(ctx, 0, y);
            } else {
                term_set_cursor_pos(ctx, 0, y - 1);
            }
            break;
        case 'Z':
            if (likely(ctx->callback != NULL)) ctx->callback(ctx, FLANTERM_CB_PRIVATE_ID, 0, 0, 0);
            break;
        case '(':
        case ')':
            ctx->g_select = c - '\'';
            break;
    }
    ctx->escape = false;
}

static bool dec_special_print(struct flanterm_context *ctx, uint8_t c) {
#define FLANTERM_DEC_SPCL_PRN(C) term_raw_putchar(ctx, (C)); return true;
    switch (c) {
        case '`': FLANTERM_DEC_SPCL_PRN(0x04)
        case '0': FLANTERM_DEC_SPCL_PRN(0xdb)
        case '-': FLANTERM_DEC_SPCL_PRN(0x18)
        case ',': FLANTERM_DEC_SPCL_PRN(0x1b)
        case '.': FLANTERM_DEC_SPCL_PRN(0x19)
        case 'a': FLANTERM_DEC_SPCL_PRN(0xb1)
        case 'f': FLANTERM_DEC_SPCL_PRN(0xf8)
        case 'g': FLANTERM_DEC_SPCL_PRN(0xf1)
        case 'h': FLANTERM_DEC_SPCL_PRN(0xb0)
        case 'j': FLANTERM_DEC_SPCL_PRN(0xd9)
        case 'k': FLANTERM_DEC_SPCL_PRN(0xbf)
        case 'l': FLANTERM_DEC_SPCL_PRN(0xda)
        case 'm': FLANTERM_DEC_SPCL_PRN(0xc0)
        case 'n': FLANTERM_DEC_SPCL_PRN(0xc5)
        case 'q': FLANTERM_DEC_SPCL_PRN(0xc4)
        case 's': FLANTERM_DEC_SPCL_PRN(0x5f)
        case 't': FLANTERM_DEC_SPCL_PRN(0xc3)
        case 'u': FLANTERM_DEC_SPCL_PRN(0xb4)
        case 'v': FLANTERM_DEC_SPCL_PRN(0xc1)
        case 'w': FLANTERM_DEC_SPCL_PRN(0xc2)
        case 'x': FLANTERM_DEC_SPCL_PRN(0xb3)
        case 'y': FLANTERM_DEC_SPCL_PRN(0xf3)
        case 'z': FLANTERM_DEC_SPCL_PRN(0xf2)
        case '~': FLANTERM_DEC_SPCL_PRN(0xfa)
        case '_': FLANTERM_DEC_SPCL_PRN(0xff)
        case '+': FLANTERM_DEC_SPCL_PRN(0x1a)
        case '{': FLANTERM_DEC_SPCL_PRN(0xe3)
        case '}': FLANTERM_DEC_SPCL_PRN(0x9c)
    }
#undef FLANTERM_DEC_SPCL_PRN
    return false;
}

struct interval {
    uint32_t first;
    uint32_t last;
};

static int32_t bisearch(uint32_t ucs, const struct interval *table, int32_t max) {
  int32_t min = 0;
  int32_t mid;

  if (unlikely(ucs < table[0].first || ucs > table[max].last))
    return 0;
  while (likely(max >= min)) {
    mid = (min + max) / 2;
    if (ucs > table[mid].last)
      min = mid + 1;
    else if (ucs < table[mid].first)
      max = mid - 1;
    else
      return 1;
  }

  return 0;
}

int32_t mk_wcwidth(uint32_t ucs) {
  static const struct interval combining[] = {
    { 0x0300, 0x036F }, { 0x0483, 0x0486 }, { 0x0488, 0x0489 },
    { 0x0591, 0x05BD }, { 0x05BF, 0x05BF }, { 0x05C1, 0x05C2 },
    { 0x05C4, 0x05C5 }, { 0x05C7, 0x05C7 }, { 0x0600, 0x0603 },
    { 0x0610, 0x0615 }, { 0x064B, 0x065E }, { 0x0670, 0x0670 },
    { 0x06D6, 0x06E4 }, { 0x06E7, 0x06E8 }, { 0x06EA, 0x06ED },
    { 0x070F, 0x070F }, { 0x0711, 0x0711 }, { 0x0730, 0x074A },
    { 0x07A6, 0x07B0 }, { 0x07EB, 0x07F3 }, { 0x0901, 0x0902 },
    { 0x093C, 0x093C }, { 0x0941, 0x0948 }, { 0x094D, 0x094D },
    { 0x0951, 0x0954 }, { 0x0962, 0x0963 }, { 0x0981, 0x0981 },
    { 0x09BC, 0x09BC }, { 0x09C1, 0x09C4 }, { 0x09CD, 0x09CD },
    { 0x09E2, 0x09E3 }, { 0x0A01, 0x0A02 }, { 0x0A3C, 0x0A3C },
    { 0x0A41, 0x0A42 }, { 0x0A47, 0x0A48 }, { 0x0A4B, 0x0A4D },
    { 0x0A70, 0x0A71 }, { 0x0A81, 0x0A82 }, { 0x0ABC, 0x0ABC },
    { 0x0AC1, 0x0AC5 }, { 0x0AC7, 0x0AC8 }, { 0x0ACD, 0x0ACD },
    { 0x0AE2, 0x0AE3 }, { 0x0B01, 0x0B01 }, { 0x0B3C, 0x0B3C },
    { 0x0B3F, 0x0B3F }, { 0x0B41, 0x0B43 }, { 0x0B4D, 0x0B4D },
    { 0x0B56, 0x0B56 }, { 0x0B82, 0x0B82 }, { 0x0BC0, 0x0BC0 },
    { 0x0BCD, 0x0BCD }, { 0x0C3E, 0x0C40 }, { 0x0C46, 0x0C48 },
    { 0x0C4A, 0x0C4D }, { 0x0C55, 0x0C56 }, { 0x0CBC, 0x0CBC },
    { 0x0CBF, 0x0CBF }, { 0x0CC6, 0x0CC6 }, { 0x0CCC, 0x0CCD },
    { 0x0CE2, 0x0CE3 }, { 0x0D41, 0x0D43 }, { 0x0D4D, 0x0D4D },
    { 0x0DCA, 0x0DCA }, { 0x0DD2, 0x0DD4 }, { 0x0DD6, 0x0DD6 },
    { 0x0E31, 0x0E31 }, { 0x0E34, 0x0E3A }, { 0x0E47, 0x0E4E },
    { 0x0EB1, 0x0EB1 }, { 0x0EB4, 0x0EB9 }, { 0x0EBB, 0x0EBC },
    { 0x0EC8, 0x0ECD }, { 0x0F18, 0x0F19 }, { 0x0F35, 0x0F35 },
    { 0x0F37, 0x0F37 }, { 0x0F39, 0x0F39 }, { 0x0F71, 0x0F7E },
    { 0x0F80, 0x0F84 }, { 0x0F86, 0x0F87 }, { 0x0F90, 0x0F97 },
    { 0x0F99, 0x0FBC }, { 0x0FC6, 0x0FC6 }, { 0x102D, 0x1030 },
    { 0x1032, 0x1032 }, { 0x1036, 0x1037 }, { 0x1039, 0x1039 },
    { 0x1058, 0x1059 }, { 0x1160, 0x11FF }, { 0x135F, 0x135F },
    { 0x1712, 0x1714 }, { 0x1732, 0x1734 }, { 0x1752, 0x1753 },
    { 0x1772, 0x1773 }, { 0x17B4, 0x17B5 }, { 0x17B7, 0x17BD },
    { 0x17C6, 0x17C6 }, { 0x17C9, 0x17D3 }, { 0x17DD, 0x17DD },
    { 0x180B, 0x180D }, { 0x18A9, 0x18A9 }, { 0x1920, 0x1922 },
    { 0x1927, 0x1928 }, { 0x1932, 0x1932 }, { 0x1939, 0x193B },
    { 0x1A17, 0x1A18 }, { 0x1B00, 0x1B03 }, { 0x1B34, 0x1B34 },
    { 0x1B36, 0x1B3A }, { 0x1B3C, 0x1B3C }, { 0x1B42, 0x1B42 },
    { 0x1B6B, 0x1B73 }, { 0x1DC0, 0x1DCA }, { 0x1DFE, 0x1DFF },
    { 0x200B, 0x200F }, { 0x202A, 0x202E }, { 0x2060, 0x2063 },
    { 0x206A, 0x206F }, { 0x20D0, 0x20EF }, { 0x302A, 0x302F },
    { 0x3099, 0x309A }, { 0xA806, 0xA806 }, { 0xA80B, 0xA80B },
    { 0xA825, 0xA826 }, { 0xFB1E, 0xFB1E }, { 0xFE00, 0xFE0F },
    { 0xFE20, 0xFE23 }, { 0xFEFF, 0xFEFF }, { 0xFFF9, 0xFFFB },
    { 0x10A01, 0x10A03 }, { 0x10A05, 0x10A06 }, { 0x10A0C, 0x10A0F },
    { 0x10A38, 0x10A3A }, { 0x10A3F, 0x10A3F }, { 0x1D167, 0x1D169 },
    { 0x1D173, 0x1D182 }, { 0x1D185, 0x1D18B }, { 0x1D1AA, 0x1D1AD },
    { 0x1D242, 0x1D244 }, { 0xE0001, 0xE0001 }, { 0xE0020, 0xE007F },
    { 0xE0100, 0xE01EF }
  };

  if (unlikely(ucs == 0))
    return 0;
  if (unlikely(ucs < 32 || (ucs >= 0x7f && ucs < 0xa0)))
    return 1;

  if (unlikely(bisearch(ucs, combining, sizeof(combining) / sizeof(struct interval) - 1)))
    return 0;

  return 1 +
    (ucs >= 0x1100 &&
     (ucs <= 0x115f ||                    
      ucs == 0x2329 || ucs == 0x232a ||
      (ucs >= 0x2e80 && ucs <= 0xa4cf &&
       ucs != 0x303f) ||                  
      (ucs >= 0xac00 && ucs <= 0xd7a3) || 
      (ucs >= 0xf900 && ucs <= 0xfaff) || 
      (ucs >= 0xfe10 && ucs <= 0xfe19) || 
      (ucs >= 0xfe30 && ucs <= 0xfe6f) || 
      (ucs >= 0xff00 && ucs <= 0xff60) || 
      (ucs >= 0xffe0 && ucs <= 0xffe6) ||
      (ucs >= 0x20000 && ucs <= 0x2fffd) ||
      (ucs >= 0x30000 && ucs <= 0x3fffd)));
}

static int32_t unicode_to_cp437(uint64_t code_point) {
    switch (code_point) {
        case 0x263a: return 1;
        case 0x263b: return 2;
        case 0x2665: return 3;
        case 0x2666: return 4;
        case 0x2663: return 5;
        case 0x2660: return 6;
        case 0x2022: return 7;
        case 0x25d8: return 8;
        case 0x25cb: return 9;
        case 0x25d9: return 10;
        case 0x2642: return 11;
        case 0x2640: return 12;
        case 0x266a: return 13;
        case 0x266b: return 14;
        case 0x263c: return 15;
        case 0x25ba: return 16;
        case 0x25c4: return 17;
        case 0x2195: return 18;
        case 0x203c: return 19;
        case 0x00b6: return 20;
        case 0x00a7: return 21;
        case 0x25ac: return 22;
        case 0x21a8: return 23;
        case 0x2191: return 24;
        case 0x2193: return 25;
        case 0x2192: return 26;
        case 0x2190: return 27;
        case 0x221f: return 28;
        case 0x2194: return 29;
        case 0x25b2: return 30;
        case 0x25bc: return 31;

        case 0x2302: return 127;
        case 0x00c7: return 128;
        case 0x00fc: return 129;
        case 0x00e9: return 130;
        case 0x00e2: return 131;
        case 0x00e4: return 132;
        case 0x00e0: return 133;
        case 0x00e5: return 134;
        case 0x00e7: return 135;
        case 0x00ea: return 136;
        case 0x00eb: return 137;
        case 0x00e8: return 138;
        case 0x00ef: return 139;
        case 0x00ee: return 140;
        case 0x00ec: return 141;
        case 0x00c4: return 142;
        case 0x00c5: return 143;
        case 0x00c9: return 144;
        case 0x00e6: return 145;
        case 0x00c6: return 146;
        case 0x00f4: return 147;
        case 0x00f6: return 148;
        case 0x00f2: return 149;
        case 0x00fb: return 150;
        case 0x00f9: return 151;
        case 0x00ff: return 152;
        case 0x00d6: return 153;
        case 0x00dc: return 154;
        case 0x00a2: return 155;
        case 0x00a3: return 156;
        case 0x00a5: return 157;
        case 0x20a7: return 158;
        case 0x0192: return 159;
        case 0x00e1: return 160;
        case 0x00ed: return 161;
        case 0x00f3: return 162;
        case 0x00fa: return 163;
        case 0x00f1: return 164;
        case 0x00d1: return 165;
        case 0x00aa: return 166;
        case 0x00ba: return 167;
        case 0x00bf: return 168;
        case 0x2310: return 169;
        case 0x00ac: return 170;
        case 0x00bd: return 171;
        case 0x00bc: return 172;
        case 0x00a1: return 173;
        case 0x00ab: return 174;
        case 0x00bb: return 175;
        case 0x2591: return 176;
        case 0x2592: return 177;
        case 0x2593: return 178;
        case 0x2502: return 179;
        case 0x2524: return 180;
        case 0x2561: return 181;
        case 0x2562: return 182;
        case 0x2556: return 183;
        case 0x2555: return 184;
        case 0x2563: return 185;
        case 0x2551: return 186;
        case 0x2557: return 187;
        case 0x255d: return 188;
        case 0x255c: return 189;
        case 0x255b: return 190;
        case 0x2510: return 191;
        case 0x2514: return 192;
        case 0x2534: return 193;
        case 0x252c: return 194;
        case 0x251c: return 195;
        case 0x2500: return 196;
        case 0x253c: return 197;
        case 0x255e: return 198;
        case 0x255f: return 199;
        case 0x255a: return 200;
        case 0x2554: return 201;
        case 0x2569: return 202;
        case 0x2566: return 203;
        case 0x2560: return 204;
        case 0x2550: return 205;
        case 0x256c: return 206;
        case 0x2567: return 207;
        case 0x2568: return 208;
        case 0x2564: return 209;
        case 0x2565: return 210;
        case 0x2559: return 211;
        case 0x2558: return 212;
        case 0x2552: return 213;
        case 0x2553: return 214;
        case 0x256b: return 215;
        case 0x256a: return 216;
        case 0x2518: return 217;
        case 0x250c: return 218;
        case 0x2588: return 219;
        case 0x2584: return 220;
        case 0x258c: return 221;
        case 0x2590: return 222;
        case 0x2580: return 223;
        case 0x03b1: return 224;
        case 0x00df: return 225;
        case 0x0393: return 226;
        case 0x03c0: return 227;
        case 0x03a3: return 228;
        case 0x03c3: return 229;
        case 0x00b5: return 230;
        case 0x03c4: return 231;
        case 0x03a6: return 232;
        case 0x0398: return 233;
        case 0x03a9: return 234;
        case 0x03b4: return 235;
        case 0x221e: return 236;
        case 0x03c6: return 237;
        case 0x03b5: return 238;
        case 0x2229: return 239;
        case 0x2261: return 240;
        case 0x00b1: return 241;
        case 0x2265: return 242;
        case 0x2264: return 243;
        case 0x2320: return 244;
        case 0x2321: return 245;
        case 0x00f7: return 246;
        case 0x2248: return 247;
        case 0x00b0: return 248;
        case 0x2219: return 249;
        case 0x00b7: return 250;
        case 0x221a: return 251;
        case 0x207f: return 252;
        case 0x00b2: return 253;
        case 0x25a0: return 254;
    }

    return -1;
}

static void flanterm_putchar(struct flanterm_context *ctx, uint8_t c) {
    if (unlikely(ctx->discard_next || (c == 0x18 || c == 0x1a))) {
        ctx->discard_next = false;
        ctx->escape = false;
        ctx->control_sequence = false;
        ctx->unicode_remaining = 0;
        ctx->osc = false;
        ctx->osc_escape = false;
        ctx->g_select = 0;
        return;
    }

    if (unlikely(ctx->unicode_remaining != 0)) {
        if (unlikely((c & 0xc0) != 0x80)) {
            ctx->unicode_remaining = 0;
            goto unicode_error;
        }

        ctx->unicode_remaining--;
        ctx->code_point |= (uint64_t)(c & 0x3f) << (6 * ctx->unicode_remaining);
        if (likely(ctx->unicode_remaining != 0)) {
            return;
        }

        int32_t cc = unicode_to_cp437(ctx->code_point);

        if (unlikely(cc == -1)) {
            size_t replacement_width = (size_t)mk_wcwidth(ctx->code_point);
            if (likely(replacement_width > 0)) {
                term_raw_putchar(ctx, 0xfe);
            }
            for (size_t i = 1; i < replacement_width; i++) {
                term_raw_putchar(ctx, ' ');
            }
        } else {
            term_raw_putchar(ctx, cc);
        }
        return;
    }

unicode_error:
    if (likely(c >= 0xc0 && c <= 0xf7)) {
        if (c >= 0xc0 && c <= 0xdf) {
            ctx->unicode_remaining = 1;
            ctx->code_point = (uint64_t)(c & 0x1f) << 6;
        } else if (c >= 0xe0 && c <= 0xef) {
            ctx->unicode_remaining = 2;
            ctx->code_point = (uint64_t)(c & 0x0f) << (6 * 2);
        } else if (c >= 0xf0 && c <= 0xf7) {
            ctx->unicode_remaining = 3;
            ctx->code_point = (uint64_t)(c & 0x07) << (6 * 3);
        }
        return;
    }

    if (unlikely(ctx->escape == true)) {
        escape_parse(ctx, c);
        return;
    }

    if (unlikely(ctx->g_select)) {
        ctx->g_select--;
        switch (c) {
            case 'B':
                ctx->charsets[ctx->g_select] = CHARSET_DEFAULT; break;
            case '0':
                ctx->charsets[ctx->g_select] = CHARSET_DEC_SPECIAL; break;
        }
        ctx->g_select = 0;
        return;
    }

    size_t x, y;
    term_get_cursor_pos(ctx, &x, &y);

    switch (c) {
        case 0x00:
        case 0x7f:
            return;
        case 0x1b:
            ctx->escape_offset = 0;
            ctx->escape = true;
            return;
        case '\t':
            if (unlikely((x / ctx->tab_size + 1) >= ctx->cols)) {
                term_set_cursor_pos(ctx, ctx->cols - 1, y);
                return;
            }
            term_set_cursor_pos(ctx, (x / ctx->tab_size + 1) * ctx->tab_size, y);
            return;
        case 0x0b:
        case 0x0c:
        case '\n':
            if (unlikely(y == ctx->scroll_bottom_margin - 1)) {
                term_scroll(ctx);
                term_set_cursor_pos(ctx, (ctx->oob_output & FLANTERM_OOB_OUTPUT_ONLCR) ? 0 : x, y);
            } else {
                term_set_cursor_pos(ctx, (ctx->oob_output & FLANTERM_OOB_OUTPUT_ONLCR) ? 0 : x, y + 1);
            }
            return;
        case '\b':
            term_set_cursor_pos(ctx, x - 1, y);
            return;
        case '\r':
            term_set_cursor_pos(ctx, 0, y);
            return;
        case '\a':
            if (likely(ctx->callback != NULL)) {
                ctx->callback(ctx, FLANTERM_CB_BELL, 0, 0, 0);
            }
            return;
        case 14:
            ctx->current_charset = 1;
            return;
        case 15:
            ctx->current_charset = 0;
            return;
    }

    if (unlikely(ctx->insert_mode == true)) {
        for (size_t i = ctx->cols - 1; ; i--) {
            term_move_character(ctx, i + 1, y, i, y);
            if (unlikely(i == x)) {
                break;
            }
        }
    }

    switch (ctx->charsets[ctx->current_charset]) {
        case CHARSET_DEFAULT:
            break;
        case CHARSET_DEC_SPECIAL:
            if (unlikely(dec_special_print(ctx, c))) {
                return;
            }
            break;
    }

    if (likely(c >= 0x20 && c <= 0x7e)) {
        term_raw_putchar(ctx, c);
    } else {
        term_raw_putchar(ctx, 0xfe);
    }
}
#pragma endregion

#pragma region "Init/Deinit/Resize"
struct flanterm_context *flanterm_ttf_init(FrameBuffer *fb, TTF_Font *font, size_t margin) {
    struct flanterm_context *ctx = (struct flanterm_context *)malloc(sizeof(struct flanterm_context));
    if (unlikely(!ctx)) return NULL;
    memset(ctx, 0, sizeof(struct flanterm_context));

    ctx->grid = NULL;
    ctx->dirty_rows = NULL;

    ctx->ansi_colours[0] = 0x000000; ctx->ansi_colours[1] = 0xAA0000;
    ctx->ansi_colours[2] = 0x00AA00; ctx->ansi_colours[3] = 0xAA5500;
    ctx->ansi_colours[4] = 0x0000AA; ctx->ansi_colours[5] = 0xAA00AA;
    ctx->ansi_colours[6] = 0x00AAAA; ctx->ansi_colours[7] = 0xAAAAAA;
    
    ctx->ansi_bright_colours[0] = 0x555555; ctx->ansi_bright_colours[1] = 0xFF5555;
    ctx->ansi_bright_colours[2] = 0x55FF55; ctx->ansi_bright_colours[3] = 0xFFFF55;
    ctx->ansi_bright_colours[4] = 0x5555FF; ctx->ansi_bright_colours[5] = 0xFF55FF;
    ctx->ansi_bright_colours[6] = 0x55FFFF; ctx->ansi_bright_colours[7] = 0xFFFFFF;

    ctx->default_fg = 0xAAAAAA;
    ctx->default_bg = 0x000000;
    ctx->text_fg = ctx->default_fg;
    ctx->text_bg = 0xFFFFFFFF;

    flanterm_ttf_resize(ctx, fb, font, margin);
    if (unlikely(!ctx->grid || !ctx->dirty_rows)) {
        free(ctx);
        return NULL;
    }

    return ctx;
}

void flanterm_ttf_resize(struct flanterm_context *ctx, FrameBuffer *fb, TTF_Font *font, size_t margin) {
    if (unlikely(!ctx || !fb || !font)) return;

    if (ctx->grid) free(ctx->grid);
    if (ctx->dirty_rows) free(ctx->dirty_rows);

    ctx->fb = fb;
    ctx->font = font;
    ctx->margin = margin;
    ctx->screen_width = fb->Width;
    ctx->screen_height = fb->Height;

    int32_t w, h;
    TTF_GetTextSize(font, "M", &w, &h);
    if (unlikely(w <= 0)) w = 8;
    if (unlikely(h <= 0)) h = 16;
    ctx->char_width = w;
    ctx->char_height = h;

    int cols = (ctx->screen_width - margin * 2) / ctx->char_width;
    int rows = (ctx->screen_height - margin * 2) / ctx->char_height;
    if (unlikely(cols <= 0)) cols = 1;
    if (unlikely(rows <= 0)) rows = 1;

    ctx->cols = cols;
    ctx->rows = rows;

    ctx->grid = (struct flanterm_ttf_char *)malloc(ctx->rows * ctx->cols * sizeof(struct flanterm_ttf_char));
    ctx->dirty_rows = (bool *)malloc(ctx->rows * sizeof(bool));
    
    if (unlikely(!ctx->grid || !ctx->dirty_rows)) {
        if (ctx->grid) free(ctx->grid);
        if (ctx->dirty_rows) free(ctx->dirty_rows);
        ctx->grid = NULL;
        ctx->dirty_rows = NULL;
        return;
    }

    flanterm_context_reinit(ctx);
    term_clear(ctx, true);
    term_full_refresh(ctx);
}

void flanterm_deinit(struct flanterm_context *ctx) {
    if (unlikely(!ctx)) return;
    if (ctx->grid) free(ctx->grid);
    if (ctx->dirty_rows) free(ctx->dirty_rows);
    free(ctx);
}

void flanterm_flush(struct flanterm_context *ctx) { term_double_buffer_flush(ctx); }
void flanterm_clear(struct flanterm_context *ctx) { term_clear(ctx, true); }
void flanterm_full_refresh(struct flanterm_context *ctx) { term_full_refresh(ctx); }
void flanterm_get_dimensions(struct flanterm_context *ctx, size_t *cols, size_t *rows) { *cols = ctx->cols; *rows = ctx->rows; }
void flanterm_set_autoflush(struct flanterm_context *ctx, bool state) { ctx->autoflush = state; }
void flanterm_set_callback(struct flanterm_context *ctx, void (*callback)(struct flanterm_context *, uint64_t, uint64_t, uint64_t, uint64_t)) { ctx->callback = callback; }
uint64_t flanterm_get_oob_output(struct flanterm_context *ctx) { return ctx->oob_output; }
void flanterm_set_oob_output(struct flanterm_context *ctx, uint64_t oob_output) { ctx->oob_output = oob_output; }
#pragma endregion