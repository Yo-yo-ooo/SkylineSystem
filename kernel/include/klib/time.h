#pragma once

typedef struct {
    int sec;                    /* Seconds (0-60) */
    int min;                    /* Minutes (0-59) */
    int hour;                   /* Hours (0-23) */
    int mday;                   /* Day of the month (1-31) */
    int mon;                    /* Month (0-11) */
    int year;                   /* Year - 1900 */
    int wday;                   /* Day of the week (0-6, Sunday = 0) */
    int yday;                   /* Day in the year (0-365, 1 Jan = 0) */
    int isdst;                  /* Daylight saving time */
} tm_t;
