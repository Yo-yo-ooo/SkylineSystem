//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT
//
// scanf.c - full scanf / sscanf for SkylineSystem.
//
// The parser is written against a tiny character-source abstraction so the
// same core drives both:
//
//   * scanf  - stdin is the shared keyboard event ring, gathered through a
//              canonical line discipline (echo, Backspace editing, Enter
//              submits; the submitting '\n' is kept as the token delimiter);
//   * sscanf - the source is a NUL-terminated string.
//
// Conversions: %d %i %u %x %X %o %f %e %g %a %F %E %G %A %c %s %p %n %[ %%
// with field width, assignment suppression '*' and length modifiers
// hh / h / l / ll / L / j / z / t. Integer and float token conversion reuse
// the SWAR strtoll/strtoull/strtod family in basic_convert.
//
// Note on portability: va_list is an array type in C (decays as a parameter)
// but an object type in C++, so it is never passed to helpers. All va_arg()
// calls happen in vscan itself; helpers only store through a void* target.
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <base/arch/x86_64/syscall.h>
#include <base/arch/x86_64/syscalln.h>
#include <graphic/devtype.h>
#include <graphic/kbdshare.h>

/* ================================================================== */
/* character source                                                   */
/* ================================================================== */
typedef struct ScanSrc {
    int  (*get)(void *ctx);    /* consume next: unsigned char, or EOF      */
    void (*unget)(void *ctx);  /* undo the last get (one level)            */
    void *ctx;
    long  consumed;            /* net characters taken (for %n)            */
} ScanSrc;

static int src_get(ScanSrc *s) {
    int c = s->get(s->ctx);
    if (c >= 0) s->consumed++;
    return c;
}
static void src_unget(ScanSrc *s) {
    s->unget(s->ctx);
    s->consumed--;
}

static int is_ws(int c) { return c >= 0 && isspace(c); }

/* ================================================================== */
/* stdin source: keyboard ring + canonical line buffer                */
/* ================================================================== */
#define SCAN_LINE_CAP 256

static KbdShared *g_kbd;
static int        g_slot = -1;
static uint64_t   g_pos;

static char   g_line[SCAN_LINE_CAP];
static size_t g_line_len;
static size_t g_line_pos;
static int    g_have_line;

static KbdShared *stdin_kbd(void) {
    if (g_kbd) return g_kbd;

    uint64_t va = syscall(SYSCALL_DEV_MMAP, (uint64_t)DEV_TYPE_KEYBOARD,
                          0, 0, 0, 0, 0);
    if ((int64_t)va <= 0) return NULL;

    g_kbd  = (KbdShared *)va;
    g_slot = kbd_reader_register(g_kbd);
    g_pos  = (g_slot >= 0)
               ? kbd_reader_pos(g_kbd, g_slot)
               : __atomic_load_n(&g_kbd->head, __ATOMIC_ACQUIRE);
    return g_kbd;
}

static uint16_t next_key(void) {
    KbdShared *k = stdin_kbd();
    if (!k) return 0;

    for (;;) {
        uint64_t h = __atomic_load_n(&k->head, __ATOMIC_ACQUIRE);
        g_pos = kbd_reader_resync(k, g_pos);

        while (g_pos < h) {
            KbdEvent e = k->ring[g_pos & (KBD_RING_CAP - 1u)];
            g_pos++;
            if (g_slot >= 0) kbd_reader_setpos(k, g_slot, g_pos);
            if (e.action == KBD_ACTION_DOWN) return e.key;
        }
        sys_yield();
    }
}

static void echo_str(const char *s, size_t n) { console_stdout_write(s, n); }

static void read_line(void) {
    g_line_len = 0;
    g_line[0]  = '\0';

    for (;;) {
        uint16_t key = next_key();

        if (key == '\n' || key == '\r') {
            /* Keep '\n' in the buffer as the token delimiter, the way a cooked
               tty delivers it; otherwise %s/%d block into the next line. */
            if (g_line_len < SCAN_LINE_CAP - 1)
                g_line[g_line_len++] = '\n';
            g_line[g_line_len] = '\0';
            echo_str("\n", 1);
            g_line_pos  = 0;
            g_have_line = 1;
            return;
        }
        if (key == 0x08u) {                     /* Backspace */
            if (g_line_len > 0) {
                g_line_len--;
                g_line[g_line_len] = '\0';
                echo_str("\b \b", 3);
            }
            continue;
        }
        if ((key >= 0x20u && key < 0x7Fu) || key == '\t') {
            if (g_line_len < SCAN_LINE_CAP - 1) {
                g_line[g_line_len++] = (char)key;
                g_line[g_line_len]   = '\0';
                char c = (char)key;
                echo_str(&c, 1);
            }
            continue;
        }
        /* arrows / function keys ignored in canonical mode */
    }
}

