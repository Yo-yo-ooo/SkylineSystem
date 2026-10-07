/* Host harness: run the real Skyline DOOM shim on Windows.
   Everything Skyline-specific is faked here — the fixed 0x400000 protocol
   page, the shared window surface, the keyboard ring and the syscalls — so
   doom_main.c itself runs unmodified (compiled with -Dmain=doom_client_main). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <syscall.h>
#include <graphic/winstyle.h>
#include <graphic/kbdshare.h>
#include <graphic/mouseshare.h>

extern int doom_client_main(void);
extern int gametic;          /* PureDOOM global: game tics elapsed (35/s) */

#define SURF_W 692u
#define SURF_H 492u
#define CONTENT_W 640u
#define CONTENT_H 400u
#define CONTENT_X 26u
#define CONTENT_Y 58u

static uint32_t* g_surf = 0;
static uint64_t* g_proto = 0;
static KbdShared* g_kbd = 0;
static ps2_mouse_state_t* g_mouse = 0;
static volatile uint64_t g_kbd_head = 0;

/* ---- path rewriting: the shim hardcodes /mp/... ----
   Implemented on top of _open/_fdopen so the override does not recurse. */
#include <io.h>
#include <fcntl.h>
/* Must be a real Win32 path: the mingw CRT does NOT translate MSYS "/tmp". */
#define MP_REAL "C:/Users/yoyo1/AppData/Local/Temp/mp"

static int mode_flags(const char* m) {
    int f = _O_BINARY;
    if (!m) return -1;
    if (m[0] == 'r') f |= (strchr(m, '+') ? _O_RDWR : _O_RDONLY);
    else if (m[0] == 'w') f |= (strchr(m, '+') ? _O_RDWR : _O_WRONLY) | _O_CREAT | _O_TRUNC;
    else if (m[0] == 'a') f |= (strchr(m, '+') ? _O_RDWR : _O_WRONLY) | _O_CREAT | _O_APPEND;
    else return -1;
    return f;
}

FILE* fopen(const char* path, const char* mode) {
    char buf[512];
    int flags = mode_flags(mode);
    if (flags < 0 || !path) return NULL;
    if (strncmp(path, "/mp/", 4) == 0) {
        snprintf(buf, sizeof(buf), "%s%s", MP_REAL, path + 3);
        path = buf;
    }
    int fd = _open(path, flags, 0666);
    if (fd < 0) return NULL;
    return _fdopen(fd, mode);
}

long fsize(FILE* f) {
    long pos, end;
    if (!f) return 0;
    pos = ftell(f);
    fseek(f, 0, SEEK_END);
    end = ftell(f);
    fseek(f, pos, SEEK_SET);
    return end;
}

uint64_t sys_getpid(void) { return 4242; }
void     sys_exit(uint64_t s) { exit((int)s); }

/* ---- the frame pump: DOOM calls this once per loop turn ---- */
static int     g_tick = 0;
static clock_t g_t0 = 0;
static int     g_step = 0;
static int     g_dumped = 0;

static void kbd_push(uint16_t key, uint8_t action) {
    if (!g_kbd) return;
    KbdEvent e;
    e.key = key; e.action = action; e.mods = 0;
    g_kbd->ring[g_kbd_head & (KBD_RING_CAP - 1u)] = e;
    g_kbd_head++;
    __atomic_store_n(&g_kbd->head, g_kbd_head, __ATOMIC_RELEASE);
}

static void dump_raw(const char* path) {
    FILE* f = fopen(path, "wb");
    if (!f) return;
    for (uint32_t y = 0; y < CONTENT_H; y++) {
        const uint32_t* row = g_surf + (uint64_t)(CONTENT_Y + y) * SURF_W + CONTENT_X;
        fwrite(row, 1, CONTENT_W * 4, f);
    }
    fclose(f);
    fprintf(stderr, "[harness] dumped %s\n", path);
}

