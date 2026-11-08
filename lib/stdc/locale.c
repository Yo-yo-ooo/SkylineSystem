//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT

/*
 * lib/stdc/locale.c — ISO C locale (目前仅 C/POSIX locale)
 *
 * Lua 5.4 只需要 localeconv()->decimal_point[0] (luaconf.h 的
 * lua_getlocaledecpoint), 同时标准要求 setlocale/localeconv 可链接。
 */

#include <locale.h>
#include <limits.h>
#include <string.h>

static const char sky_c_locale[] = "C";

static struct lconv sky_c_lconv = {
    /* 顺序与 ISO C 一致: decimal_point 必须是第一个成员 */
    .decimal_point     = ".",
    .thousands_sep     = "",
    .grouping          = "",
    .int_curr_symbol   = "",
    .currency_symbol   = "",
    .mon_decimal_point = "",
    .mon_thousands_sep = "",
    .mon_grouping      = "",
    .positive_sign     = "",
    .negative_sign     = "",
    .int_frac_digits   = (char)CHAR_MAX,
    .frac_digits       = (char)CHAR_MAX,
    .p_cs_precedes     = (char)CHAR_MAX,
    .p_sep_by_space    = (char)CHAR_MAX,
    .n_cs_precedes     = (char)CHAR_MAX,
    .n_sep_by_space    = (char)CHAR_MAX,
    .p_sign_posn       = (char)CHAR_MAX,
    .n_sign_posn       = (char)CHAR_MAX,
};

/*
 * setlocale:
 *   - category 非法 -> NULL
 *   - locale == NULL -> 查询, 返回当前 locale ("C")
 *   - locale 为 "C" / "POSIX" / "" -> 成功, 返回 "C"
 *   - 其他 locale 名 -> 未支持, 返回 NULL (当前 locale 不变)
 */
char *setlocale(int category, const char *locale) {
    if (category < LC_ALL || category > LC_TIME) return NULL;
    if (locale == NULL) return (char *)sky_c_locale;
    if (locale[0] == '\0' ||
        strcmp(locale, "C") == 0 ||
        strcmp(locale, "POSIX") == 0) {
        return (char *)sky_c_locale;
    }
    return NULL;
}

struct lconv *localeconv(void) {
    return &sky_c_lconv;
}