static int stdin_get_f(void *ctx) {
    (void)ctx;
    if (!g_have_line || g_line_pos >= g_line_len) read_line();
    return (int)(uint8_t)g_line[g_line_pos++];
}
static void stdin_unget_f(void *ctx) {
    (void)ctx;
    if (g_line_pos > 0) g_line_pos--;
}

/* ================================================================== */
/* string source (sscanf)                                             */
/* ================================================================== */
typedef struct { const char *s; size_t pos; } StrSrc;

static int str_get_f(void *ctx) {
    StrSrc *r = (StrSrc *)ctx;
    char c = r->s[r->pos];
    if (c == '\0') return EOF;
    r->pos++;
    return (int)(uint8_t)c;
}
static void str_unget_f(void *ctx) {
    StrSrc *r = (StrSrc *)ctx;
    if (r->pos > 0) r->pos--;
}

/* ================================================================== */
/* length modifiers                                                   */
/* ================================================================== */
enum {
    LM_D = 0,   /* default int / float   */
    LM_HH,      /* char                 */
    LM_H,       /* short                */
    LM_L,       /* long / double        */
    LM_LL,      /* long long            */
    LM_J,       /* intmax_t             */
    LM_Z,       /* size_t               */
    LM_T,       /* ptrdiff_t            */
    LM_LD       /* long double          */
};

/* ================================================================== */
/* target-pointer extraction (va_arg must live in vscan)              */
/* ================================================================== */
#define NEXT_SIGNED_TARGET(ap, lm, t)                          \
    switch (lm) {                                              \
    case LM_HH: (t) = va_arg((ap), signed char *);  break;     \
    case LM_H:  (t) = va_arg((ap), short *);         break;     \
    case LM_L:  (t) = va_arg((ap), long *);          break;     \
    case LM_LL:                                                \
    case LM_J:  (t) = va_arg((ap), long long *);     break;     \
    case LM_T:  (t) = va_arg((ap), ptrdiff_t *);     break;     \
    case LM_Z:  (t) = va_arg((ap), size_t *);        break;     \
    default:    (t) = va_arg((ap), int *);           break;     \
    }
#define NEXT_UNSIGNED_TARGET(ap, lm, t)                        \
    switch (lm) {                                              \
    case LM_HH: (t) = va_arg((ap), unsigned char *);  break;   \
    case LM_H:  (t) = va_arg((ap), unsigned short *); break;   \
    case LM_L:  (t) = va_arg((ap), unsigned long *);  break;   \
    case LM_LL:                                                \
    case LM_J:  (t) = va_arg((ap), unsigned long long *); break;\
    case LM_Z:  (t) = va_arg((ap), size_t *);         break;   \
    case LM_T:  (t) = va_arg((ap), ptrdiff_t *);      break;   \
    default:    (t) = va_arg((ap), unsigned int *);   break;   \
    }

/* ================================================================== */
/* storage through a void* target                                     */
/* ================================================================== */
static void put_signed(void *t, int lm, long long v) {
    switch (lm) {
    case LM_HH: *(signed char *)t  = (signed char)v;  break;
    case LM_H:  *(short *)t        = (short)v;        break;
    case LM_L:  *(long *)t         = (long)v;         break;
    case LM_LL:
    case LM_J:  *(long long *)t    = v;               break;
    case LM_T:  *(ptrdiff_t *)t    = (ptrdiff_t)v;    break;
    case LM_Z:  *(size_t *)t       = (size_t)v;       break;
    default:    *(int *)t          = (int)v;          break;
    }
}
static void put_unsigned(void *t, int lm, unsigned long long v) {
    switch (lm) {
    case LM_HH: *(unsigned char *)t  = (unsigned char)v;  break;
    case LM_H:  *(unsigned short *)t = (unsigned short)v; break;
    case LM_L:  *(unsigned long *)t  = (unsigned long)v;  break;
    case LM_LL:
    case LM_J:  *(unsigned long long *)t = v;             break;
    case LM_Z:  *(size_t *)t         = (size_t)v;         break;
    case LM_T:  *(ptrdiff_t *)t      = (ptrdiff_t)v;      break;
    default:    *(unsigned int *)t   = (unsigned int)v;   break;
    }
}

/* ================================================================== */
/* token gathering                                                    */
/* ================================================================== */
/* Skip leading whitespace when skip_ws, then read up to `width` (0 =
   unlimited) characters accepted by pred into tok. The first character that
   fails pred (or EOF) is pushed back. Returns token length, or -1 on EOF
   before any character. */
