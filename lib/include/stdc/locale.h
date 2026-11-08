//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT

/*
 * locale.h — ISO C 本地化头 (SkylineSystem 用户态 libc)
 *
 * 目前只实现 C/POSIX locale:
 *   - setlocale(..., NULL)  返回当前 locale 名 "C"
 *   - setlocale(..., "C")/"POSIX"/"" 成功, 返回 "C"
 *   - 其他 locale 名返回 NULL (未支持)
 *   - localeconv()->decimal_point 恒为 "."
 *
 * Lua 5.4 依赖 lua_getlocaledecpoint() = localeconv()->decimal_point[0]
 * (luaconf.h), 因此 decimal_point 必须是 struct lconv 的第一个成员。
 */

#pragma once
#ifndef SKYLINE_LIB_LOCALE_H
#define SKYLINE_LIB_LOCALE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ISO C locale 分类 (取值与 glibc 一致) */
#define LC_ALL       0
#define LC_COLLATE   1
#define LC_CTYPE     2
#define LC_MONETARY  3
#define LC_NUMERIC   4
#define LC_TIME      5

#define LC_MIN       LC_ALL
#define LC_MAX       LC_TIME

struct lconv {
    /* 顺序必须与 ISO C 一致: decimal_point 是第一个成员 */
    char *decimal_point;
    char *thousands_sep;
    char *grouping;
    char *int_curr_symbol;
    char *currency_symbol;
    char *mon_decimal_point;
    char *mon_thousands_sep;
    char *mon_grouping;
    char *positive_sign;
    char *negative_sign;
    char int_frac_digits;
    char frac_digits;
    char p_cs_precedes;
    char p_sep_by_space;
    char n_cs_precedes;
    char n_sep_by_space;
    char p_sign_posn;
    char n_sign_posn;
};

char *setlocale(int category, const char *locale);
struct lconv *localeconv(void);

#ifdef __cplusplus
}
#endif

#endif /* SKYLINE_LIB_LOCALE_H */
