#include <arch/x86_64/schedule/sched.h>
#include <arch/x86_64/schedule/syscall.h>
#include <klib/errno.h>
#include <elf/elf.h>
#include <mem/pmm.h>
#include <klib/algorithm/queue.h>

#include <arch/x86_64/rtc/rtc.h>

static inline uint64_t mktime (uint32_t year, uint32_t mon,
    uint32_t day, uint32_t hour,
    uint32_t min, uint32_t sec)
{
    if (0 >= (int) (mon -= 2)){    /**//* 1..12 -> 11,12,1..10 */
         mon += 12;      /**//* Puts Feb last since it has leap day */
         year -= 1;
    }
 
    return (((
             (uint64_t) (year/4 - year/100 + year/400 + 367*mon/12 + day) +
             year*365 - 719499
          )*24 + hour /**//* now have hours */
       )*60 + min /**//* now have minutes */
    )*60 + sec; /**//* finally seconds */
}

uint64_t sys_time(uint64_t tloc,uint64_t ign_0, uint64_t ign_1, \
    uint64_t ign_2,uint64_t ign_3,uint64_t ign_4) {
    IGNORE_VALUE(ign_0);IGNORE_VALUE(ign_1);IGNORE_VALUE(ign_2);
    IGNORE_VALUE(ign_3);IGNORE_VALUE(ign_4);

    RTC::read_rtc();
    int64_t x = mktime(RTC::Year,RTC::Month,RTC::Day,RTC::Hour,RTC::Minute,RTC::Second);
    if(likely((time_t*)tloc == nullptr))
        return x;
    else
        *((time_t*)tloc) = x;
    return (uint64_t)((time_t)-1);
}