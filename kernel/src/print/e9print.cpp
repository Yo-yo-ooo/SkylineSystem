#include <print/e9print.h>
#include <stddef.h>
#include <stdint.h>

#include <flanterm/flanterm.h>
extern struct flanterm_context *ft_ctx;

static const char CONVERSION_TABLE[] = "0123456789abcdef";

void e9_putc(char c) {
#if defined (__x86_64__) || defined (__i386__)
    __asm__ __volatile__ ("outb %0, %1" :: "a" (c), "Nd" (0xe9) : "memory");
#endif
    if (ft_ctx != NULL) {
        flanterm_write(ft_ctx, &c, 1);
    }
}

void e9_print(const char *msg) {
    for (size_t i = 0; msg[i]; i++) {
        e9_putc(msg[i]);
    }
}

void e9_puts(const char *msg) {
    e9_print(msg);
    e9_putc('\n');
}

static void e9_printhex(size_t num) {
    int32_t i;
    char buf[17];

    if (!num) {
        e9_print("0x0");
        return;
    }

    buf[16] = 0;

    for (i = 15; num; i--) {
        buf[i] = CONVERSION_TABLE[num % 16];
        num /= 16;
    }

    i++;
    e9_print("0x");
    e9_print(&buf[i]);
}

static void e9_printdec(size_t num) {
    int32_t i;
    char buf[21] = {0};

    if (!num) {
        e9_putc('0');
        return;
    }

    for (i = 19; num; i--) {
        buf[i] = (num % 10) + 0x30;
        num = num / 10;
    }

    i++;
    e9_print(buf + i);
}

void e9_printf(const char *format, ...) {
    va_list argp;
    va_start(argp, format);

    while (*format != '\0') {
        if (*format == '%') {
            format++;
            if (*format == 'x') {
                e9_printhex(va_arg(argp, size_t));
            } else if (*format == 'd') {
                e9_printdec(va_arg(argp, size_t));
            } else if (*format == 's') {
                e9_print(va_arg(argp, char*));
            }
        } else {
            e9_putc(*format);
        }
        format++;
    }

    e9_putc('\n');
    va_end(argp);
}

void e9_printNL(const char *format, ...) {
    va_list argp;
    va_start(argp, format);

    while (*format != '\0') {
        if (*format == '%') {
            format++;
            if (*format == 'x') {
                e9_printhex(va_arg(argp, size_t));
            } else if (*format == 'd') {
                e9_printdec(va_arg(argp, size_t));
            } else if (*format == 's') {
                e9_print(va_arg(argp, char*));
            }
        } else {
            e9_putc(*format);
        }
        format++;
    }
    va_end(argp);
}