//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
#include <arch/x86_64/schedule/sched.h>
#include <arch/x86_64/schedule/syscall.h>
#include <klib/errno.h>
#include <elf/elf.h>
#include <mem/pmm.h>
#include <klib/algorithm/queue.h>
#include <arch/x86_64/drivers/hpet/hpet.h>
#include <arch/x86_64/rtc/rtc.h>
#include <arch/x86_64/pit/pit.h>
#include <arch/x86_64/cpu/smap.h>

static const int32_t CLOCK_REALTIME __attribute__((unused)) = 0;
static const int32_t CLOCK_MONOTONIC = 1;
static const int32_t CLOCK_PROCESS_CPUTIME_ID __attribute__((unused)) = 2;
static const int32_t CLOCK_THREAD_CPUTIME_ID __attribute__((unused)) = 3;
static const int32_t CLOCK_REALTIME_COARSE __attribute__((unused)) = 4;
static const int32_t CLOCK_MONOTONIC_RAW __attribute__((unused)) = 4;
static const int32_t CLOCK_MONOTONIC_COARSE __attribute__((unused)) = 6;
static const int32_t CLOCK_BOOTTIME __attribute__((unused)) = 7;
static const int32_t CLOCK_TAI __attribute__((unused)) = 11;
static const int32_t CLOCK_BOOTTIME_ALARM __attribute__((unused)) = 9;

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

uint64_t sys_time(uint64_t tloc,GENERATE_IGN5()) {
    IGNV_5();

    RTC::read_rtc();
    int64_t x = mktime(RTC::Year,RTC::Month,RTC::Day,RTC::Hour,RTC::Minute,RTC::Second);
    if(likely((time_t*)tloc == nullptr))
        return x;
    else { SmapGuard ug; *((time_t*)tloc) = x; }
    return (uint64_t)((time_t)-1);
}
/* 毫秒级单调时钟: 直接返回内核的 PIT::MonotonicMS() (纯 TSC, 从启动起算)。
   为什么需要它: 用户态原本只能拿到**秒级** RTC (SYSCALL_TIME), 秒内的小数
   只能靠自己用 TSC 相对 RTC 秒去校准 —— 实测该校准整体偏大约 1.8 倍,
   导致 DOOM 这类按毫秒算节拍的程序整体慢放。有了这个接口就不需要任何
   用户态校准了。 */
/* 毫秒级睡眠: 走内核的 Schedule::Sleep (定时器轮, 真正让出 CPU)。
   为什么要暴露它: 用户态的等待循环原来只有 sys_yield(), 让出后立刻又会被
   调度回来, 等于换一种方式空转。合成器的 worker 在帧间等待时就是这样一直
   占着 CPU, 实测把重负载客户端 (DOOM) 的启动拖慢一倍。 */
uint64_t sys_sleep_ms(uint64_t ms, GENERATE_IGN5()) {
    IGNV_5();
    if (ms > 0) Schedule::Sleep(ms);
    else        Schedule::Yield();
    return 0;
}

uint64_t sys_monotonic_ms(GENERATE_IGN6()) {
    IGNV_6();
    return (uint64_t)PIT::MonotonicMS();
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
uint64_t sys_gettimeofday(uint64_t tv,uint64_t tz, GENERATE_IGN4()) {
    IGNV_4();
    uint64_t pv = VMM::Internal::GetPageInfo(Schedule::this_thread()->pagemap,tv).flags;
    uint64_t pz = VMM::Internal::GetPageInfo(Schedule::this_thread()->pagemap,tz).flags;
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
    { SmapGuard ug; *((struct timeval*)tv) = time_value; }

    return 0;
}

uint64_t sys_settimeofday(uint64_t tv,uint64_t tz, GENERATE_IGN4()) {
    IGNV_4();
    { SmapGuard ug;   // read the caller's struct timeval
      tv_s_off = ((struct timeval*)tv)->tv_sec;
      tv_ms_off = ((struct timeval*)tv)->tv_usec;
    }
    return 0;
}

uint64_t sys_clock_gettime(uint64_t clkid,uint64_t tp, GENERATE_IGN4()) {
    IGNV_4();
    struct timespec *tp_ = (struct timespec *)tp;
    SmapGuard ug;   // tp_ is a user timespec
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