static int gather(ScanSrc *s, char *tok, int cap, int width,
                  bool (*pred)(int), bool skip_ws) {
    int c = src_get(s);
    if (c < 0) return -1;
    if (skip_ws) {
        while (is_ws(c)) {
            c = src_get(s);
            if (c < 0) return -1;
        }
    }

    int n = 0;
    while (c >= 0 && pred(c) && n < cap - 1 && (width == 0 || n < width)) {
        tok[n++] = (char)c;
        c = src_get(s);
    }
    if (c >= 0) src_unget(s);
    tok[n] = '\0';
    return n;
}

static bool pred_int(int c) {
    return isxdigit(c) || c == 'x' || c == 'X' || c == '+' || c == '-';
}
static bool pred_float(int c) {
    return isalnum(c) || c == '.' || c == '+' || c == '-';
}
static bool pred_ptr(int c) {
    return isxdigit(c) || c == 'x' || c == 'X';
}

/* ================================================================== */
/* core parser                                                        */
/* ================================================================== */
static int32_t vscan(ScanSrc *s, const char *fmt, va_list ap) {
    int assigned = 0;

    while (*fmt) {
        /* Whitespace in format: consume any amount of input whitespace. */
        if (is_ws((unsigned char)*fmt)) {
            int c = src_get(s);
            while (is_ws(c)) c = src_get(s);
            if (c < 0) return assigned ? assigned : EOF;
            src_unget(s);
            fmt++;
            continue;
        }

        /* Literal character: must match exactly. */
        if (*fmt != '%') {
            int c = src_get(s);
            if (c < 0) return assigned ? assigned : EOF;
            if (c != (unsigned char)*fmt) { src_unget(s); return assigned; }
            fmt++;
            continue;
        }

        fmt++;  /* consume '%' */

        bool suppress = false;
        if (*fmt == '*') { suppress = true; fmt++; }

        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }

        int lm = LM_D;
        switch (*fmt) {
        case 'h':
            fmt++;
            if (*fmt == 'h') { fmt++; lm = LM_HH; } else lm = LM_H;
            break;
        case 'l':
            fmt++;
            if (*fmt == 'l') { fmt++; lm = LM_LL; } else lm = LM_L;
            break;
        case 'L': fmt++; lm = LM_LD; break;
        case 'j': fmt++; lm = LM_J;  break;
        case 'z': fmt++; lm = LM_Z;  break;
        case 't': fmt++; lm = LM_T;  break;
        case 'q': fmt++; lm = LM_LL; break;   /* BSD alias for ll */
        default: break;
        }

        char conv = *fmt++;

        if (conv == '%') {
            int c = src_get(s);
            if (c < 0) return assigned ? assigned : EOF;
            if (c != '%') { src_unget(s); return assigned; }
            continue;
        }

        /* %n: store characters consumed so far (not an assignment). */
        if (conv == 'n') {
            if (!suppress) {
                void *t = NULL;
                NEXT_SIGNED_TARGET(ap, lm, t);
                put_signed(t, lm, s->consumed);
            }
            continue;
        }

        /* %c: read width (default 1) raw chars; no whitespace skip. */
        if (conv == 'c') {
            int n = width ? width : 1;
            char *p = suppress ? NULL : va_arg(ap, char *);
            for (int i = 0; i < n; i++) {
                int c = src_get(s);
                if (c < 0) return assigned ? assigned : EOF;
                if (p) p[i] = (char)c;
            }
            if (p) assigned++;
            continue;
        }

        /* %[ : scanset (no whitespace skip). */
        if (conv == '[') {
            const char *q = fmt;
            bool neg = false;
            if (*q == '^') { neg = true; q++; }

            bool allow[256];
            memset(allow, 0, sizeof allow);
            bool first = true;
            for (;;) {
                unsigned char mc = (unsigned char)*q;
                if (mc == ']' && !first) { q++; break; }
                if (mc == '\0') break;                 /* malformed set */

                if (q[1] == '-' && q[2] != ']' && q[2] != '\0') {
                    unsigned char lo = mc, hi = (unsigned char)q[2];
                    if (lo > hi) { unsigned char t = lo; lo = hi; hi = t; }
                    for (int k = lo; k <= hi; k++) allow[k] = true;
                    q += 3;
                } else {
                    allow[mc] = true;
                    q++;
                }
                first = false;
            }
            fmt = q;

            char *p = suppress ? NULL : va_arg(ap, char *);
            int cap = width ? width + 1 : 4096;
            int n = 0;
            int c = src_get(s);
            while (c >= 0) {
                bool member = allow[c & 255];
                if (neg ? member : !member) break;
                if (p && n < cap - 1) p[n++] = (char)c;
                c = src_get(s);
            }
            if (c >= 0) src_unget(s);
            if (n == 0) return assigned;              /* matching failure */
            if (p) { p[n] = '\0'; assigned++; }
            continue;
        }

        /* Floating-point conversions: f e g a (and uppercase). */
        if (conv == 'f' || conv == 'F' || conv == 'e' || conv == 'E' ||
            conv == 'g' || conv == 'G' || conv == 'a' || conv == 'A') {
            char tok[512];
            int n = gather(s, tok, (int)sizeof tok, width, pred_float, true);
            if (n < 0) return assigned ? assigned : EOF;

            char *end = NULL;
            if (!suppress) {
                if (lm == LM_LD) {
                    long double *p = va_arg(ap, long double *);
                    *p = strtold(tok, &end);
                } else if (lm == LM_L) {
                    double *p = va_arg(ap, double *);
                    *p = strtod(tok, &end);
                } else {
                    float *p = va_arg(ap, float *);
                    *p = strtof(tok, &end);
                }
                if (end == tok) return assigned;      /* no number parsed */
                assigned++;
            }
            continue;
        }

        /* %p: implementation-defined pointer, read as hex (0x allowed). */
        if (conv == 'p') {
            char tok[64];
            int n = gather(s, tok, (int)sizeof tok, width, pred_ptr, true);
            if (n < 0) return assigned ? assigned : EOF;
            if (!suppress) {
                void **p = va_arg(ap, void **);
                *p = (void *)(uintptr_t)strtoull(tok, NULL, 16);
                assigned++;
            }
            continue;
        }

        /* Integer conversions: d i u x X o. */
        if (conv == 'd' || conv == 'i' || conv == 'u' ||
            conv == 'x' || conv == 'X' || conv == 'o') {
            char tok[128];
            int n = gather(s, tok, (int)sizeof tok, width, pred_int, true);
            if (n < 0) return assigned ? assigned : EOF;
            if (n == 0) return assigned;

            int base = (conv == 'x' || conv == 'X') ? 16
                     : (conv == 'o') ? 8
                     : (conv == 'i') ? 0 : 10;

            char *end = NULL;
            if (conv == 'd' || conv == 'i') {
                long long v = strtoll(tok, &end, base);
                if (end == tok) return assigned;
                if (!suppress) {
                    void *t = NULL;
                    NEXT_SIGNED_TARGET(ap, lm, t);
                    put_signed(t, lm, v);
                    assigned++;
                }
            } else {
                unsigned long long v = strtoull(tok, &end, base);
                if (end == tok) return assigned;
                if (!suppress) {
                    void *t = NULL;
                    NEXT_UNSIGNED_TARGET(ap, lm, t);
                    put_unsigned(t, lm, v);
                    assigned++;
                }
            }
            continue;
        }

        /* %s: run of non-whitespace characters. */
        if (conv == 's') {
            char tok[SCAN_LINE_CAP];
            int cap = width ? width + 1 : (int)sizeof tok;
            if (cap > (int)sizeof tok) cap = (int)sizeof tok;

            int c = src_get(s);
            while (is_ws(c)) c = src_get(s);
            if (c < 0) return assigned ? assigned : EOF;

            int n = 0;
            while (c >= 0 && !is_ws(c)) {
                if (n < cap - 1) tok[n++] = (char)c;
                c = src_get(s);
            }
            if (c >= 0) src_unget(s);
            if (!suppress) {
                char *p = va_arg(ap, char *);
                memcpy(p, tok, (size_t)n);
                p[n] = '\0';
                assigned++;
            }
            continue;
        }

        /* Unknown conversion: stop. */
        return assigned;
    }

    return assigned;
}

/* ================================================================== */
/* public entry points                                                */
/* ================================================================== */
int32_t scanf_(const char *format, ...) {
    va_list ap;
    va_start(ap, format);

    ScanSrc src = { stdin_get_f, stdin_unget_f, NULL, 0 };
    int32_t r = vscan(&src, format, ap);

    va_end(ap);
    return r;
}

int32_t sscanf_(const char *str, const char *format, ...) {
    va_list ap;
    va_start(ap, format);

    StrSrc ctx = { str, 0 };
    ScanSrc src = { str_get_f, str_unget_f, &ctx, 0 };
    int32_t r = vscan(&src, format, ap);

    va_end(ap);
    return r;
}
