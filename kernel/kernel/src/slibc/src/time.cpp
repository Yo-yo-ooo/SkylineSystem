#include "../../kernelStuff/other_IO/rtc/rtc.h"
#include "../include/time.h"
#define EPOCH_YEAR 1970
#define TM_YEAR_BASE 1900

#if INT_MAX <= (LONG_MAX / 4 / 366 / 24 / 60 / 60)
typedef long int long_int;
#else
typedef long long int long_int;
#endif

static const unsigned short int __g_mon_yday[2][13] = {
    /* Normal years.  */
    { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334, 365 },
    /* Leap years.  */
    { 0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335, 366 }
};
/* Is YEAR + TM_YEAR_BASE a leap year?  */
static inline bool __is_leapyear(long_int year)
{
    /* Don't add YEAR to TM_YEAR_BASE, as that might overflow.
       Also, work even if YEAR is negative.  */
    return ((year & 3) == 0 &&
            (year % 100 != 0 || ((year / 100) & 3) == (- (TM_YEAR_BASE / 100) & 3)));
}

/* Shift A right by B bits portably, by dividing A by 2**B and
   truncating towards minus infinity.  B should be in the range 0 <= B
   <= long_BITS - 2, where long_BITS is the number of useful
   bits in a long.  long_BITS is at least 32.
   ISO C99 says that A >> B is implementation-defined if A < 0.  Some
   implementations (e.g., UNICOS 9.0 on a Cray Y-MP EL) don't shift
   right in the usual way when A < 0, so SHR falls back on division if
   ordinary A >> B doesn't seem to be the usual signed shift.  */
static inline long_int __shr(long_int a, int b)
{
    long_int one = 1;
    return ((-one >> 1 == -1) ? (a >> b) : (a + (a < 0)) / (one << b) - (a < 0));
}

static inline long_int __do_ydhms_diff(long_int year1, long_int yday1, int hour1, int min1,
                                       int sec1,
                                       long_int year0, long_int yday0, int hour0, int min0, int sec0)
{
//    verify(-1 / 2 == 0);

    /* Compute intervening leap days correctly even if year is negative.
       Take care to avoid integer overflow here.  */
    int a4 = __shr(year1, 2) + __shr(TM_YEAR_BASE, 2) - !(year1 & 3);
    int b4 = __shr(year0, 2) + __shr(TM_YEAR_BASE, 2) - !(year0 & 3);
    int a100 = (a4 + (a4 < 0)) / 25 - (a4 < 0);
    int b100 = (b4 + (b4 < 0)) / 25 - (b4 < 0);
    int a400 = __shr(a100, 2);
    int b400 = __shr(b100, 2);
    int intervening_leap_days = (a4 - b4) - (a100 - b100) + (a400 - b400);

    /* Compute the desired time without overflowing.  */
    long_int years = year1 - year0;
    long_int days = 365 * years + yday1 - yday0 + intervening_leap_days;
    long_int hours = 24 * days + hour1 - hour0;
    long_int minutes = 60 * hours + min1 - min0;
    long_int seconds = 60 * minutes + sec1 - sec0;
    return seconds;
}

/* Convert *TP to a __time64_t value, inverting
   the monotonic and mostly-unit-linear conversion function CONVERT.
   Use *OFFSET to keep track of a guess at the offset of the result,
   compared to what the result would be for UTC without leap seconds.
   If *OFFSET's guess is correct, only one CONVERT call is needed.
   If successful, set *TP to the canonicalized struct tm;
   otherwise leave *TP alone, return ((time_t) -1) and set errno.
   This function is external because it is used also by timegm.c. */
static inline time_t __mktime_internal(struct tm *tp)
{
    /* The maximum number of probes (calls to CONVERT) should be enough
         to handle any combinations of time zone rule changes, solar time,
         leap seconds, and oscillations around a spring-forward gap.
         POSIX.1 prohibits leap seconds, but some hosts have them anyway.  */
//    int remaining_probes = 6;

    /* Time requested.  Copy it in case CONVERT modifies *TP; this can
       occur if TP is localtime's returned value and CONVERT is localtime.  */
    int sec  = tp->tm_sec;
    int min  = tp->tm_min;
    int hour = tp->tm_hour;
    int mday = tp->tm_mday;
    int mon  = tp->tm_mon;
    int year_requested = tp->tm_year;

    /* Ensure that mon is in range, and set year accordingly.  */
    int mon_remainder = mon % 12;
    int negative_mon_remainder = mon_remainder < 0;
    int mon_years = mon / 12 - negative_mon_remainder;
    long_int lyear_requested = year_requested;
    long_int year = lyear_requested + mon_years;

    /* The other values need not be in range:
       the remaining code handles overflows correctly.  */

    /* Calculate day of year from year, month, and day of month.
       The result need not be in range.  */
    int mon_yday = ((__g_mon_yday[__is_leapyear(year)]
                     [mon_remainder + 12 * negative_mon_remainder]) - 1);
    long_int lmday = mday;
    long_int yday  = mon_yday + lmday;

    if (tp->tm_isdst != 0) {
        return -1;
    }

    /* skip LEAP_SECONDS_POSSIBLE &
     * skip NEGATIVE_OFFSET_GUESS
     * skip tm.tm_isdst */
    return (time_t)__do_ydhms_diff(year, yday, hour, min, sec,
                                   EPOCH_YEAR - TM_YEAR_BASE, 0, 0, 0, 0);
}

time_t mktime(struct tm *tp)
{
    if (!tp) {
        return -1;
    }
    return __mktime_internal(tp);
}

char* Strcat(char *dst, const char *src)
{
    char *temp = dst;
    while (*temp != '\0')
        temp++;
    while ((*temp++ = *src++) != '\0');

    return dst;
}

char *asctime(const struct tm *timeptr){
    char* wday_name[7] = {
        "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
    };
    char* mon_name[12] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };
    char* calist_day = wday_name[timeptr->tm_mday];
    char* calist_mon = mon_name[timeptr->tm_mon];
    char* tmp = Strcat(calist_day, Strcat(" ",
    Strcat(calist_mon,Strcat(" ",Strcat(timeptr->tm_year, 
    Strcat(":",Strcat(timeptr->tm_hour, Strcat(":",Strcat(timeptr->tm_min, 
    Strcat(":",Strcat(timeptr->tm_sec, "\0")))))))))));
    return tmp;
}

void gettime(struct tm *timep){
    timep->tm_sec = (int)RTC::Second;
    timep->tm_min = (int)RTC::Minute;
    timep->tm_hour = (int)RTC::Hour;
    timep->tm_mday = (int)RTC::Day;
    timep->tm_mon = (int)RTC::Month;
    timep->tm_year = (int)RTC::Year;

    return;
}