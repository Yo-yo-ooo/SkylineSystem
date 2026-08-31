#ifndef FLANTERM_H
#define FLANTERM_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <graphic/fb.h>
#include <base/font/ttf/ttf.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FLANTERM_CB_DEC 10
#define FLANTERM_CB_BELL 20
#define FLANTERM_CB_PRIVATE_ID 30
#define FLANTERM_CB_STATUS_REPORT 40
#define FLANTERM_CB_POS_REPORT 50
#define FLANTERM_CB_KBD_LEDS 60
#define FLANTERM_CB_MODE 70
#define FLANTERM_CB_LINUX 80

#define FLANTERM_OOB_OUTPUT_OCRNL (1 << 0)
#define FLANTERM_OOB_OUTPUT_OFDEL (1 << 1)
#define FLANTERM_OOB_OUTPUT_OFILL (1 << 2)
#define FLANTERM_OOB_OUTPUT_OLCUC (1 << 3)
#define FLANTERM_OOB_OUTPUT_ONLCR (1 << 4)
#define FLANTERM_OOB_OUTPUT_ONLRET (1 << 5)
#define FLANTERM_OOB_OUTPUT_ONOCR (1 << 6)
#define FLANTERM_OOB_OUTPUT_OPOST (1 << 7)

#define FLANTERM_MAX_ESC_VALUES 16

struct flanterm_ttf_char {
    uint8_t c;
    uint32_t fg;
    uint32_t bg;
};

struct flanterm_context {
    size_t tab_size;
    bool autoflush;
    bool cursor_enabled;
    bool scroll_enabled;
    bool control_sequence;
    bool escape;
    bool osc;
    bool osc_escape;
    bool rrr;
    bool discard_next;
    bool bold;
    bool bg_bold;
    bool reverse_video;
    bool dec_private;
    bool insert_mode;
    uint64_t code_point;
    size_t unicode_remaining;
    uint8_t g_select;
    uint8_t charsets[2];
    size_t current_charset;
    size_t escape_offset;
    size_t esc_values_i;
    size_t saved_cursor_x;
    size_t saved_cursor_y;
    size_t current_primary;
    size_t current_bg;
    size_t scroll_top_margin;
    size_t scroll_bottom_margin;
    uint32_t esc_values[FLANTERM_MAX_ESC_VALUES];
    uint64_t oob_output;
    bool saved_state_bold;
    bool saved_state_bg_bold;
    bool saved_state_reverse_video;
    size_t saved_state_current_charset;
    size_t saved_state_current_primary;
    size_t saved_state_current_bg;

    FrameBuffer *fb;
    TTF_Font *font;
    int char_width;
    int char_height;
    int margin;
    int screen_width;
    int screen_height;
    size_t rows, cols;

    struct flanterm_ttf_char *grid;
    bool *dirty_rows;

    size_t cursor_x;
    size_t cursor_y;
    size_t old_cursor_x;
    size_t old_cursor_y;

    uint32_t text_fg;
    uint32_t text_bg;
    uint32_t default_fg;
    uint32_t default_bg;

    uint32_t ansi_colours[8];
    uint32_t ansi_bright_colours[8];

    uint32_t saved_text_fg;
    uint32_t saved_text_bg;
    size_t saved_state_cursor_x;
    size_t saved_state_cursor_y;

    void (*callback)(struct flanterm_context *, uint64_t, uint64_t, uint64_t, uint64_t);
};


void flanterm_write(struct flanterm_context *ctx, const char *buf, size_t count);
void flanterm_flush(struct flanterm_context *ctx);
void flanterm_full_refresh(struct flanterm_context *ctx);
void flanterm_deinit(struct flanterm_context *ctx);

void flanterm_get_dimensions(struct flanterm_context *ctx, size_t *cols, size_t *rows);
void flanterm_set_autoflush(struct flanterm_context *ctx, bool state);
void flanterm_set_callback(struct flanterm_context *ctx, void (*callback)(struct flanterm_context *, uint64_t, uint64_t, uint64_t, uint64_t));
uint64_t flanterm_get_oob_output(struct flanterm_context *ctx);
void flanterm_set_oob_output(struct flanterm_context *ctx, uint64_t oob_output);

void flanterm_clear(struct flanterm_context *ctx);

struct flanterm_context *flanterm_ttf_init(FrameBuffer *fb, TTF_Font *font, size_t margin);
TTF_Font *console_font(void);
void flanterm_ttf_resize(struct flanterm_context *ctx, FrameBuffer *fb, TTF_Font *font, size_t margin);

#ifdef __cplusplus
}
#endif

#endif