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

/* Tier0 klib 提供的公历秒换算（PIT/RTC 也使用）。 */
extern uint64_t mktime(uint32_t year, uint32_t mon, uint32_t day,
                       uint32_t hour, uint32_t min, uint32_t sec);

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

uint64_t sys_time(uint64_t tloc,GENERATE_IGN5()) {
    IGNV_5();

    RTC::read_rtc();
    int64_t x = mktime(RTC::Year,RTC::Month,RTC::Day,RTC::Hour,RTC::Minute,RTC::Second);
    if (tloc != 0) {
        /* 用户指针必须经 ua 校验/翻译：SmapGuard 只允许内核访问用户页，
           并不校验地址是否落在用户空间，裸解引用等于任意内核地址写。 */
        pagemap_t *pm = Schedule::this_thread()->pagemap;
        if (!VMM::UserAccess::CopyToUser(pm, tloc, &x, sizeof(x)))
            return (uint64_t)((time_t)-1);
    }
    return (uint64_t)x;
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
static int64_t tv_s_off, tv_ms_off;

/* 秒级 RTC + 毫秒级 uptime 拼出 struct timeval；时区无法提供，按 POSIX
   允许忽略 tz（tz 为 NULL 是合法用法，不再拒绝）。 */
uint64_t sys_gettimeofday(uint64_t tv,uint64_t tz, GENERATE_IGN4()) {
    IGNV_4();
    (void)tz;
    if (tv == 0) return (uint64_t)(int64_t)-EFAULT;

    RTC::read_rtc();
    int64_t seconds = (int64_t)mktime(RTC::Year,RTC::Month,RTC::Day,
                                      RTC::Hour,RTC::Minute,RTC::Second);
    int64_t usec = (int64_t)(PIT::TimeSinceBootMS() % 1000ull) * 1000ll - tv_ms_off;
    int64_t sec  = seconds - tv_s_off;
    while (usec < 0)          { usec += 1000000ll; sec -= 1; }
    while (usec >= 1000000ll) { usec -= 1000000ll; sec += 1; }

    struct timeval time_value;
    time_value.tv_sec  = (time_t)sec;
    time_value.tv_usec = (suseconds_t)usec;

    pagemap_t *pm = Schedule::this_thread()->pagemap;
    if (!VMM::UserAccess::CopyToUser(pm, tv, &time_value, sizeof(time_value)))
        return (uint64_t)(int64_t)-EFAULT;
    return 0;
}

uint64_t sys_settimeofday(uint64_t tv,uint64_t tz, GENERATE_IGN4()) {
    IGNV_4();
    (void)tz;
    if (tv == 0) return (uint64_t)(int64_t)-EINVAL;

    struct timeval want;
    pagemap_t *pm = Schedule::this_thread()->pagemap;
    if (!VMM::UserAccess::CopyFromUser(pm, &want, (const void *)tv, sizeof(want)))
        return (uint64_t)(int64_t)-EFAULT;

    /* 内核不改宿主机 RTC，只记录与当前时钟的偏移，gettimeofday 叠加回报。 */
    RTC::read_rtc();
    int64_t seconds = (int64_t)mktime(RTC::Year,RTC::Month,RTC::Day,
                                      RTC::Hour,RTC::Minute,RTC::Second);
    tv_s_off  = seconds - (int64_t)want.tv_sec;
    tv_ms_off = (int64_t)(PIT::TimeSinceBootMS() % 1000ull) * 1000ll
                - (int64_t)want.tv_usec;
    return 0;
}

uint64_t sys_clock_gettime(uint64_t clkid,uint64_t tp, GENERATE_IGN4()) {
    IGNV_4();
    if (tp == 0) return (uint64_t)(int64_t)-EFAULT;

    struct timespec ts = {};
    switch (clkid)
    {
    case CLOCK_REALTIME: {
        RTC::read_rtc();
        ts.tv_sec  = (time_t)mktime(RTC::Year,RTC::Month,RTC::Day,
                                    RTC::Hour,RTC::Minute,RTC::Second);
        ts.tv_nsec = (int64_t)(PIT::TimeSinceBootMS() % 1000ull) * 1000000ll;
        break;
    }
    case CLOCK_MONOTONIC_COARSE:   /* 与 CLOCK_MONOTONIC_RAW 同值 */
    case CLOCK_MONOTONIC: {
        const uint64_t ns = HPET::GetTimeNS();
        ts.tv_sec  = (time_t)(ns / 1000000000ull);
        ts.tv_nsec = (int64_t)(ns % 1000000000ull);
        break;
    }
    default:
        return (uint64_t)(int64_t)-EINVAL;
    }

    pagemap_t *pm = Schedule::this_thread()->pagemap;
    if (!VMM::UserAccess::CopyToUser(pm, tp, &ts, sizeof(ts)))
        return (uint64_t)(int64_t)-EFAULT;
    return 0;
}
