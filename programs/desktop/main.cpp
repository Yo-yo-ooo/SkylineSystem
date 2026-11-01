//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT
#include <graphic/fb.h>
#include <syscall.h>


#include <base/arch/x86_64/syscalln.h>
#include <graphic/devtype.h>   /* DEV_TYPE_FRAMEBUFFER: 设备类型编号权威 */
#include <graphic/basicdraw.hpp>
#include <graphic/winstyle.h>
#include <graphic/flanterm.h>
#include <graphic/kbdshare.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>


#include <base/font/ttf/ttf.h>
#include <mouse/ps2.h>
#include <synthesizer/window.h>
#include <synthesizer/wm_client.h>
/* __thread 消除全局缓冲的非重入问题 (合成器 worker 线程
   并行调用 to_string 时互不踩踏) */
static __thread char intTo_stringOutput[128];

uint64_t TLoad(FrameBuffer *Fb, SkyWinPlacement *place);

/* 临时性能埋点 (synthesizer/window.cpp 提供) */
extern "C" void comp_perf_take(uint64_t* compose_cyc, uint64_t* compose_cnt,
                               uint64_t* band_cyc, uint64_t* band_cnt,
                               uint64_t* lock_miss);

/* Caption glyph painter + sized chrome rasterizer, both defined in loader.cpp */
void SkyPaintCaptionIcons(FrameBuffer* s, int32_t bx0, int32_t by0,
                          int32_t bodyW, int32_t titleH, int maximized);
void SkyPaintChromeSized(FrameBuffer* s, int32_t surfW, int32_t surfH,
                         int32_t bodyW, int32_t bodyH, int restoreGlyph,
                         const char* title, uint32_t paperRGB);

/* Pointer shape for an edge-resize direction bitmask (bit0 L,1 R,2 T,3 B):
   corners get the diagonal double-arrow, pure edges the axis-aligned one. */
static uint32_t cursor_shape_for(uint8_t dir) {
    const bool l = (dir & 1) != 0, r = (dir & 2) != 0;
    const bool t = (dir & 4) != 0, b = (dir & 8) != 0;
    if ((l || r) && (t || b)) return SKY_CURSOR_SIZE_DIAG;
    if (l || r)               return SKY_CURSOR_SIZE_HORZ;
    if (t || b)               return SKY_CURSOR_SIZE_VERT;
    return SKY_CURSOR_ARROW;
}

const char *to_string(uint64_t value)
{
    uint8_t i = 0;
    if (value == 0) {
        intTo_stringOutput[i++] = '0';
        intTo_stringOutput[i] = '\0';
        return intTo_stringOutput;
    }
    while (value > 0) {
        intTo_stringOutput[i++] = (value % 10) + '0';
        value /= 10;
    }
    intTo_stringOutput[i] = '\0';
    uint8_t left = 0;
    uint8_t right = i - 1;
    while (left < right) {
        char temp = intTo_stringOutput[left];
        intTo_stringOutput[left] = intTo_stringOutput[right];
        intTo_stringOutput[right] = temp;
        left++;
        right--;
    }
    return intTo_stringOutput;
}
const char *to_string(int64_t value)
{
    if (value < 0) {
        uint64_t u_val = -value;
        const char* num_str = to_string(u_val);
        uint8_t len = 0;
        while (num_str[len] != '\0') len++;
        for (int8_t j = len; j >= 0; j--) {
            intTo_stringOutput[j + 1] = intTo_stringOutput[j];
        }
        intTo_stringOutput[0] = '-';
        return intTo_stringOutput;
    }
    return to_string((uint64_t)value);
}

/* DrawMousePointer extern 已随 bd.cpp 死函数一并删除 */

/* ---- monotonic TSC frame-pacing helpers -----------------------------------
 * The kernel's uptime_ms is only refreshed by the idle thread, which does
 * not run while this main loop is RUNNABLE, so it cannot pace the cursor.
 * Derive cycles/ms directly from CPUID 0x15 (crystal*num/den) with 0x16
 * (base MHz) and a conservative 3 GHz fallback. */
