#pragma once

typedef struct {
    int32_t sec;                    /* Seconds (0-60) */
    int32_t min;                    /* Minutes (0-59) */
    int32_t hour;                   /* Hours (0-23) */
    int32_t mday;                   /* Day of the month (1-31) */
    int32_t mon;                    /* Month (0-11) */
    int32_t year;                   /* Year - 1900 */
    int32_t wday;                   /* Day of the week (0-6, Sunday = 0) */
    int32_t yday;                   /* Day in the year (0-365, 1 Jan = 0) */
    int32_t isdst;                  /* Daylight saving time */
} tm_t;
