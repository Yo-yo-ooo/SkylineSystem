#include <arch/x86_64/schedule/sched.h>
#include <arch/x86_64/schedule/syscall.h>
#include <klib/errno.h>
#include <elf/elf.h>
#include <mem/pmm.h>
#include <klib/algorithm/queue.h>
#include <arch/x86_64/drivers/hpet/hpet.h>
#include <arch/x86_64/rtc/rtc.h>

static const int32_t CLOCK_REALTIME = 0;
static const int32_t CLOCK_MONOTONIC = 1;
static const int32_t CLOCK_PROCESS_CPUTIME_ID = 2;
static const int32_t CLOCK_THREAD_CPUTIME_ID = 3;
static const int32_t CLOCK_REALTIME_COARSE = 4;
static const int32_t CLOCK_MONOTONIC_RAW = 4;
static const int32_t CLOCK_MONOTONIC_COARSE = 6;
static const int32_t CLOCK_BOOTTIME = 7;
static const int32_t CLOCK_TAI = 11;
static const int32_t CLOCK_BOOTTIME_ALARM = 9;

uint64_t mktime (uint32_t year, uint32_t mon,
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
    IGNV_5();

    RTC::read_rtc();
    int64_t x = mktime(RTC::Year,RTC::Month,RTC::Day,RTC::Hour,RTC::Minute,RTC::Second);
    if(likely((time_t*)tloc == nullptr))
        return x;
    else
        *((time_t*)tloc) = x;
    return (uint64_t)((time_t)-1);
}
struct timeval {
    time_t      tv_sec;     /* seconds */
    suseconds_t tv_usec;    /* microseconds */
};

struct timezone {
    int32_t tz_minuteswest;     /* minutes west of Greenwich */
    int32_t tz_dsttime;         /* type of DST correction */
};
struct timespec
{
    time_t tv_sec;        /* Seconds. */
    int64_t   tv_nsec;       /* Nanoseconds. */
};


/* int gettimeofday(struct timeval *restrict tv,
                 struct timezone *_Nullable restrict tz); */
static int64_t tv_s_off,tv_ms_off;
uint64_t sys_gettimeofday(uint64_t tv,uint64_t tz, uint64_t ign_0, 
uint64_t ign_1,uint64_t ign_2,uint64_t ign_3) {
    IGNV_4();
    uint64_t pv = VMM::Useless::GetPhysicsFlags(Schedule::this_thread()->pagemap,tv);
    uint64_t pz = VMM::Useless::GetPhysicsFlags(Schedule::this_thread()->pagemap,tz);
    if((tv == NULL) || (tz == NULL) || 
        !(pv & MM_READ & MM_WRITE)|| !(pz & MM_READ & MM_WRITE))
        return -EFAULT;
    if(tv_s_off < 0 || (0 <= tv_ms_off && tv_ms_off <= 999999))
        return -EINVAL;
    RTC::read_rtc();
    time_t seconds = mktime(RTC::Year,RTC::Month,RTC::Day,RTC::Hour,RTC::Minute,RTC::Second);
    suseconds_t microseconds = PIT::TimeSinceBootMS() / 1000
                    - (seconds - TIME_SINCE_RTC_INITED_SECOND); 
    struct timeval time_value;
    time_value.tv_sec = seconds - tv_s_off;
    time_value.tv_usec = microseconds - tv_ms_off;
    *((struct timeval*)tv) = time_value;

    return 0;
}

uint64_t sys_settimeofday(uint64_t tv,uint64_t tz, uint64_t ign_0, 
uint64_t ign_1,uint64_t ign_2,uint64_t ign_3) {
    IGNV_4();
    tv_s_off = ((struct timeval*)tv)->tv_sec;
    tv_ms_off = ((struct timeval*)tv)->tv_usec;
    return 0;
}

uint64_t sys_clock_gettime(uint64_t clkid,uint64_t tp, uint64_t ign_0, 
    uint64_t ign_1,uint64_t ign_2,uint64_t ign_3) {
    IGNV_4();
    struct timespec *tp_ = (struct timespec *)tp;
    switch (clkid)
    {
    case CLOCK_MONOTONIC_COARSE:
    case CLOCK_MONOTONIC_RAW:
    case CLOCK_MONOTONIC:
        tp_->tv_sec = HPET::GetTimeNS() / 1000000000;
        tp_->tv_nsec = HPET::GetTimeNS() - tp_->tv_sec * 1000000000;
        break;
    default:
        break;
    }

    return 0;

}