static inline uint64_t rdtsc64() {
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static uint64_t probe_tsc_per_ms() {
    /* 首选**内核用 HPET 校准过**的值 (SysInfo.reserved[0])。
       原来只靠 CPUID 0x15/0x16 猜: QEMU 下这两片经常给出垃圾值, 实测算出
       ~4 cycles/ms (真实 2610428), 于是 scene_gap/clock_gap/idle_gap 全部
       塌成 0 —— 任务栏 (圆角 pill + SDF 文本) 每一轮重活都重绘一遍, 桌面
       直接把一个核烧满, 表现就是"进桌面后整机卡"。 */
    {
        SysInfo* si = (SysInfo*)sys_sysinfo(0);
        if ((int64_t)si >= 0) {
            const uint64_t v = si->reserved[0];
            sys_sysinfo((uint64_t)si);        /* 把映射还给内核 */
            if (v >= 100000ull && v <= 100000000ull) return v;
        }
    }
    uint32_t a, b, c, d;
    a = 0x15; c = 0;                                   /* TSC / crystal ratio */
    __asm__ __volatile__("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                                    : "0"(a), "2"(c) : "memory");
    if (a && b && c) return (uint64_t)c * b / a / 1000u;     /* Hz -> /ms    */
    a = 0x16; c = 0;                                   /* processor base MHz */
    __asm__ __volatile__("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                                    : "0"(a), "2"(c) : "memory");
    if (a) return (uint64_t)a * 1000u;               /* MHz -> cycles/ms      */
    return 3000000u;                                  /* assume 3 GHz         */
}

const char* to_string(char value)
{
    intTo_stringOutput[0] = value;
    intTo_stringOutput[1] = '\0';
    return intTo_stringOutput;
}

/* ========================================================================== */
/*  Window-manager helpers: acrylic taskbar, clock/power tray, geometry,      */
/*  runtime resize surface and 1:1 client-content mirror.                     */
/* ========================================================================== */

/* Calendar fields the tray shows (Win11-style two-line clock). */
/* 整机物理内存总量 (MiB)。用 SysInfo.mem_total —— 静态值, 不需要实际分配
   或触碰任何页面 (以前试过"探测剩余内存", 那要真分配 24 MiB, 在低内存下
   反而把整机拖进换页)。拿不到时返回 0, 调用方按"内存未知"处理。 */
static uint64_t wm_total_mem_mb(void) {
    SysInfo* si = (SysInfo*)sys_sysinfo(0);
    if ((int64_t)si < 0) return 0;
    uint64_t total = si->mem_total;
    sys_sysinfo((uint64_t)si);          /* 把映射还给内核 */
    return total >> 20;
}

struct WmDateTime { int year, month, day, hh, mm; };

/* Integer source-over used for the taskbar's acrylic darkening. */
static inline uint32_t wm_acrylic_pixel(uint32_t wallpaper) {
    const uint32_t k  = SKY_ACRYLIC_KEEP;
    const uint32_t ik = 255u - k;
    uint32_t wr = (wallpaper >> 16) & 0xFF, wg = (wallpaper >> 8) & 0xFF, wb = wallpaper & 0xFF;
    uint32_t br = (SKY_ACRYLIC_BASE >> 16) & 0xFF,
             bg = (SKY_ACRYLIC_BASE >> 8)  & 0xFF, bb = SKY_ACRYLIC_BASE & 0xFF;
    uint32_t r = (wr * k + br * ik) / 255u;
    uint32_t g = (wg * k + bg * ik) / 255u;
    uint32_t b = (wb * k + bb * ik) / 255u;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

/* Paint the bottom taskbar straight into the wallpaper bitmap (bottom-most
   compositor layer). `cleanBar` is the pristine wallpaper strip; it is
   restored first so repeatedly repainting (clock tick / state change) never
   compounds the acrylic darkening. appState: 0 idle (shown), 1 active
   (minimized), 2 closed (no app entry). */
/* One taskbar pill geometry. Shared by drawing and click hit-testing so the
   click areas can never drift from what is painted. */
struct TBRect {
    int32_t x, w;
    int id, state;          /* id: 0=控制台, >0 客户端序号; -1=溢出指示 */
    const char* title;
    char    text[40];       /* 实际绘制用的（可能被省略号截断的）标题 */
};

/* Lay out pills left-to-right: console first (unless closed), then every
   generic client in spawn order. state: 0 shown, 1 minimized. */
/* 任务栏药丸的最小/最大宽度，以及右侧给时钟预留的宽度。
   原来宽度 = 文字宽 + 34（下限 96），于是 "Skyline Console" 这种长标题
   一个就占 ~200px，开三个窗口就把整条栏塞满、时钟被挤到角落。 */
#define TB_MIN_W        72
#define TB_MAX_W        200
#define TB_GAP          8
#define TB_LEFT_MARGIN  10
#define SKY_TRAY_RESERVE 104

/* 按**字符数**截断（放不下时末尾补 "..."）。
   为什么不用 TTF_GetTextSize 量像素：实测它对同样的字符串报出的宽度
   与实际绘制的宽度相差很大（"Notepad" 报 >166px，实际只有约 70px），
   照它定宽/截断会把三个药丸全顶到上限、标题被截成 "Not..."。
   用固定平均字宽估算既确定又不会溢出。 */
#define TB_CHAR_W 9
static void tb_elide(const char* src, int32_t maxChars, char* out, size_t outsz) {
    if (!out || outsz < 5) return;
    if (!src) { out[0] = 0; return; }
    int32_t n = 0;
    if (maxChars < 1) maxChars = 1;
    while (src[n] && n < maxChars && n < (int32_t)outsz - 1) {
        out[n] = src[n];
        ++n;
    }
    out[n] = 0;
    if (src[n]) {                       /* 还有剩余 -> 加省略号 */
        if (n > (int32_t)outsz - 5) n = (int32_t)outsz - 5;
        if (n < 0) n = 0;
        out[n] = '.'; out[n+1] = '.'; out[n+2] = '.'; out[n+3] = 0;
    }
}

/* 布局药丸：从左到右，控制台在最前（未关闭时），然后按 spawn 顺序的客户端。
   availW 是药丸区可用宽度（已扣除左边距和右侧时钟预留）。
   三层自适应，保证任何情况下都不会盖住时钟、也不会画出屏幕：
     1) 自然宽度（文字 + 34），各自限制在 [TB_MIN_W, TB_MAX_W]；
     2) 总宽超出可用宽 -> 所有药丸取等宽（仍不低于 TB_MIN_W），标题用省略号截断；
     3) 等宽也放不下 -> 只保留放得下的若干个，最后一个显示 "+N" 表示其余窗口。 */
static int tb_layout(TBRect* r, int cap, int consoleHidden, TTF_Font* f,
                     int32_t availW) {
    if (!r || cap <= 0) return 0;

    /* --- 收集条目（控制台 + 客户端） --- */
    struct { const char* title; int id, state; } src[24];
    int ns = 0;
    if (!consoleHidden) {
        src[ns].title = "Skyline Console"; src[ns].id = 0; src[ns].state = 0; ns++;
    }
    int cc = wm_client_count();
    for (int i = 0; i < cc && ns < 24; i++) {
        WmClient* c = wm_client_by_order(i);
        if (!c) continue;
        src[ns].title = c->title; src[ns].id = i + 1;
        src[ns].state = c->minimized ? 1 : 0;
        ns++;
    }
    if (ns == 0) return 0;

    /* --- 1) 自然宽度（按字符数估算，见 tb_elide 注释） --- */
    (void)f;
    int32_t want[24];
    int64_t total = 0;
    for (int i = 0; i < ns; i++) {
        int32_t len = (int32_t)strlen(src[i].title);
        int32_t pw = len * TB_CHAR_W + 34;
        if (pw < TB_MIN_W) pw = TB_MIN_W;
        if (pw > TB_MAX_W) pw = TB_MAX_W;
        want[i] = pw;
        total += pw + (i ? TB_GAP : 0);
    }

    int n = ns;                     /* 实际要放几个 */
    int32_t w = 0;                  /* 统一宽度（0 表示用各自自然宽） */

    if (total > availW) {
        /* --- 2) 等宽压缩 --- */
        int32_t uni = (availW - TB_GAP * (ns - 1)) / ns;
        if (uni >= TB_MIN_W) {
            w = uni;
            if (w > TB_MAX_W) w = TB_MAX_W;
        } else {
            /* --- 3) 放不下就少放几个，末尾留一个 "+N" 指示 --- */
            int32_t keep = (availW + TB_GAP) / (TB_MIN_W + TB_GAP);
            if (keep < 1) keep = 1;
            if (keep > ns) keep = ns;
            /* 留一格给 "+N"（除非全部放得下） */
            int overflow = ns - keep;
            if (overflow > 0) {
                if (keep > 1) keep -= 1;
                overflow = ns - keep;
            }
            n = keep + (overflow > 0 ? 1 : 0);
            w = TB_MIN_W;
        }
    }

    /* --- 落位 + 截断标题 --- */
    int32_t x = TB_LEFT_MARGIN;
    for (int i = 0; i < n && i < cap; i++) {
        int32_t pw = (w > 0) ? w : want[i];
        if (i == n - 1 && n < ns) {
            /* 溢出指示药丸 */
            int rest = ns - i;
            r[i].x = x; r[i].w = pw; r[i].id = -1; r[i].state = 0;
            r[i].text[0] = '+';
            if (rest >= 10) { r[i].text[1] = (char)('0' + rest / 10); r[i].text[2] = (char)('0' + rest % 10); r[i].text[3] = 0; }
            else { r[i].text[1] = (char)('0' + rest); r[i].text[2] = 0; }
            r[i].title = r[i].text;
        } else {
            r[i].x = x; r[i].w = pw; r[i].id = src[i].id; r[i].state = src[i].state;
            tb_elide(src[i].title, (pw - 34) / TB_CHAR_W, r[i].text, sizeof(r[i].text));
            r[i].title = r[i].text;
        }
        x += pw + TB_GAP;
    }
    return (n < cap) ? n : cap;
}

static void wm_draw_taskbar(uint32_t* wall, const uint32_t* cleanBar,
                            uint32_t W, uint32_t H, int appState,
                            const WmDateTime* dt, bool haveClock) {
    if (!wall || !cleanBar) return;   /* cleanBar malloc failure guard */
    FrameBuffer lb;
    lb.BaseAddress       = wall;
    lb.BufferSize        = (uint64_t)W * H * sizeof(uint32_t);
    lb.Width = lb.PixelsPerScanLine = W;
    lb.Height            = H;

    const uint32_t barH = SKYWIN_TASKBAR_H;
    const uint32_t y0   = H - barH;

    /* 1) restore the pristine wallpaper strip, then darken it in place so the
          wallpaper glows through (Win11 acrylic / macOS translucency). */
    memcpy(wall + (uint64_t)y0 * W, cleanBar, (size_t)W * barH * sizeof(uint32_t));
    uint32_t* stripe = wall + (uint64_t)y0 * W;
    for (uint32_t i = 0; i < W * barH; i++) stripe[i] = wm_acrylic_pixel(stripe[i]);
    DrawFillRect(&lb, 0, y0, W, 1, SKYRGB_TB_HILITE);   /* luminous top edge  */

    /* 2) left: one rounded pill per open window (console + generic clients) */
    TTF_Font* f = console_font();
    TBRect rects[12];
    const int32_t pillAvail = (int32_t)W - TB_LEFT_MARGIN - SKY_TRAY_RESERVE;
    int ne = tb_layout(rects, 12, appState == 2, f, pillAvail);
    const int32_t py = (int32_t)y0 + 7, ph = (int32_t)barH - 14;
    for (int i = 0; i < ne; i++) {
        const TBRect* t = &rects[i];
        BasicDraw pill(&lb);
        const bool on = (t->state == 1);
        const uint32_t face = on ? SKYRGB_TBTN_ON : SKYRGB_TBTN_IDLE;
        pill.DrawRoundedRect(t->x, py, t->w, ph, 7, face, true);
        pill.DrawRoundedRect(t->x, py, t->w, ph, 7, on ? SKYRGB_ACCENT : SKYRGB_BORDER, false);
        if (on) DrawFillRect(&lb, t->x + 2, py + 5, 3, ph - 10, SKYRGB_ACCENT);
        if (f)
            TTF_DrawText(&lb, f, t->x + 16, py + (ph - 22) / 2,
                         t->title, on ? 0xFFFFFFFFu : SKYRGB_INK);
    }

    /* 3) right tray: Win11-style two-line clock, right aligned */
    TTF_Font* tf = console_font();
    char line1[8] = "", line2[16] = "";
    if (haveClock && dt) {
        line1[0] = (char)('0' + dt->hh / 10); line1[1] = (char)('0' + dt->hh % 10);
        line1[2] = ':';
        line1[3] = (char)('0' + dt->mm / 10); line1[4] = (char)('0' + dt->mm % 10);
        line1[5] = '\0';

        /* second line: YYYY/M/D (unpadded month/day, Win11 CJK locale look) */
        int n = 0;
        const char* ys = to_string((uint64_t)dt->year);
        while (*ys) line2[n++] = *ys++;
        line2[n++] = '/';
        if (dt->month >= 10) { line2[n++] = '1'; line2[n++] = (char)('0' + dt->month - 10); }
        else line2[n++] = (char)('0' + dt->month);
        line2[n++] = '/';
        if (dt->day >= 10) { line2[n++] = (char)('0' + dt->day / 10); line2[n++] = (char)('0' + dt->day % 10); }
        else line2[n++] = (char)('0' + dt->day);
        line2[n] = '\0';
    }

    /* 时钟的宽度用**字符数估算**, 不再调用 TTF_GetTextSize。
       原因: 该函数在字体被绘制过一帧之后就会返回放大约 11 倍的值
       (实测同一字符串: 第一帧 33/72, 之后恒定 371/389, 行高 18→71),
       于是时钟被画到屏幕中间并截断成 ": 5" —— 就是"时间不显示"。
       实测比例约 7px/字符 ("11:10"≈33px、"2026/10/25"≈72px), 用它右对齐
       既准确又不依赖那个会漂移的状态。 */
    /* 时钟度量: 只在**第一次**真正量一次, 之后一直复用。
       原因: TTF_GetTextSize 在第一帧返回正确值 (实测 "11:07"=33px, 行高 18),
       但字体被绘制过一帧之后, 同样的字符串会返回 371px / 行高 71 ——
       lazy glyph 窗口 (ttf_glyf_load) 会就地改写字体数据, 度量随之失真。
       把唯一正确的那次结果缓存下来, 位置就永远是对的。 */
    static int32_t s_w1 = 0, s_w2 = 0, s_th = 0;
    if (s_th == 0 && tf && haveClock) {
        int32_t t1 = 0, t2 = 0, h1 = 0, h2 = 0;
        TTF_GetTextSize(tf, line1, &t1, &h1);
        TTF_GetTextSize(tf, line2, &t2, &h2);
        /* 合理性检查: 只接受"像样"的度量, 荒谬值 (字体状态已坏) 直接丢弃 */
        if (t1 > 0 && t1 < 120 && t2 > 0 && t2 < 200 && h1 > 6 && h1 < 40) {
            s_w1 = t1; s_w2 = t2;
            s_th = h1;
        }
    }
    int32_t w1 = s_w1, w2 = s_w2, th = s_th ? s_th : 15;
    int32_t blockW = w1 > w2 ? w1 : w2;
    int32_t blockH = 2 * th + 2;
    int32_t right  = (int32_t)W - (int32_t)SKY_TRAY_MARGIN;
    int32_t blockTop = (int32_t)y0 + ((int32_t)barH - blockH) / 2;
    (void)blockW;
    if (tf && haveClock) {
        TTF_DrawText(&lb, tf, right - w1, blockTop,             line1, SKYRGB_TRAY_INK);
        TTF_DrawText(&lb, tf, right - w2, blockTop + th + 2,    line2, SKYRGB_TRAY_INK);
    }
}

/* Convert the UTC epoch seconds from SYSCALL_TIME into local civil date/time,
   applying the fixed timezone offset. Day conversion is the Howard Hinnant
   civil-from-days algorithm (era/400-year cycle, no per-year loop). */
static bool wm_read_datetime(WmDateTime* dt) {
    int64_t s = (int64_t)syscall(SYSCALL_TIME, 0, 0, 0, 0, 0, 0);
    if (s < 0) return false;
    int64_t ls = s + (int64_t)SKY_LOCAL_TZ_MIN * 60;
    int64_t days = ls / 86400;
    int64_t rem  = ls % 86400;
    if (rem < 0) { rem += 86400; days -= 1; }
    dt->hh = (int)(rem / 3600);
    dt->mm = (int)((rem / 60) % 60);

    int64_t d   = days + 719468;
    int64_t era = (d >= 0 ? d : d - 146096) / 146097;
    int64_t doe = d - era * 146097;
    int64_t y   = (doe - doe / 1460 + doe / 36524 - doe / 146097) / 365;
    int64_t doy = doe - (365 * y + y / 4 - y / 100);
    int64_t mp  = (5 * doy + 2) / 153;
    dt->day   = (int)(doy - (153 * mp + 2) / 5 + 1);
    dt->month = (int)(mp < 10 ? mp + 3 : mp - 9);
    dt->year  = (int)(y + era * 400 + (dt->month <= 2 ? 1 : 0));
    return true;
}

/* Switch the console window to its rounded NORMAL presentation. When the body
   is still the fixed SKYWIN_W x H it uses the zero-copy shared client surface;
   once it has been resized it uses the WM-owned fixed-pitch rzSurf. Called only
   between synchronous Compose() frames (workers parked on the barrier). */
static void wm_apply_normal(Window* w, const SkyWinPlacement* pl,
                           uint32_t nx, uint32_t ny, uint32_t nw, uint32_t nh,
                           const uint32_t* rzSurf, int32_t rzPitch) {
    w->PosX = nx;  w->PosY = ny;
    if (rzSurf && (nw != SKYWIN_W || nh != SKYWIN_H)) {
        w->SizeX = (uint32_t)rzPitch;
        w->SizeY = nh + 2u * SKYWIN_SHADOW;
        w->FbAddr = (uint64_t)rzSurf;
        w->HasAlpha = 1;
    } else {
        w->SizeX = SKYWIN_SURF_W;
        w->SizeY = SKYWIN_SURF_H;
        w->FbAddr = pl->desk_surf;
        w->HasAlpha = 1;
    }
}

/* Switch the console window to the opaque, full-work-area MAXIMIZED surface. */
static void wm_apply_max(Window* w, const uint32_t* maxSurf,
                        uint32_t mw, uint32_t mh) {
    w->PosX = 0;  w->PosY = 0;
    w->SizeX = mw;  w->SizeY = mh;
    w->FbAddr = (uint64_t)maxSurf;
    w->HasAlpha = 0;
}

/* Mirror the client's fixed terminal bitmap 1:1 into a WM-owned presentation
   surface. No scaling, so glyphs stay crisp: the visible content is clipped to
   CONTENT_W x CONTENT_H (a smaller window crops it, a larger one leaves the
   rest as the already-painted black paper). */
static void wm_mirror_content(uint32_t* dst, int32_t dstPitch,
                              int32_t dx, int32_t dy,
                              int32_t bodyW, int32_t bodyH,
                              const uint32_t* normSurf) {
    int32_t cw = bodyW;
    if (cw > (int32_t)SKYWIN_CONTENT_W) cw = (int32_t)SKYWIN_CONTENT_W;
    int32_t ch = bodyH - (int32_t)SKYWIN_TITLE_H - (int32_t)SKYWIN_RADIUS;
    if (ch > (int32_t)SKYWIN_CONTENT_H) ch = (int32_t)SKYWIN_CONTENT_H;
    if (cw <= 0 || ch <= 0) return;
    for (int32_t r = 0; r < ch; r++) {
        const uint32_t* sp = normSurf
            + (uint64_t)((int32_t)SKYWIN_CONTENT_Y + r) * SKYWIN_SURF_W
            + SKYWIN_CONTENT_X;
        uint32_t* dp = dst + (uint64_t)(dy + r) * (uint32_t)dstPitch + dx;
        memcpy(dp, sp, (size_t)cw * sizeof(uint32_t));
    }
}

/* (Re)paint the fixed-pitch resize surface for a body of nw x nh: clear the
   valid rows so a shrink leaves no stale edge, rasterize the rounded chrome at
   the new size, then mirror the latest client text. */
static void wm_rebuild_resize(uint32_t* rzSurf, int32_t rzPitch,
                              int32_t nw, int32_t nh, const uint32_t* normSurf) {
    const int32_t M  = (int32_t)SKYWIN_SHADOW;
    const int32_t rows = nh + 2 * M;
    memset(rzSurf, 0, (size_t)rzPitch * (uint32_t)rows * sizeof(uint32_t));
    FrameBuffer rb;
    rb.BaseAddress       = rzSurf;
    rb.BufferSize        = (uint64_t)rzPitch * rows * sizeof(uint32_t);
    rb.Width = rb.PixelsPerScanLine = rzPitch;
    rb.Height            = rows;
    SkyPaintChromeSized(&rb, rzPitch, rows, nw, nh, 0, "Skyline Console",
                        SKYRGB_PAPER);
    wm_mirror_content(rzSurf, rzPitch, M, M + (int32_t)SKYWIN_TITLE_H,
                      nw, nh, normSurf);
}

/* Cheap LIVE preview painted on every drag step: flat rectangles, square
   corners, caption glyphs and the 1:1 client mirror, but NO per-pixel SDF and
   NO soft shadow. The full rounded/shadowed chrome is rasterized exactly once
   when the drag ends (wm_rebuild_resize). Keeping the drag frame O(area) with
   only linear fills prevents the heavy SDF raster (which on a slow/emulated
   core starves the PS/2 IRQ and drops movement packets, so the drag would
   lag its own cursor) and matches how mainstream WMs show a lightweight
   outline while resizing. */
static void wm_paint_chrome_live(uint32_t* rzSurf, int32_t rzPitch,
                                 int32_t nw, int32_t nh,
                                 const uint32_t* normSurf) {
    const int32_t M  = (int32_t)SKYWIN_SHADOW;
    const int32_t TH = (int32_t)SKYWIN_TITLE_H;
    const int32_t rows = nh + 2 * M;
    memset(rzSurf, 0, (size_t)rzPitch * (uint32_t)rows * sizeof(uint32_t));
    FrameBuffer lb;
    lb.BaseAddress       = rzSurf;
    lb.BufferSize        = (uint64_t)rzPitch * rows * sizeof(uint32_t);
    lb.Width = lb.PixelsPerScanLine = rzPitch;
    lb.Height            = rows;
    DrawFillRect(&lb, M, M, nw, TH, SKYRGB_TITLE);                 /* title bar */
    DrawFillRect(&lb, M, M + TH, nw, nh - TH, SKYRGB_PAPER);       /* paper     */
    DrawFillRect(&lb, M, M + TH - 1, nw, 1, SKYRGB_SEP);           /* separator */
    DrawRect(&lb, M, M, nw, nh, SKYRGB_BORDER);                    /* hair frame*/
    SkyPaintCaptionIcons(&lb, M, M, nw, TH, 0);                    /* glyphs    */
    wm_mirror_content(rzSurf, rzPitch, M, M + TH, nw, nh, normSurf);
}


/* ==========================================================================
   光标线程 (鼠标即图层: 由它独立服务)
   为什么单独开线程: 主循环一次"重活"(整屏合成 10~19ms + 窗口交互/轮询 ~17ms)
   期间完全无法采样鼠标, 指针就会一顿一顿。光标本身只需要重绘它覆盖的那 16 行
   (Compositor::CursorMoveTo), 亚毫秒级 —— 交给独立线程后, 鼠标响应不再取决于
   主线程何时有空。
   并发安全: 所有会写扫描出缓冲或遍历图层/窗口链表的入口都在 Compositor 里
   加了扫描出锁 (CompScanGuard), 光标线程与主线程互斥。
   ========================================================================== */
static volatile ps2_mouse_state_t* g_mouse_page;

extern "C" void CursorThreadEntry() {
    int32_t lx = -100000, ly = -100000;
    Compositor& comp = Compositor::Get();
    /* 连续抢锁失败计数。
       "非阻塞丢帧"是对的 (不能自旋占核), 但**不能无限丢**: 主线程一次整屏
       合成要 ~20ms, TryLockScan 只有几微秒的窗口, 密集移动时几乎每帧都失败,
       结果就是光标画不上去 —— 用户看到"鼠标卡死"。
       连续失败到阈值就退化为阻塞版 CursorMoveTo: 它走 LockScan (有界自旋
       + sys_yield 让出), 既不会把核占死, 也保证光标一定被画出来。 */
    uint32_t miss_run = 0;
    for (;;) {
        volatile ps2_mouse_state_t* mp = g_mouse_page;
        if (mp) {
            uint32_t s1, s2;
            int32_t x = 0, y = 0;
            do {
                s1 = __atomic_load_n(&mp->seq, __ATOMIC_ACQUIRE);
                if (s1 & 1) continue;
                x = mp->x; y = mp->y;
                s2 = __atomic_load_n(&mp->seq, __ATOMIC_ACQUIRE);
            } while (s1 != s2);

            if (x < 0) x = 0;
            if (y < 0) y = 0;
            if (x > 16000) x = 16000;
            if (y > 16000) y = 16000;
            if (x != lx || y != ly) {
                /* 非阻塞: 主线程正在整屏合成时抢不到锁就丢掉这次采样,
                   2ms 后自然重试。**不能**在这里阻塞等锁 —— 自旋会把本核
                   占死, 持锁的主线程反而上不了 CPU, 就是"DOOM 跑一半鼠标
                   卡死"的成因。lx/ly 只在真正画成后推进, 丢采样不会丢位置。 */
                if (comp.CursorMoveToTry(x, y)) {
                    lx = x; ly = y; miss_run = 0;
                } else if (++miss_run >= 4u) {
                    /* 连续 4 次 (≈8ms) 都没抢到 —— 主线程正在长合成。
                       退化为阻塞版, 保证这一次移动被真正画出来。 */
                    comp.CursorMoveTo(x, y);
                    lx = x; ly = y; miss_run = 0;
                }
            }
        }
        /* 2ms 采样 (=500Hz); 真正的上限是鼠标 200Hz 采样率 */
        syscall(SYSCALL_SLEEP_MS, 2, 0, 0, 0, 0, 0);
    }
}

int main(){
    const uint64_t boot_t0 = rdtsc64();
    /* 第一件事: 打开串口自检通道。用固定字符串 (不经过 snprintf), 排除
       "格式化函数本身出问题"导致日志缺失的可能。 */
    {
        static const char probe[] = "[desktop] main reached, start probe\n";
        syscall(SYS_DBGSOUT, (uint64_t)probe, (uint64_t)(sizeof(probe) - 1),
                0, 0, 0, 0);
    }

    /* ---- 帧缓冲初始化 -------------------------------------------------
       MapFB() 走 sys_dev_mmap: 内核把 FB 映射进本进程并返回目标 VA
       (内核日志 "Framebuffer mapped to VADDR: 0x..." 即此); 几何信息走
       GetFBInfo() 的 sys_dev_ioctl。
       这两条路径曾因为系统调用返回值根本回不到用户态而返回全 0, 桌面于是
       在 "framebuffer info failed" 上自杀 —— 根因是 user.asm 收尾写的是
       %rax 而不是帧槽 (见 docs/debug-hang-2026-10-11.md 第 11 节), 已修。
       这里保留逐个字段的兜底: 万一某个字段为 0, 用 ioctl 再取一次。 */
    FrameBuffer fb = GetFBInfo();
    fb.BaseAddress = (void*)MapFB();
    if (fb.Width == 0)
        fb.Width = syscall(SYSCALL_DEV_IOCTL, DEV_TYPE_FRAMEBUFFER, 0, 4, 0, 0, 0);
    if (fb.Height == 0)
        fb.Height = syscall(SYSCALL_DEV_IOCTL, DEV_TYPE_FRAMEBUFFER, 0, 2, 0, 0, 0);
    if (fb.PixelsPerScanLine == 0)
        fb.PixelsPerScanLine = syscall(SYSCALL_DEV_IOCTL, DEV_TYPE_FRAMEBUFFER, 0, 3, 0, 0, 0);
    if (fb.BufferSize == 0)
        fb.BufferSize = syscall(SYSCALL_DEV_IOCTL, DEV_TYPE_FRAMEBUFFER, 0, 1, 0, 0, 0);
    uint64_t FbAddr = (uint64_t)fb.BaseAddress;

    /* 启动自检日志: 桌面进程一旦"自己退了", 日志必须说清是哪一步。
       (实测: PROC 7 曾以 code=1 静默退出, 内核只知道"它调了 sys_exit(1)",
       不知道是 malloc 失败还是 compositor 初始化失败 —— 靠这里点名。) */
    char boot_msg[192];
    int bn = snprintf(boot_msg, sizeof(boot_msg),
                      "[desktop] start: fb=%llx %llux%llu psl=%llu bufsz=%llu\n",
                      (unsigned long long)FbAddr,
                      (unsigned long long)fb.Width,
                      (unsigned long long)fb.Height,
                      (unsigned long long)fb.PixelsPerScanLine,
                      (unsigned long long)fb.BufferSize);
    if (bn > 0) syscall(SYS_DBGSOUT, (uint64_t)boot_msg, (uint64_t)bn, 0, 0, 0, 0);

    /* 所有 "启动失败 -> return 1" 的地方统一走这个报告器: 之前静默退出
       让"桌面为什么没了"完全无法从日志回答。 */
    auto boot_fatal = [&](const char *what, unsigned long long bytes) -> int {
        int n = snprintf(boot_msg, sizeof(boot_msg),
                         "[desktop] FATAL: %s failed (bytes=%llu, %llux%llu) -> exit\n",
                         what, bytes, (unsigned long long)fb.Width,
                         (unsigned long long)fb.Height);
        if (n > 0) syscall(SYS_DBGSOUT, (uint64_t)boot_msg, (uint64_t)n, 0, 0, 0, 0);
        return 1;
    };

    uint32_t scrW = (uint32_t)fb.Width;
    uint32_t scrH = (uint32_t)fb.Height;
    if (scrW == 0 || scrH == 0 || FbAddr == 0)
        return boot_fatal("framebuffer info", (unsigned long long)FbAddr);

    size_t wallBytes = (size_t)scrW * scrH * sizeof(uint32_t);
    uint32_t* wallBuf = (uint32_t*)malloc(wallBytes);
    if (wallBuf == nullptr) {
        /* 降级而不是退出: 壁纸/合成缓冲各约 8MB@1080p, 一次分配失败不应
           让唯一的窗口管理器整体消失。这里退到 1/4 面积再试一次。 */
        wallBuf = (uint32_t*)malloc((wallBytes = wallBytes / 4));
        if (wallBuf == nullptr)
            return boot_fatal("wallpaper malloc", (unsigned long long)wallBytes);
    }

    const uint32_t barH = SKYWIN_TASKBAR_H;

    BasicDraw bd((FrameBuffer*)&fb);
    bd.RenderWallpaper(wallBuf);

    /* Pristine copy of just the taskbar strip, so the acrylic layer can be
       repainted every minute without darkening on top of itself. */
    uint32_t* cleanBar = (uint32_t*)malloc((size_t)scrW * barH * sizeof(uint32_t));
    if (cleanBar)
        memcpy(cleanBar, wallBuf + (uint64_t)(scrH - barH) * scrW,
               (size_t)scrW * barH * sizeof(uint32_t));

    Compositor& comp = Compositor::Get();
    if (!comp.Init((FrameBuffer*)&fb)) {
        /* 同样先降级: compositor 的后台缓冲是 malloc 出来的, 失败时
           报告具体字节数, 便于判断是"内存不够"还是"参数非法"。 */
        return boot_fatal("Compositor::Init",
                          (unsigned long long)((uint64_t)scrW * scrH * 4u));
    }

    /* 把壁纸交给合成器当背景层。原来没人调用 SetBackground, 于是每一行都
       先 memset 成黑色、再把壁纸窗口整行 memcpy 上去 —— 每帧白扔一整屏
       (1280x800x4 ≈ 4 MB) 的黑写 + 一次多余拷贝。 */
    comp.SetBackground(wallBuf);

    /* 任务栏行带永远重画: wm_draw_taskbar 是就地改写壁纸缓冲, 增量合成
       必须知道这块区域每帧都可能变 (否则时钟停在旧时间)。 */
    comp.SetDynamicRows((int32_t)(scrH - barH), (int32_t)scrH);

    /* Start compositor workers BEFORE spawning any client app: the other
       cores are idle now, so the pinned workers execute their entry ack
       immediately. The workers just spin until the first frame once windows
       are registered. */
    comp.StartWorkers();

    /* 启动自检: 确认 spawn worker 之后主线程的 TCB 自指针 ([fs:0] 就是运行期
       的 %fs 值) 没有被破坏。
       背景: 动态链接调试期间, 桌面曾在 StartWorkers 之后崩在 malloc 的
       `%fs` 访问上; 真正原因是内核侧 TPOFF64 公式算错 (见
       docs/dynamic-linking-plan.md), 但这条日志在定位过程中很有用,
       留下来作为回归检查 —— 它每进程只打一行。 */
    {
        uint64_t fsv = 0;
        __asm__ volatile("movq %%fs:0, %0" : "=r"(fsv));
        char m[160];
        int k = snprintf(m, sizeof(m),
                         "[desktop] after StartWorkers: fs(tcb)=0x%llx\n",
                         (unsigned long long)fsv);
        if (k > 0) syscall(SYS_DBGSOUT, (uint64_t)m, (uint64_t)k, 0, 0, 0, 0);
    }

    bn = snprintf(boot_msg, sizeof(boot_msg),
                  "[desktop] compositor up, workspace allocated; spawning console\n");
    if (bn > 0) syscall(SYS_DBGSOUT, (uint64_t)boot_msg, (uint64_t)bn, 0, 0, 0, 0);

    WmDateTime bootDT;
    bool haveClock = wm_read_datetime(&bootDT);
    if (cleanBar)
        wm_draw_taskbar(wallBuf, cleanBar, scrW, scrH, 0, &bootDT, haveClock);

    static Window wallpaperWin;
    wallpaperWin.PosX = wallpaperWin.PosY = 0;
    wallpaperWin.SizeX = scrW;
    wallpaperWin.SizeY = scrH;
    wallpaperWin.FrameStartX = wallpaperWin.FrameStartY = 0;
    wallpaperWin.FrameEndX = scrW;
    wallpaperWin.FrameEndY = scrH;
    wallpaperWin.FbAddr = (uint64_t)wallBuf;

    static SkyWinPlacement place;
    uint64_t consoleSurf = TLoad(&fb, &place);

    /* ---- auto layers: registration order == initial stacking order -----
       Wallpaper first (bottom), console next, then every spawned client —
       no hand-numbered z values anywhere. The console keeps a layer handle
       only for the topmost-hit z comparison in the input loop. */
    comp.RegisterWindowAuto(&wallpaperWin);
    CompLayer* consoleLayer = nullptr;

    static Window consoleWin;
    if (consoleSurf && place.desk_surf) {
        consoleWin.PosX = place.x;
        consoleWin.PosY = place.y;
        consoleWin.SizeX = place.w;
        consoleWin.SizeY = place.h;
        consoleWin.FrameStartX = SKYWIN_CONTENT_X;
        consoleWin.FrameStartY = SKYWIN_CONTENT_Y;
        consoleWin.FrameEndX   = SKYWIN_CONTENT_X + SKYWIN_CONTENT_W;
        consoleWin.FrameEndY   = SKYWIN_CONTENT_Y + SKYWIN_CONTENT_H;
        consoleWin.FbAddr = place.desk_surf;
        consoleWin.HasAlpha = 1;   /* rounded corners + soft drop shadow */
        consoleLayer = comp.RegisterWindowAuto(&consoleWin);
    }

    /* ---- generic windowed clients ---------------------------------------
       Notepad is down to spawn PARAMETERS now: title, body size, paper
       colour, stacking offset. Caption drag / close / click-to-front /
       dead-process cleanup are app-agnostic (wm_client.cpp); adding
       another windowed app is one more wm_client_spawn() line. */
    wm_client_spawn(comp, &fb, "/mp/notepad.elf", "Notepad",
                    560u, 420u, 0xFF0F0F12u, 90u, 70u);

    /* DOOM (PureDOOM shim). The game is only spawned when an IWAD is
       actually present at /mp/doom.wad (registered) or /mp/doom1.wad
       (shareware) — otherwise the client would I_Error() and exit during
       boot. Body 640x440 leaves the WM a 640x400 content area, i.e. exactly
       2x DOOM's fixed 320x200 framebuffer. Sound and music are hidden:
       there is no audio driver. */
    {
        FILE* wad = fopen("/mp/doom.wad", "rb");
        if (!wad) wad = fopen("/mp/doom1.wad", "rb");
        if (wad) {
            fclose(wad);
            /* DOOM 的峰值需求约 25-30 MiB (ELF + 4 MiB zone + 运行时堆)。
               在 64 MiB 的目标机上, 桌面 + 控制台 + 记事本本身只用约 30 MiB,
               整机零 OOM、完全正常; 一旦再拉起 DOOM, 内核就开始连环 OOM,
               DOOM 自己也会因为读盘失败 (lwext4 拿不到块缓冲) 中途退出。
               实测: 不含 DOOM 时 64 MiB 与 56 MiB 都是 OOM=0、无崩溃;
               含 DOOM 需要 84 MiB 才稳定。

               所以默认**不自动启动** DOOM。需要 DOOM 时把下面这个开关改成 1
               (相应地整机内存请给到 84 MiB 以上)。
               注: 曾经尝试过"运行时探测剩余内存再决定", 但那需要在低内存下
               真的分配并触碰 24 MiB, 会把整机拖进剧烈换页 (64 MiB 下
               OOM 4806 次), 反而更糟 —— 故改回编译期开关。 */
#define DESKTOP_ENABLE_DOOM 1
            /* 运行时按**内存总量**决定是否拉起 DOOM, 而不是编译期写死:
               64 MiB 目标机跳过 (否则内核连锁 OOM, DOOM 也会读盘失败),
               >=84 MiB 正常启动。同一份镜像在两种配置下都对, 无需改代码重编。
               阈值 84 MiB 来自实测 (72 MiB 时 37 次 OOM, 96 MiB 完全干净)。 */
            const uint64_t memMB = wm_total_mem_mb();
            if (!DESKTOP_ENABLE_DOOM || (memMB && memMB < 84)) {
                char msg[96];
                int mn = snprintf(msg, sizeof(msg),
                                  "[desktop] DOOM skipped (mem=%lu MiB, need >=84)\n",
                                  (unsigned long)memMB);
                if (mn > 0) syscall(SYS_DBGSOUT, (uint64_t)msg, (uint64_t)mn, 0, 0, 0, 0);
            } else {
                wm_client_spawn(comp, &fb, "/mp/doom.elf", "DOOM",
                                640u, 440u, 0xFF000000u, 120u, 60u);
            }
        }
    }

    /* Desktop-owned (NOT shared with the client) full-work-area surface used
       only while maximized: opaque, no rounded shadow margin. The live text is
       mirrored in from the normal shared surface each frame.

       2026-10 内存: 原来在启动时就 malloc 整块 (~4MiB), 但绝大多数会话从不
       最大化。改成**第一次最大化时才分配** —— 64MiB 目标下这 4MiB 是硬开销。 */
    const uint32_t maxW = scrW;
    const uint32_t maxH = scrH - barH;
    uint32_t* maxSurf = nullptr;
    auto ensure_max_surf = [&]() -> uint32_t* {
        if (maxSurf) return maxSurf;
        maxSurf = (uint32_t*)malloc((size_t)maxW * maxH * sizeof(uint32_t));
        if (!maxSurf) return nullptr;
        FrameBuffer mb;
        mb.BaseAddress       = maxSurf;
        mb.BufferSize        = (uint64_t)maxW * maxH * sizeof(uint32_t);
        mb.Width = mb.PixelsPerScanLine = maxW;
        mb.Height            = maxH;
        DrawFillRect(&mb, 0, 0, maxW, SKYWIN_TITLE_H, SKYRGB_TITLE);
        DrawFillRect(&mb, 0, SKYWIN_TITLE_H, maxW, maxH - SKYWIN_TITLE_H, SKYRGB_PAPER);
        DrawFillRect(&mb, 0, SKYWIN_TITLE_H - 1, maxW, 1, SKYRGB_SEP);
        TTF_Font* mf = console_font();
        if (mf)
            TTF_DrawText(&mb, mf, 14, (SKYWIN_TITLE_H - 22) / 2,
                         "Skyline Console", SKYRGB_INK);
        SkyPaintCaptionIcons(&mb, 0, 0, (int32_t)maxW,
                             (int32_t)SKYWIN_TITLE_H, 1);
        return maxSurf;
    };

    /* WM-owned fixed-pitch surface used after an interactive edge resize. Its
       pitch is the maximum work-area width (plus shadow margin) so a drag that
       changes width never reallocates; unused right-hand columns stay alpha 0
       and are skipped by the alpha blender. */
    const int32_t M  = (int32_t)SKYWIN_SHADOW;
    const int32_t rzPitch = (int32_t)maxW + 2 * M;
    const int32_t rzCapRows = (int32_t)maxH + 2 * M;
    uint32_t* rzSurf = nullptr;

    for (int warm = 0; warm < 8; warm++) {
        comp.Compose();
        sys_yield();
    }

    MouseInit();

    /* WM-side keyboard channel: map the same shared event ring (the notepad
       app is the text consumer; the WM reads global keys / future focus). */
    static KbdShared* wmKbd = nullptr;
    uint64_t kbdAddr = syscall(SYSCALL_DEV_MMAP, (uint64_t)KBD_DEV_TYPE,
                               0, 0, 0, 0, 0);
    if ((int64_t)kbdAddr > 0) wmKbd = (KbdShared*)kbdAddr;
    int wmKbdSlot = wmKbd ? kbd_reader_register(wmKbd, (int32_t)sys_getpid()) : -1;
    uint64_t wmKbdCursor = wmKbdSlot >= 0 ? kbd_reader_pos(wmKbd, wmKbdSlot) : 0;

    const uint64_t tsc_per_ms = probe_tsc_per_ms();
    const uint64_t move_gap   = 8u   * tsc_per_ms;   /* pointer overlay ~120Hz */
    const uint64_t scene_gap  = 33u  * tsc_per_ms;   /* recompose scene ~30Hz */
    const uint64_t idle_gap   = 50u  * tsc_per_ms;   /* scene refresh, still  */
    const uint64_t chrome_gap = 16u  * tsc_per_ms;   /* live-resize repaint  */
    const uint64_t clock_gap  = 500u * tsc_per_ms;   /* poll RTC at 2 Hz */

    /* 进桌面耗时 (main 入口 -> 主循环就绪): 把"卡"的入口段单独量化 */
    {
        char pm[160];
        int pn = snprintf(pm, sizeof(pm),
                          "[perf] desktop ready in %llums (fb %llux%llu, barH=%llu)\n",
                          (unsigned long long)((rdtsc64() - boot_t0) / tsc_per_ms),
                          (unsigned long long)scrW, (unsigned long long)scrH,
                          (unsigned long long)barH);
        if (pn > 0) syscall(SYS_DBGSOUT, (uint64_t)pm, (uint64_t)pn, 0, 0, 0, 0);
    }
    uint64_t pf_loops = 0, pf_last_report = 0;

    int32_t prev_x = -100;
    int32_t prev_y = -100;

    int32_t fb_width = (int32_t)fb.Width;
    int32_t fb_height = (int32_t)fb.Height;

    ps2_mouse_state_t *p = (ps2_mouse_state_t*)mouse_addr;

    uint32_t seq1, seq2;
    int32_t mx, my;
    uint64_t last_move  = rdtsc64();
    uint64_t last_scene = rdtsc64();
    uint64_t last_chrome = 0;
    uint64_t last_clock = 0;
    uint64_t last_client_seq = 0;   /* 控制台 OUT_SEQ 监视 (dirty 标记) */

    /* ---- window-manager interaction state ---- */
    enum { WM_NORMAL = 0, WM_MAX = 1, WM_MIN = 2, WM_CLOSED = 3 } wmMode = WM_NORMAL;
    int wm_prior = WM_NORMAL;   /* 最小化先态 */
    uint32_t normX = place.x, normY = place.y;   /* NORMAL surface top-left   */
    uint32_t normW = SKYWIN_W, normH = SKYWIN_H; /* NORMAL body size          */
    bool     prevLeft = false, dragging = false, resizing = false;
    bool     clientResizing = false;   /* a generic client owns the edge drag  */
    WmClient* rzClient = nullptr;      /* the client being resized             */
    int32_t  grabDX = 0, grabDY = 0;
    uint8_t  rzDir = 0;   /* edge resize directions: bit0 L,1 R,2 T,3 B       */
    int32_t  rsX = 0, rsY = 0, rsW = 0, rsH = 0, rsMX = 0, rsMY = 0;
    int      pressHit = 0;   /* 0 none,1 caption,2 min,3 max,4 close,5 tb,6 rz,8 content;
                                generic-client presses are owned by wm_client_* */
    int      tbPressId = -1; /* taskbar pill id under the current press        */
    /* 点击焦点: 按下命中某个窗口时记录它的 pid。焦点不再只看 z 序 ——
       DOOM 一启动就在最上层, 按 z 序它会永久抢走键盘, 于是点开 Console
       敲字却一个都收不到, 表现就是输入卡死。 */
    static uint64_t press_focus_pid = 0;
    uint8_t  ml = 0;         /* left-button snapshot from the seqlock block    */

    /* ---- focus routing (keyboard delivery follows click-to-front) ----
       Recompute the focused window every frame: the topmost VISIBLE window
       owns the keyboard. This self-heals lazy reader registration and client
       death without per-event bookkeeping. */
    auto wm_focus_pid = [&](uint64_t pid) {
        if (!wmKbd) return;
        if (!pid) { kbd_focus_broadcast(wmKbd); return; }
        int s = kbd_slot_for_pid(wmKbd, (int32_t)pid);
        if (s >= 0) kbd_focus_set_slot(wmKbd, s);
        /* slot not registered yet: leave the current owner, retried next frame */
    };
    auto wm_refocus = [&]() {
        if (!wmKbd) return;
        /* 优先看"用户最后按下的是哪个窗口": 它被抬到最上层后 z 序也会指向它,
           这里是双保险 —— 只要记录的 pid 还注册着, 键盘就给它。
           否则 z 序会让后启动的客户端 (DOOM) 永久霸占键盘。 */
        if (press_focus_pid != 0) {
            int fs = kbd_slot_for_pid(wmKbd, (int32_t)press_focus_pid);
            if (fs >= 0) { kbd_focus_set_slot(wmKbd, fs); return; }
        }
        const bool consVis = (wmMode == WM_NORMAL || wmMode == WM_MAX) &&
                             consoleLayer != nullptr;
        WmClient* tc = wm_client_top_visible();
        if (consVis && (!tc || consoleLayer->z >= tc->layer->z))
            wm_focus_pid(place.client_pid);
        else if (tc)
            wm_focus_pid(tc->place.client_pid);
        else
            kbd_focus_broadcast(wmKbd);
    };

    bool wmForceFullCompose = false;
    comp.SetCursor(0, 0, true);

    /* 光标线程: 见 CursorThreadEntry 的说明。必须在 SetCursor 之后启动,
       否则第一帧可能先被线程画一次再被初始化覆盖。 */
    g_mouse_page = p;
    if (sys_thread_launch((uint64_t)CursorThreadEntry, 0) < 0) {
        /* 启动失败就退回主线程贴图 (老路径), 保证光标仍可用 */
        g_mouse_page = nullptr;
    }
    /* 光标位置/可见性是"结构性"状态: 之后的第一次合成必须整屏 (局部行带可能
       根本不含光标所在行, 那样屏幕上就没有箭头)。 */
    wmForceFullCompose = true;

    for(;;){
        /* take the newest available mouse snapshot (seqlock retry) */
        while (true) {
            seq1 = __atomic_load_n(&p->seq, __ATOMIC_ACQUIRE);
            if (seq1 & 1) continue;
            mx = p->x;
            my = p->y;
            ml = p->left;
            seq2 = __atomic_load_n(&p->seq, __ATOMIC_ACQUIRE);
            if (seq1 == seq2) break;
        }

        if (mx < 0) mx = 0;
        if (my < 0) my = 0;
        if (mx >= fb_width - 16) mx = fb_width - 16;
        if (my >= fb_height - 16) my = fb_height - 16;
        /* 注意: 这里**不要**把夹紧后的坐标写回共享状态。
           曾经这么做过, 结果是一场竞态: 桌面每个循环都写回, 而驱动随时可能
           刚更新了位置 —— 写回会把新位置覆盖成旧值, 单次移动很容易被抹掉
           (实测光标直接卡在原地不动)。
           死区问题已经在内核侧解决: 驱动累加时就夹紧到 [0, 16384]
           (见 mouse.cpp), 指针不可能再跑到负数或无穷远。 */

        /* Drain the WM keyboard cursor (multi-reader ring; notepad has its
           own). Global shortcuts would be handled here; text goes to notepad.
           事件被显式丢弃 —— 键盘输入到焦点客户端的
           转发通道未实现 (已知缺口, 见 WM 路线图) */
        if (wmKbd) {
            uint64_t kh = __atomic_load_n(&wmKbd->head, __ATOMIC_ACQUIRE);
            wmKbdCursor = kbd_reader_resync(wmKbd, wmKbdCursor);
            while (wmKbdCursor < kh) {
                KbdEvent ke = wmKbd->ring[wmKbdCursor & (KBD_RING_CAP - 1u)];
                (void)ke;   /* 转发通道未实现前显式丢弃 (已知缺口) */
                wmKbdCursor++;
            }
            if (wmKbdSlot >= 0) kbd_reader_setpos(wmKbd, wmKbdSlot, wmKbdCursor);
        }

        uint64_t now = rdtsc64();
        /* ---- 临时性能统计: 每 2s 打出循环速率 / 合成耗时 / 光标耗时 ---- */
        {
            ++pf_loops;
            if (pf_last_report == 0) pf_last_report = now;
            if (now - pf_last_report >= tsc_per_ms * 2000u) {
                uint64_t cc, cn, bc, bn, lm;
                comp_perf_take(&cc, &cn, &bc, &bn, &lm);
                const uint64_t el_ms = (now - pf_last_report) / tsc_per_ms;
                char pm[220];
                int pn = snprintf(pm, sizeof(pm),
                    "[perf] loops=%llu/s comp=%llu avg_us=%llu | cur=%llu avg_us=%llu lockfail=%llu\n",
                    (unsigned long long)(el_ms ? pf_loops * 1000ull / el_ms : 0),
                    (unsigned long long)cn,
                    (unsigned long long)(cn ? (cc * 1000ull / tsc_per_ms) / cn : 0),
                    (unsigned long long)bn,
                    (unsigned long long)(bn ? (bc * 1000ull / tsc_per_ms) / bn : 0),
                    (unsigned long long)lm);
                if (pn > 0) syscall(SYS_DBGSOUT, (uint64_t)pm, (uint64_t)pn, 0, 0, 0, 0);
                pf_loops = 0;
                pf_last_report = now;
            }
        }
        bool moved = (mx != prev_x || my != prev_y);
        /* 损伤行带 (本轮的局部合成区域): 空 = 需要整屏合成 */
        int32_t dmg_y0 = 0, dmg_y1 = 0;


        /* ------------------------------------------------------------------
           输入优先 (响应速度的核心): 鼠标位置一有新值就**立刻**贴上光标,
           这一步只是重贴 16x16 方块, 亚毫秒级。

           然后把"重活" (窗口交互、客户端轮询、整屏合成) 限速到 ~30Hz:
           实测重活一段要 10~33ms (合成 10~19ms + 交互/轮询 ~17ms), 如果每轮
           都跑, 鼠标采样就被拖到 30~260ms 一次 —— 手感就是"指针跟不上手"。
           限速后输入链路每轮都走, 采样间隔只取决于空转让出, 通常 1~3ms。
           ------------------------------------------------------------------ */
        /* 光标由 CursorThreadEntry 独立线程贴图 (见上面的说明), 主线程不再做,
           这样即使本轮重活跑了几十毫秒, 指针也是跟手的。 */
        {
            static uint64_t last_heavy;
            if (last_heavy && now - last_heavy < scene_gap) {
                /* 让出 CPU 等下一轮重活窗口。
                   注意: 这里**不能**换成 SYSCALL_SLEEP_MS —— QEMU 实测该
                   内核的 Sleep 对 1~2ms 的短睡眠不会按时唤醒, 桌面主循环
                   会整体睡死 (loops 归零, DOOM 停在某一帧), 且因线程处于
                   睡眠态不触发 STALL WATCHDOG, 极难定位。
                   yield 虽然空转, 但绝不会丢唤醒。 */
                sys_yield();
                continue;
            }
            last_heavy = now;
        }



        /* ==================== Window-manager interaction ==================== */
        const bool leftDown = (ml != 0);
        const int32_t th = (int32_t)SKYWIN_TITLE_H;

        /* Current BODY rectangle on screen (the visible rect). */
        int32_t bx, by, bw, bh;
        if (wmMode == WM_MAX) { bx = 0; by = 0; bw = (int32_t)maxW; bh = (int32_t)maxH; }
        else { bx = (int32_t)normX + M; by = (int32_t)normY + M;
               bw = (int32_t)normW;   bh = (int32_t)normH; }

        /* taskbar app-pill hit rectangle (matches wm_draw_taskbar layout) */
        const int32_t tbX0 = 10, tbW = 208;
        const int32_t tbY0 = fb_height - (int32_t)barH + 7;
        const int32_t tbH  = (int32_t)barH - 14;
        auto inBox = [&](int32_t x0, int32_t y0, int32_t ww, int32_t hh) {
            return mx >= x0 && mx < x0 + ww && my >= y0 && my < y0 + hh;
        };
        /* ---- cursor shape: resize glyphs over (or dragging) an edge band -- */
        {
            uint32_t want = SKY_CURSOR_ARROW;
            if (clientResizing && rzClient)
                want = cursor_shape_for(rzClient->rzDir);
            else if (resizing)
                want = cursor_shape_for(rzDir);
            else if (!leftDown) {
                uint8_t d = 0;
                if (wm_client_edge_at(mx, my, &d) && d) {
                    want = cursor_shape_for(d);
                } else if (wmMode == WM_NORMAL && consoleLayer) {
                    const int32_t OUT = 4, RB = (int32_t)SKYWIN_RESIZE_BORDER;
                    bool spanX = mx >= bx - OUT && mx < bx + bw + OUT;
                    bool spanY = my >= by - OUT && my < by + bh + OUT;
                    uint8_t cd = 0;
                    if (spanY && (mx - bx) >= -OUT && (mx - bx) < RB)      cd |= 1;
                    if (spanY && (bx + bw - 1 - mx) >= -OUT &&
                                 (bx + bw - 1 - mx) < RB)                  cd |= 2;
                    if (spanX && (my - by) >= -OUT && (my - by) < RB)      cd |= 4;
                    if (spanX && (by + bh - 1 - my) >= -OUT &&
                                 (by + bh - 1 - my) < RB)                  cd |= 8;
                    if (cd) want = cursor_shape_for(cd);
                }
            }
            comp.SetCursorShape(want);
        }

        bool wmDirty = false;

        if (leftDown && !prevLeft) {                 /* press edge: classify  */
            pressHit = 0; rzDir = 0;

            /* ---- topmost-window hit test --------------------------------
               Every window (console + generic clients) sits on its own
               auto-created layer, so stacking order == layer z. Whichever
               covering window has the HIGHEST z owns the press; a press on
               the visible top window is never stolen from underneath. */
            WmClient* cTop = wm_client_hit(mx, my);

            /* Console grab region = body plus a few px outside (the resize
               band below reaches over the shadow, corners included). */
            const int32_t OUT = 4;
            bool inConsole = (wmMode == WM_NORMAL || wmMode == WM_MAX) &&
                             consoleLayer != nullptr &&
                             mx >= bx - OUT && mx < bx + bw + OUT &&
                             my >= by - OUT && my < by + bh + OUT;
            bool consoleTop = inConsole &&
                              (!cTop || consoleLayer->z >= cTop->layer->z);

            if (consoleTop) {
                /* Win11 click-to-front: a press anywhere on the body (caption,
                   border band or content) raises it above every other window. */
                comp.RaiseWindow(&consoleWin);

                int32_t minL   = bx + bw - 3 * (int32_t)SKYWIN_BTN_W;
                int32_t maxL   = bx + bw - 2 * (int32_t)SKYWIN_BTN_W;
                int32_t closeL = bx + bw - 1 * (int32_t)SKYWIN_BTN_W;
                if      (inBox(closeL, by, (int32_t)SKYWIN_BTN_W, th)) pressHit = 4;
                else if (inBox(maxL,   by, (int32_t)SKYWIN_BTN_W, th)) pressHit = 3;
                else if (inBox(minL,   by, (int32_t)SKYWIN_BTN_W, th)) pressHit = 2;
                else if (wmMode == WM_NORMAL) {
                    /* edge/corner resize band takes precedence over caption  */
                    int32_t dL = mx - bx, dR = bx + bw - 1 - mx;
                    int32_t dT = my - by, dB = by + bh - 1 - my;
                    const int32_t RB = (int32_t)SKYWIN_RESIZE_BORDER;
                    bool spanX = mx >= bx - OUT && mx < bx + bw + OUT;
                    bool spanY = my >= by - OUT && my < by + bh + OUT;
                    bool onL = spanY && dL >= -OUT && dL < RB;
                    bool onR = spanY && dR >= -OUT && dR < RB;
                    bool onT = spanX && dT >= -OUT && dT < RB;
                    bool onB = spanX && dB >= -OUT && dB < RB;
                    if (onL || onR || onT || onB) {
                        rzDir = (uint8_t)((onL?1:0)|(onR?2:0)|(onT?4:0)|(onB?8:0));
                        pressHit = 6;
                        resizing = true;
                        rsX = (int32_t)normX; rsY = (int32_t)normY;
                        rsW = (int32_t)normW; rsH = (int32_t)normH;
                        rsMX = mx; rsMY = my;
                        last_chrome = 0;         /* repaint immediately   */
                    } else if (inBox(bx, by, bw, th)) {
                        pressHit = 1;            /* caption empty area    */
                        /* 点中 Console 标题栏: 抬到所有客户端之上并抢焦点 */
                        press_focus_pid = place.client_pid;
                        comp.RaiseWindow(&consoleWin);
                        dragging = true;
                        grabDX = mx - (int32_t)normX;
                        grabDY = my - (int32_t)normY;
                    }
                }
                /* Plain content press only raised the window; claim it so it
                   cannot fall through to the taskbar. */
                if (pressHit == 0 && mx >= bx && mx < bx + bw &&
                                        my >= by && my < by + bh)
                    pressHit = 8;
                    press_focus_pid = place.client_pid;
            } else if (cTop) {
                /* Generic client press: raise + arm close/drag — all client
                   interaction lives in wm_client.cpp (the old notepad
                   special cases pressHit 7 / 9 used to be inline here).
                   The edge/corner resize band wins over the caption, exactly
                   like the console window. */
                uint8_t dir = 0;
                WmClient* rz = wm_client_edge_at(mx, my, &dir);
                if (rz && dir) {
                    wm_client_resize_begin(rz, mx, my, dir);
                    clientResizing = true;
                    rzClient = rz;
                } else {
                    wm_client_press(comp, cTop, mx, my);
                    /* 按下客户端窗口 = 切到它: 置顶 + 接管键盘。
                       不这样做的话键盘只按 z 序走, 点哪个窗口都换不了焦点。 */
                    if (cTop) {
                        comp.RaiseWindow(&cTop->win);
                        press_focus_pid = cTop->place.client_pid;
                    }
                }
            }

            if (pressHit == 0 && !wm_client_pressed() && my >= tbY0 &&
                my < tbY0 + tbH) {
        /* Hit whichever taskbar pill (console + generic clients) is
                   under the pointer; record its window id for the release. */
                TBRect tr[12];
                int nt = tb_layout(tr, 12, wmMode == WM_CLOSED, console_font(),
                                   (int32_t)scrW - TB_LEFT_MARGIN - SKY_TRAY_RESERVE);
                for (int i = 0; i < nt; i++)
                    if (mx >= tr[i].x && mx < tr[i].x + tr[i].w) {
                        pressHit = 5; tbPressId = tr[i].id; break;
                    }
            }
        }

        if (leftDown && dragging && wmMode == WM_NORMAL) {       /* caption move */
            int32_t nx = mx - grabDX, ny = my - grabDY;
            const int32_t xLo = -((int32_t)normW + 2 * M - 120);
            const int32_t xHi = fb_width  - 120;
            const int32_t yLo = -M;
            const int32_t yHi = fb_height - (int32_t)barH - th - M;
            if (nx < xLo) { nx = xLo; }
            if (nx > xHi) { nx = xHi; }
            if (ny < yLo) { ny = yLo; }
            if (ny > yHi) { ny = yHi; }
            normX = (uint32_t)nx; normY = (uint32_t)ny;
            comp.MoveWindow(&consoleWin, normX, normY);
        }

        /* Generic client caption drags — one shared path for every spawned
           app (replaces the notepad-only noteDrag block). */
        if (leftDown)
            wm_client_drag_all(comp, mx, my, fb_width, fb_height, (int32_t)barH);

        /* Generic-client edge/corner resize (same 8 directions as the
           console). Rate-limited by the caller's compose cadence. */
        if (leftDown && clientResizing && rzClient) {
            bool dirty = false;
            wm_client_resize_drag(rzClient, mx, my, fb_width, fb_height,
                                  (int32_t)barH, &dirty);
            if (dirty) { comp.Compose(); last_scene = now; }
        }

        if (leftDown && resizing && wmMode == WM_NORMAL) {      /* edge resize */
            int32_t dx = mx - rsMX, dy = my - rsMY;
            int32_t nw = rsW, nh = rsH, nx = rsX, ny = rsY;
            if (rzDir & 2) nw = rsW + dx;                       /* right      */
            if (rzDir & 8) nh = rsH + dy;                       /* bottom     */
            if (rzDir & 1) { nw = rsW - dx; nx = rsX + dx; }    /* left       */
            if (rzDir & 4) { nh = rsH - dy; ny = rsY + dy; }    /* top        */
            const int32_t minW = (int32_t)SKYWIN_MIN_W, minH = (int32_t)SKYWIN_MIN_H;
            if (nw < minW) { if (rzDir & 1) nx -= (minW - nw); nw = minW; }
            if (nh < minH) { if (rzDir & 4) ny -= (minH - nh); nh = minH; }
            if (nw > (int32_t)maxW) nw = (int32_t)maxW;
            if (nh > (int32_t)maxH) nh = (int32_t)maxH;
            if (nx < -M) { nw += (nx + M); nx = -M; if (nw < minW) nw = minW; }
            if (ny < -M) { nh += (ny + M); ny = -M; if (nh < minH) nh = minH; }

            normX = (uint32_t)nx; normY = (uint32_t)ny;
            normW = (uint32_t)nw; normH = (uint32_t)nh;
            if (!rzSurf)
                rzSurf = (uint32_t*)malloc((size_t)rzPitch * rzCapRows * sizeof(uint32_t));
            /* Repaint at a bounded rate; the pointer overlay still runs at
               its own 60 Hz, so the hand never stutters between resizes. */
            if (rzSurf && (now - last_chrome >= chrome_gap || last_chrome == 0)) {
                wm_paint_chrome_live(rzSurf, rzPitch, nw, nh,
                                     (const uint32_t*)place.desk_surf);
                wm_apply_normal(&consoleWin, &place, normX, normY, normW, normH,
                                rzSurf, rzPitch);
                last_chrome = now;
                comp.Compose();
                last_scene = now;
            }
        }

        if (!leftDown && prevLeft) {                 /* release edge: action  */
            bool fire = false;
            if (pressHit >= 2 && pressHit <= 4 &&
                (wmMode == WM_NORMAL || wmMode == WM_MAX)) {
                int32_t lx = bx + bw - (5 - pressHit) * (int32_t)SKYWIN_BTN_W;
                fire = inBox(lx, by, (int32_t)SKYWIN_BTN_W, th);
            } else if (pressHit == 5) {
                /* fire only if the release stays inside the same pill. */
                TBRect tr[12];
                int nt = tb_layout(tr, 12, wmMode == WM_CLOSED, console_font(),
                                   (int32_t)scrW - TB_LEFT_MARGIN - SKY_TRAY_RESERVE);
                for (int i = 0; i < nt; i++)
                    if (tr[i].id == tbPressId && mx >= tr[i].x &&
                        mx < tr[i].x + tr[i].w && my >= tbY0 && my < tbY0 + tbH)
                        fire = true;
            }

            /* Generic-client release (old pressHit 7/9): fire an armed close
               button only if the release is still inside it; every armed
               client drag ends here as well. Runs between Compose() frames,
               so the unregister/kill inside is worker-park safe. */
            bool relDirty = false;
            wm_client_release(comp, mx, my, fb_width, fb_height,
                              (int32_t)barH, &relDirty);
            if (relDirty) wmDirty = true;

            /* Finish a generic-client resize: repaint the real chrome once at
               the release geometry (the drag only showed a flat preview). */
            if (clientResizing && rzClient) {
                bool dirty = false;
                wm_client_resize_end(rzClient, fb_width, fb_height,
                                     (int32_t)barH, &dirty);
                if (dirty) wmDirty = true;
            }
            clientResizing = false;
            rzClient = nullptr;

            /* Finish a live resize at the exact release geometry. */
            if (resizing) {
                if (!rzSurf)
                    rzSurf = (uint32_t*)malloc((size_t)rzPitch * rzCapRows * sizeof(uint32_t));
                if (rzSurf) {
                    wm_rebuild_resize(rzSurf, rzPitch, (int32_t)normW, (int32_t)normH,
                                      (const uint32_t*)place.desk_surf);
                    wm_apply_normal(&consoleWin, &place, normX, normY, normW, normH,
                                    rzSurf, rzPitch);
                    wmDirty = true;
                }
            }
            dragging = false;
            resizing = false;

            if (fire) {
                WmDateTime fireDT;
                bool haveDT = wm_read_datetime(&fireDT);
                int tbState = (wmMode == WM_MIN) ? 1 : 0;
                if (pressHit == 2) {                        /* minimize         */
                    wmMode = WM_MIN;
                    comp.SetVisible(&consoleWin, false);
                    wm_draw_taskbar(wallBuf, cleanBar, scrW, scrH, 1, &fireDT, haveDT);
                    wmDirty = true;
                } else if (pressHit == 4) {                 /* close: kill + unregister */
                    wmMode = WM_CLOSED;
                    /* Audit: signal 9 actually terminates the client; signal 0
                       is only a liveness probe and left the button dead. */
                    if (place.client_pid) sys_kill(place.client_pid, 9);
                    comp.UnregisterWindow(&consoleWin);
                    consoleLayer = nullptr;   /* auto layer retired with it */
                    wm_draw_taskbar(wallBuf, cleanBar, scrW, scrH, 2, &fireDT, haveDT);
                    wmDirty = true;
                } else if (pressHit == 3 && (ensure_max_surf())) {    /* maximize toggle  */
                    if (wmMode == WM_NORMAL) {
                        wmMode = WM_MAX;
                        wm_apply_max(&consoleWin, maxSurf, maxW, maxH);
                    } else if (wmMode == WM_MAX) {
                        wmMode = WM_NORMAL;
                        wm_apply_normal(&consoleWin, &place, normX, normY,
                                        normW, normH, rzSurf, rzPitch);
                    }
                    wmDirty = true;
                } else if (pressHit == 5) {                 /* taskbar toggle   */
                    if (tbPressId == 0) {
                        /* Console pill: the existing minimize/restore path. */
                        if (wmMode == WM_MIN) {
                            /* restore the pre-minimize mode */
                            if (wm_prior == WM_MAX && maxSurf) {
                                wmMode = WM_MAX;
                                wm_apply_max(&consoleWin, maxSurf, maxW, maxH);
                            } else {
                                wmMode = WM_NORMAL;
                                wm_apply_normal(&consoleWin, &place, normX, normY,
                                                normW, normH, rzSurf, rzPitch);
                            }
                            comp.SetVisible(&consoleWin, true);
                            tbState = 0;
                        } else if (wmMode == WM_NORMAL || wmMode == WM_MAX) {
                            wm_prior = wmMode;
                            wmMode = WM_MIN;
                            comp.SetVisible(&consoleWin, false);
                            tbState = 1;
                        }
                        wm_draw_taskbar(wallBuf, cleanBar, scrW, scrH, tbState,
                                        &fireDT, haveDT);
                        /* 点 Console 的 pill: 还原(或本就可见)时提到最前并
                           把键盘给它。否则 z 序仍会让后启动的客户端 (DOOM)
                           永久霸占键盘, 敲字一个都进不了 Console。 */
                        if (wmMode != WM_MIN) {
                            comp.RaiseWindow(&consoleWin);
                            press_focus_pid = place.client_pid;
                        } else {
                            press_focus_pid = 0;   /* 最小化后交给 z 序 */
                        }
                        wmDirty = true;
                    } else if (tbPressId >= 1) {
                        /* Generic-client pill: minimize or restore that app. */
                        WmClient* cc = wm_client_by_order(tbPressId - 1);
                        if (cc) {
                            if (cc->minimized) {
                                wm_client_restore(comp, cc);
                                /* 还原即"切到它": 置顶并接管键盘 */
                                comp.RaiseWindow(&cc->win);
                                press_focus_pid = cc->place.client_pid;
                            } else {
                                wm_client_minimize(comp, cc);
                                press_focus_pid = 0;
                            }
                        }
                        const int cs = (wmMode == WM_CLOSED) ? 2
                                       : ((wmMode == WM_MIN) ? 1 : 0);
                        wm_draw_taskbar(wallBuf, cleanBar, scrW, scrH, cs,
                                        &fireDT, haveDT);
                        wmDirty = true;
                    }
                }
                /* notepad close (old pressHit 9) was handled above inside
                   wm_client_release() — nothing left to do here. */

                /* 卡死修复: 任务栏切换是点击触发的最重栅格化 (圆角 pill +
                   SDF 文本)。本内核的 IRQ 下半部依赖 idle 线程, 而 WM 主
                   循环可长期 RUNNABLE —— 让出一次, 给 PS/2 鼠标/键盘中断
                   处理喘息, 避免慢速/模拟核心上点击后输入看起来冻死。 */
                sys_yield();
            }
            pressHit = 0;
            tbPressId = -1;
        }
        prevLeft = leftDown;

        /* Keep the mirrored presentation surfaces in step with the client.
           只在客户端**真的产出新内容** (OUT_SEQ 变化) 或几何变化时才搬运。
           原实现在每次重活循环里无条件 memcpy 620x~350x4 ≈ 0.9 MB ——
           内容没变时这一遍完全是白搬的, 而重活循环是 30Hz。 */
        {
            static uint64_t last_mirror_seq  = ~0ull;
            static int32_t  last_mirror_w = -1, last_mirror_h = -1;
            static int      last_mirror_mode = -1;
            uint64_t mseq = 0;
            if (place.proto)
                mseq = *(volatile uint64_t*)(place.proto + SKYWIN_PROTO_OUT_SEQ * 8u);
            const int32_t mw = (wmMode == WM_MAX) ? (int32_t)maxW : (int32_t)normW;
            const int32_t mh = (wmMode == WM_MAX) ? (int32_t)maxH : (int32_t)normH;
            if (mseq != last_mirror_seq || wmMode != last_mirror_mode ||
                mw != last_mirror_w || mh != last_mirror_h) {
                last_mirror_seq  = mseq;
                last_mirror_mode = wmMode;
                last_mirror_w    = mw;
                last_mirror_h    = mh;
                if (wmMode == WM_MAX && maxSurf)
                    wm_mirror_content(maxSurf, (int32_t)maxW, 0,
                                      (int32_t)SKYWIN_TITLE_H,
                                      (int32_t)maxW, (int32_t)maxH,
                                      (const uint32_t*)place.desk_surf);
                else if (wmMode == WM_NORMAL && rzSurf &&
                         (normW != SKYWIN_W || normH != SKYWIN_H))
                    wm_mirror_content(rzSurf, rzPitch, M, M + th,
                                      (int32_t)normW, (int32_t)normH,
                                      (const uint32_t*)place.desk_surf);
            }
        }

        /* 审计卫生: 实际为 2 Hz (clock_gap = 500ms) */
        if (cleanBar && now - last_clock >= clock_gap) {
            last_clock = now;
            WmDateTime ndt;
            if (wm_read_datetime(&ndt)) {
                int tstate = (wmMode == WM_MIN) ? 1 : (wmMode == WM_CLOSED ? 2 : 0);
                wm_draw_taskbar(wallBuf, cleanBar, scrW, scrH, tstate,
                                &ndt, true);
                /* 只有任务栏条带变了 -> 局部合成即可 */
                dmg_y0 = (int32_t)scrH - (int32_t)barH;
                dmg_y1 = (int32_t)scrH;
                wmDirty = true;
            }
        }

        /* 分配器健康自检 (排查 "日志停在一行中间" 类整机停摆):
           deferred-free 链表一旦被写坏, 加固后的 walker 会累加这个计数。
           非 0 就说明"堆链表损坏"这个 bug 还存在 (只是不再把桌面打死),
           把计数与坏指针打到串口, 便于定位根因。

           动态链接注意: 这里**不能**用 `extern uint64_t g_alloc_...` 直接读 ——
           那两个变量住在 libsky.so 里, 主程序直接引用会生成 R_X86_64_COPY
           重定位 (链接器在主程序 BSS 里再留一份副本), 于是"读的人"和"写的人"
           看到的不是同一块内存, 计数永远是 0。改为通过导出函数取值。 */
        {
            static uint64_t alloc_check_at = 0;
            static uint64_t alloc_reported = 0;
            if (now - alloc_check_at > tsc_per_ms * 2000) {
                alloc_check_at = now;
                uint64_t corrupt = alloc_scb_list_corrupt_count();
                if (corrupt && corrupt != alloc_reported) {
                    alloc_reported = corrupt;
                    char msg[160];
                    int n = snprintf(msg, sizeof(msg),
                                     "[alloc] deferred-free list corrupt: "
                                     "count=%llu bad_ptr=0x%llx\n",
                                     (unsigned long long)corrupt,
                                     (unsigned long long)alloc_scb_list_bad_ptr_value());
                    if (n > 0)
                        syscall(SYS_DBGSOUT, (uint64_t)msg, (uint64_t)n,
                                0, 0, 0, 0);
                }
            }
        }

        /* 死窗口轮询 —— 客户端进程退出后移除窗口 (sys_kill(pid,0)
           为存活探测)。控制台单独处理; 其余通用客户端 (含 notepad) 由
           wm_clients_sweep 统一清扫, 图层随最后一个窗口自动退役。 */
        {
            static uint64_t dead_check_at = 0;
            if (now - dead_check_at > tsc_per_ms * 500) {
                dead_check_at = now;
                int64_t rConsole = place.client_pid ? sys_kill(place.client_pid, 0) : 1;
                if (place.client_pid && rConsole < 0) {
                    comp.UnregisterWindow(&consoleWin);
                    place.client_pid = 0;
                    consoleLayer = nullptr;   /* auto layer retired with it */
                    wmDirty = true;
                }
                wm_clients_sweep(comp, &wmDirty);
            }
        }

        /* 客户端输出立即标记 dirty —— 监视协议页 OUT_SEQ,
           任何新字符触发重合成 (不再依赖 2Hz 时钟, 迟到输出秒上屏);
           任务栏时钟的 2Hz 重合成仍作为空闲兜底 */
        /* 轮询之前 wmDirty 是否已经为真: 为真说明本轮有**结构性**变化
           (点击/窗口增删移动缩放/模式切换), 必须整屏合成; 否则才可以只重绘
           轮询给出的损伤行带。 */
        const bool dirty_pre_polls = wmDirty;

        if (place.proto) {
            uint64_t seq = *(volatile uint64_t *)(place.proto +
                                                  SKYWIN_PROTO_OUT_SEQ * 8u);
            if (seq != last_client_seq) {
                last_client_seq = seq;
                wmDirty = true;
                dmg_y0 = (int32_t)consoleWin.PosY - 8;
                dmg_y1 = (int32_t)(consoleWin.PosY + consoleWin.SizeY) + 8;
            }
        }

        /* Same frame ping for the generic (non-console) clients: DOOM bumps
           OUT_SEQ once per rendered frame, so a continuous-render client is
           recomposed at its own pace instead of the 2 Hz idle fallback. */
        wm_clients_poll_output(&wmDirty, &dmg_y0, &dmg_y1);

        wm_refocus();   /* keyboard focus tracks the topmost visible window */

        /* 整屏合成。
           试过"按损伤行带只重绘变化的行"(ComposeRows), 但在本机 (TCG) 上
           反而更慢: 行带合成要重新走一遍整行(壁纸 memcpy + 全层遍历),
           收益集中在任务栏那种极矮的带, 而客户端内容往往覆盖半屏以上 ——
           实测桌面主循环从 30~38 it/s 掉到 13~16 it/s, 鼠标更涩。
           所以这里保持整屏合成; 局部重绘只用于**光标**(16 行 × 2, 见
           Compositor::CursorMoveTo) —— 那才是真正高频且极小面积的部分。 */
        (void)dirty_pre_polls;
        (void)dmg_y0; (void)dmg_y1;
        wmForceFullCompose = false;
        if (wmDirty) { comp.Compose(); last_scene = rdtsc64(); }

        if (moved) {
            prev_x = mx;
            prev_y = my;
            /* 只有**几何变化** (拖动/缩放) 才需要强制重组 (光标已在上方贴好)。 */
            if (dragging || resizing || wm_client_any_drag()) {
                comp.Compose();
                last_scene = now;
            }
        } else {
            /* 指针静止: 不重绘 (场景变化有 wmDirty 路径), 让出 CPU。
               同上一处: 不可用 SYSCALL_SLEEP_MS (会睡死且不触发 STALL)。 */
            sys_yield();
        }

    }
    syscall(9, 0, 0, 0, 0, 0, 0);
    return 0;
}