uint64_t sys_yield(void) {
    static int last_gametic = 0;
    static double last_t = 0;
    g_tick++;
    if (!g_t0) g_t0 = clock();
    double t = (double)(clock() - g_t0) / (double)CLOCKS_PER_SEC;

    if (t - last_t >= 1.0) {          /* game tics advanced per wall second */
        fprintf(stderr, "[harness] t=%.1f tics/s=%d (gametic=%d)\n",
                t, gametic - last_gametic, gametic);
        last_gametic = gametic;
        last_t = t;
    }

    /* script: ENTER menu -> ENTER new game -> ENTER skill -> in game */
    if (g_step == 0 && t > 1.2) {
        kbd_push('\n', KBD_ACTION_DOWN); kbd_push('\n', KBD_ACTION_UP);
        g_step = 1; fprintf(stderr, "[harness] t=%.2f enter#1 (menu)\n", t);
    } else if (g_step == 1 && t > 2.0) {
        kbd_push('\n', KBD_ACTION_DOWN); kbd_push('\n', KBD_ACTION_UP);
        g_step = 2; fprintf(stderr, "[harness] t=%.2f enter#2 (new game)\n", t);
    } else if (g_step == 2 && t > 2.8) {
        kbd_push('\n', KBD_ACTION_DOWN); kbd_push('\n', KBD_ACTION_UP);
        g_step = 3; fprintf(stderr, "[harness] t=%.2f enter#3 (episode 1)\n", t);
    } else if (g_step == 3 && t > 3.6) {
        kbd_push('\n', KBD_ACTION_DOWN); kbd_push('\n', KBD_ACTION_UP);
        g_step = 4; fprintf(stderr, "[harness] t=%.2f enter#4 (skill)\n", t);
    } else if (g_step == 4 && t > 4.4) {
        dump_raw("/tmp/frame_menu.raw");
        g_step = 5;
    } else if (g_step == 5 && t > 8.5) {
        dump_raw("/tmp/frame_game.raw");
        fprintf(stderr, "[harness] frames=%d t=%.2f gametic=%d -> done\n",
                g_tick, t, gametic);
        exit(0);
    }
    if (t > 30.0) { fprintf(stderr, "[harness] timeout\n"); exit(2); }
    return 0;
}

uint64_t host_syscall(uint64_t n, uint64_t a1, uint64_t a2, uint64_t a3,
                      uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    switch (n) {
    case 17:   /* SYSCALL_TIME: RTC wall seconds */
        return (uint64_t)time(0);
    case 24: { /* SYSCALL_DBGSOUT */
        const char* s = (const char*)a1;
        uint64_t len = a2;
        fwrite(s, 1, (size_t)len, stderr);
        return len;
    }
    case 21: { /* SYSCALL_DEV_MMAP: keyboard page (9) or mouse page (7) */
        if (a1 == DEV_TYPE_KEYBOARD) {
            if (!g_kbd) {
                g_kbd = (KbdShared*)calloc(1, sizeof(KbdShared));
                g_kbd_head = 0;
            }
            return (uint64_t)g_kbd;
        }
        if (a1 == DEV_TYPE_PS2_MOUSE) {
            if (!g_mouse) g_mouse = (ps2_mouse_state_t*)calloc(1, sizeof(*g_mouse));
            return (uint64_t)g_mouse;
        }
        return 0;
    }
    default:
        return 0;
    }
}

int main(void) {
    /* the shim reads its geometry from a fixed protocol page */
    g_proto = (uint64_t*)VirtualAlloc((LPVOID)SKYWIN_PROTO_PAGE_VA, 8192,
                                      MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!g_proto) { fprintf(stderr, "[harness] cannot reserve 0x400000\n"); return 1; }
    memset(g_proto, 0, 8192);

    g_surf = (uint32_t*)calloc((size_t)SURF_W * SURF_H, 4);
    memset(g_surf, 0, (size_t)SURF_W * SURF_H * 4);

    volatile uint64_t* q = (volatile uint64_t*)SKYWIN_PROTO_PAGE_VA;
    q[SKYWIN_PROTO_CONTENT_VA] =
        (uint64_t)(g_surf + (uint64_t)CONTENT_Y * SURF_W + CONTENT_X);
    q[SKYWIN_PROTO_CONTENT_SZ] = (uint64_t)SURF_W * CONTENT_H * 4u;
    q[SKYWIN_PROTO_CONTENT_W]  = CONTENT_W;
    q[SKYWIN_PROTO_CONTENT_H]  = CONTENT_H;
    q[SKYWIN_PROTO_PITCH]      = SURF_W;
    q[SKYWIN_PROTO_WHOLE_VA]   = (uint64_t)g_surf;
    q[SKYWIN_PROTO_WIN_W]      = SURF_W;
    q[SKYWIN_PROTO_WIN_H]      = SURF_H;

    fprintf(stderr, "[harness] proto=%p surf=%p\n", (void*)g_proto, (void*)g_surf);
    return doom_client_main();
}
