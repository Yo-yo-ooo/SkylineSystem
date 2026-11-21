//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT
//
// dos: SkylineSystem 的 80x25 DOS 文本屏窗口客户端（L3 用户态验收）。
//
// 职责：
//   - 按 settings/notepad 的方式读取 SKYWIN_PROTO_PAGE_VA 协议页，构造
//     FrameBuffer，直接把 uint32_t 像素写进共享内容区（不调用任何绘制库、不
//     依赖 TTF 字体文件 —— 字体是可选资源，缺失时用户仍必须能看到 DOS 画面）；
//   - 内置 CP437 风格 8x8 位图点阵字模（ASCII 0x20..0x7E 全可辨认 +
//     上半区块/框线字符回退）与标准 VGA16 色调色板；
//   - syscall(210, buf, 4016) 读取 kernel.arch.dosvm DynModule 导出的影屏快照，按
//     generation 判脏：只有 generation / flags / cpu / trips / 读状态变化，
//     或首次有效、或按键触发过 syscall 211，才重绘并递增 OUT_SEQ；
//   - 100ms 节流轮询（SYSCALL_MONOTONIC_MS=27）+ sys_yield()，空闲不空转刷屏；
//   - Enter / R / 空格触发 syscall(211) 做一次 long->real->long 往返，启动时
//     自动触发一次，保证打开窗口就能看到内容；返回码显示在状态栏。
//
// 与内核的契约（只读定义在 kernel/include/arch/x86_64/dosvm/dosvm_contract.h，
// 本文件不 include 任何内核头，只在本地复刻常量，避免用户态依赖 kernel/）：
//   buf = 16B header + 4000B cells((char, attr) 交替、行优先)
//   u64[0] = generation(低32) | flags(高32)
//   u64[1] = last_cpu(低32)  | trips(高32)
//   flags: VALID(1<<0) / RUNNING(1<<1) / REAL_OK(1<<2)
//   attr: 低 4 位前景、高 4 位背景（标准 VGA 属性字节）
#include <stdint.h>
#include <stdio.h>
#include <syscall.h>
#include <base/arch/x86_64/syscalln.h>
#include <graphic/fb.h>
#include <graphic/winstyle.h>
#include <graphic/kbdshare.h>

/* ---- DOS 影屏快照契约（本地复刻，不 include kernel/） ------------------- */
#define DOS_SYSCALL_READ    210u   /* (user_buf, cap) -> 4016 / -EAGAIN / -EINVAL */
#define DOS_SYSCALL_ENTER   211u   /* (cpu_id=0 自动选 AP, ...) -> 0 / -errno */
#define DOS_SYSCALL_STAGE   212u   /* (user_img, len, user_args, args_len) -> 0 */
#define DOS_SYSCALL_KEYS    213u   /* (user_keys, count) -> 0 */
#define DOS_SYSCALL_MEM     214u   /* (off, user_buf, len, is_write) -> 0 */

/* 211 的正向返回码：0 之外只有这两个，用户态据此决定下一步动作。 */
#define DOS_RC_NEED_FS      1
#define DOS_RC_SUSPENDED    2

/* 低内存里 16 位侧的控制块（与内核 dosvm_contract.h 的布局一一对应）。 */
#define DOS_LOW_BASE        0x00070000u
#define DOS_FS_BLOCK        0x0007E1B8u   /* S_FSACT .. S_FSERR，9 x uint16 */
/* 把 .COM 放进 /mp/boot.com 就在启动时被装进内核实模式加载区；缺文件时
 * 继续用内核内置的 banner 演示（老行为）。 */
#define DOS_COM_PATH        "/mp/boot.com"
#define DOS_COM_MAX         0xC000u
#define DOS_COLS            80u
#define DOS_ROWS            25u
#define DOS_CELL_BYTES      (DOS_COLS * DOS_ROWS * 2u)      /* 4000 */
#define DOS_HDR_BYTES       16u                              /* 2 x uint64 */
#define DOS_BUFSZ           (DOS_HDR_BYTES + DOS_CELL_BYTES) /* 4016 */
#define DOS_FLAG_VALID      (1u << 0)
#define DOS_FLAG_RUNNING    (1u << 1)
#define DOS_FLAG_REAL_OK    (1u << 2)
#define DOS_FLAG_COM_MODE   (1u << 3)   /* 加载区里有 .COM，不是 banner */
#define DOS_FLAG_COM_EXITED (1u << 4)   /* 上一次 .COM 自己结束了 */
#define DOS_FLAG_WAITKEY    (1u << 5)   /* .COM 要键但队列空，本次已收尾 */
#define DOS_EAGAIN          11     /* -EAGAIN：还没跑过 / 正在切换 */

/* ---- 几何与节流 --------------------------------------------------------- */
#define DOS_GLYPH_W         8
#define DOS_GLYPH_H         8
#define DOS_SCREEN_W        (DOS_COLS * DOS_GLYPH_W)   /* 640 */
#define DOS_SCREEN_H        (DOS_ROWS * DOS_GLYPH_H)   /* 200 */
#define DOS_STATUS_H        8                          /* 状态栏一行 */
#define DOS_POLL_INTERVAL_MS 100u

/* ---- 标准 VGA 16 色（0xAARRGGBB）--------------------------------------- *
 * 索引即 VGA 属性半字节值：低 4 位前景 / 高 4 位背景。                      */
static const uint32_t kVgaPalette[16] = {
    0xFF000000u, /*  0 black          */
    0xFF0000AAu, /*  1 blue           */
    0xFF00AA00u, /*  2 green          */
    0xFF00AAAAu, /*  3 cyan           */
    0xFFAA0000u, /*  4 red            */
    0xFFAA00AAu, /*  5 magenta        */
    0xFFAA5500u, /*  6 brown          */
    0xFFAAAAAAu, /*  7 light grey     */
    0xFF555555u, /*  8 dark grey      */
    0xFF5555FFu, /*  9 light blue     */
    0xFF55FF55u, /* 10 light green    */
    0xFF55FFFFu, /* 11 light cyan     */
    0xFFFF5555u, /* 12 light red      */
    0xFFFF55FFu, /* 13 light magenta  */
    0xFFFFFF55u, /* 14 yellow         */
    0xFFFFFFFFu, /* 15 white          */
};

/* ---- 内置 8x8 位图点阵字模：ASCII 0x20..0x7E（95 个）------------------ *
 * 每个 uint8_t 是一行 8 像素，bit7 = 最左、bit0 = 最右；行序从上到下。
 * 数据源自 font8x8 系（原始约定 bit0 = 最左），已统一 bit-reverse 成 MSB-left，
 * 与绘制侧 (bits & (0x80u >> c)) 及扩展字符表（0xF0 = 左半）语义一致。
 * 采用 IBM PC / VGA 8x8 风格点阵（public-domain 8x8 basic font 的等价实现），
 * 是真正的硬编码位图，运行时不读任何字体文件、不渲染 TTF。 */
static const uint8_t kAsciiGlyphs[95][DOS_GLYPH_H] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* 0x20 ' '  */
    {0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00}, /* 0x21 '!'  */
    {0x6C,0x6C,0x00,0x00,0x00,0x00,0x00,0x00}, /* 0x22 '"'  */
    {0x6C,0x6C,0xFE,0x6C,0xFE,0x6C,0x6C,0x00}, /* 0x23 '#'  */
    {0x30,0x7C,0xC0,0x78,0x0C,0xF8,0x30,0x00}, /* 0x24 '$'  */
    {0x00,0xC6,0xCC,0x18,0x30,0x66,0xC6,0x00}, /* 0x25 '%'  */
    {0x38,0x6C,0x38,0x76,0xDC,0xCC,0x76,0x00}, /* 0x26 '&'  */
    {0x60,0x60,0xC0,0x00,0x00,0x00,0x00,0x00}, /* 0x27 '\'' */
    {0x18,0x30,0x60,0x60,0x60,0x30,0x18,0x00}, /* 0x28 '('  */
    {0x60,0x30,0x18,0x18,0x18,0x30,0x60,0x00}, /* 0x29 ')'  */
    {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00}, /* 0x2A '*'  */
    {0x00,0x30,0x30,0xFC,0x30,0x30,0x00,0x00}, /* 0x2B '+'  */
    {0x00,0x00,0x00,0x00,0x00,0x30,0x30,0x60}, /* 0x2C ','  */
    {0x00,0x00,0x00,0xFC,0x00,0x00,0x00,0x00}, /* 0x2D '-'  */
    {0x00,0x00,0x00,0x00,0x00,0x30,0x30,0x00}, /* 0x2E '.'  */
    {0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80}, /* 0x2F '/'  */
    {0x7C,0xC6,0xCE,0xDE,0xF6,0xE6,0x7C,0x00}, /* 0x30 '0'  */
    {0x30,0x70,0x30,0x30,0x30,0x30,0xFC,0x00}, /* 0x31 '1'  */
    {0x78,0xCC,0x0C,0x38,0x60,0xCC,0xFC,0x00}, /* 0x32 '2'  */
    {0x78,0xCC,0x0C,0x38,0x0C,0xCC,0x78,0x00}, /* 0x33 '3'  */
    {0x1C,0x3C,0x6C,0xCC,0xFE,0x0C,0x1E,0x00}, /* 0x34 '4'  */
    {0xFC,0xC0,0xF8,0x0C,0x0C,0xCC,0x78,0x00}, /* 0x35 '5'  */
    {0x38,0x60,0xC0,0xF8,0xCC,0xCC,0x78,0x00}, /* 0x36 '6'  */
    {0xFC,0xCC,0x0C,0x18,0x30,0x30,0x30,0x00}, /* 0x37 '7'  */
    {0x78,0xCC,0xCC,0x78,0xCC,0xCC,0x78,0x00}, /* 0x38 '8'  */
    {0x78,0xCC,0xCC,0x7C,0x0C,0x18,0x70,0x00}, /* 0x39 '9'  */
    {0x00,0x30,0x30,0x00,0x30,0x30,0x00,0x00}, /* 0x3A ':'  */
    {0x00,0x30,0x30,0x00,0x30,0x30,0x60,0x00}, /* 0x3B ';'  */
    {0x18,0x30,0x60,0xC0,0x60,0x30,0x18,0x00}, /* 0x3C '<'  */
    {0x00,0x00,0xFC,0x00,0xFC,0x00,0x00,0x00}, /* 0x3D '='  */
    {0x60,0x30,0x18,0x0C,0x18,0x30,0x60,0x00}, /* 0x3E '>'  */
    {0x78,0xCC,0x0C,0x18,0x30,0x00,0x30,0x00}, /* 0x3F '?'  */
    {0x7C,0xC6,0xDE,0xDE,0xDE,0xC0,0x78,0x00}, /* 0x40 '@'  */
    {0x30,0x78,0xCC,0xCC,0xFC,0xCC,0xCC,0x00}, /* 0x41 'A'  */
    {0xFC,0x66,0x66,0x7C,0x66,0x66,0xFC,0x00}, /* 0x42 'B'  */
    {0x3C,0x66,0xC0,0xC0,0xC0,0x66,0x3C,0x00}, /* 0x43 'C'  */
    {0xF8,0x6C,0x66,0x66,0x66,0x6C,0xF8,0x00}, /* 0x44 'D'  */
    {0xFE,0x62,0x68,0x78,0x68,0x62,0xFE,0x00}, /* 0x45 'E'  */
    {0xFE,0x62,0x68,0x78,0x68,0x60,0x60,0x00}, /* 0x46 'F'  */
    {0x3C,0x66,0xC0,0xC0,0xCE,0x66,0x3E,0x00}, /* 0x47 'G'  */
    {0xCC,0xCC,0xCC,0xFC,0xCC,0xCC,0xCC,0x00}, /* 0x48 'H'  */
    {0x78,0x30,0x30,0x30,0x30,0x30,0x78,0x00}, /* 0x49 'I'  */
    {0x1E,0x0C,0x0C,0x0C,0xCC,0xCC,0x78,0x00}, /* 0x4A 'J'  */
    {0xE6,0x66,0x6C,0x78,0x6C,0x66,0xE6,0x00}, /* 0x4B 'K'  */
    {0xF0,0x60,0x60,0x60,0x60,0x62,0xFE,0x00}, /* 0x4C 'L'  */
    {0xC6,0xEE,0xFE,0xFE,0xD6,0xC6,0xC6,0x00}, /* 0x4D 'M'  */
    {0xC6,0xE6,0xF6,0xDE,0xCE,0xC6,0xC6,0x00}, /* 0x4E 'N'  */
    {0x38,0x6C,0xC6,0xC6,0xC6,0x6C,0x38,0x00}, /* 0x4F 'O'  */
    {0xFC,0x66,0x66,0x7C,0x60,0x60,0x60,0x00}, /* 0x50 'P'  */
    {0x38,0x6C,0xC6,0xC6,0xC6,0x6C,0x38,0x0E}, /* 0x51 'Q'  */
    {0xFC,0x66,0x66,0x7C,0x6C,0x66,0xE6,0x00}, /* 0x52 'R'  */
    {0x78,0xCC,0xE0,0x70,0x1C,0xCC,0x78,0x00}, /* 0x53 'S'  */
    {0x7E,0x18,0x18,0x18,0x18,0x18,0x18,0x00}, /* 0x54 'T'  */
    {0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0x78,0x00}, /* 0x55 'U'  */
    {0xCC,0xCC,0xCC,0xCC,0xCC,0x78,0x30,0x00}, /* 0x56 'V'  */
    {0xC6,0xC6,0xC6,0xD6,0xFE,0xEE,0xC6,0x00}, /* 0x57 'W'  */
    {0xC6,0xC6,0x6C,0x38,0x38,0x6C,0xC6,0x00}, /* 0x58 'X'  */
    {0xCC,0xCC,0xCC,0x78,0x30,0x30,0x30,0x00}, /* 0x59 'Y'  */
    {0xFE,0xC6,0x8C,0x18,0x32,0x66,0xFE,0x00}, /* 0x5A 'Z'  */
    {0x78,0x60,0x60,0x60,0x60,0x60,0x78,0x00}, /* 0x5B '['  */
    {0x80,0x40,0x20,0x10,0x08,0x04,0x02,0x01}, /* 0x5C '\\' */
    {0x1E,0x06,0x06,0x06,0x06,0x06,0x1E,0x00}, /* 0x5D ']'  */
    {0x30,0x78,0xCC,0x00,0x00,0x00,0x00,0x00}, /* 0x5E '^'  */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF}, /* 0x5F '_'  */
    {0x30,0x30,0x60,0x00,0x00,0x00,0x00,0x00}, /* 0x60 '`'  */
    {0x00,0x00,0x78,0x0C,0x7C,0xCC,0x76,0x00}, /* 0x61 'a'  */
    {0xE0,0x60,0x60,0x7C,0x66,0x66,0xDC,0x00}, /* 0x62 'b'  */
    {0x00,0x00,0x78,0xCC,0xC0,0xCC,0x78,0x00}, /* 0x63 'c'  */
    {0x1C,0x0C,0x0C,0x7C,0xCC,0xCC,0x76,0x00}, /* 0x64 'd'  */
    {0x00,0x00,0x78,0xCC,0xFC,0xC0,0x78,0x00}, /* 0x65 'e'  */
    {0x38,0x6C,0x60,0xF0,0x60,0x60,0xF0,0x00}, /* 0x66 'f'  */
    {0x00,0x00,0x76,0xCC,0xCC,0x7C,0x0C,0xF8}, /* 0x67 'g'  */
    {0xE0,0x60,0x6C,0x76,0x66,0x66,0xE6,0x00}, /* 0x68 'h'  */
    {0x30,0x00,0x70,0x30,0x30,0x30,0x78,0x00}, /* 0x69 'i'  */
    {0x0C,0x00,0x0C,0x0C,0x0C,0xCC,0xCC,0x78}, /* 0x6A 'j'  */
    {0xE0,0x60,0x66,0x6C,0x78,0x6C,0xE6,0x00}, /* 0x6B 'k'  */
    {0x70,0x30,0x30,0x30,0x30,0x30,0x78,0x00}, /* 0x6C 'l'  */
    {0x00,0x00,0xCC,0xFE,0xFE,0xD6,0xC6,0x00}, /* 0x6D 'm'  */
    {0x00,0x00,0xF8,0xCC,0xCC,0xCC,0xCC,0x00}, /* 0x6E 'n'  */
    {0x00,0x00,0x78,0xCC,0xCC,0xCC,0x78,0x00}, /* 0x6F 'o'  */
    {0x00,0x00,0xDC,0x66,0x66,0x7C,0x60,0x60}, /* 0x70 'p'  */
    {0x00,0x00,0x76,0xCC,0xCC,0x7C,0x0C,0x0C}, /* 0x71 'q'  */
    {0x00,0x00,0xDC,0x76,0x66,0x60,0x60,0x00}, /* 0x72 'r'  */
    {0x00,0x00,0x7C,0xC0,0x78,0x0C,0xF8,0x00}, /* 0x73 's'  */
    {0x10,0x30,0xFC,0x30,0x30,0x34,0x18,0x00}, /* 0x74 't'  */
    {0x00,0x00,0xCC,0xCC,0xCC,0xCC,0x76,0x00}, /* 0x75 'u'  */
    {0x00,0x00,0xCC,0xCC,0xCC,0x78,0x30,0x00}, /* 0x76 'v'  */
    {0x00,0x00,0xC6,0xC6,0xD6,0xFE,0x6C,0x00}, /* 0x77 'w'  */
    {0x00,0x00,0xC6,0x6C,0x38,0x6C,0xC6,0x00}, /* 0x78 'x'  */
    {0x00,0x00,0xCC,0xCC,0xCC,0x7C,0x0C,0xF8}, /* 0x79 'y'  */
    {0x00,0x00,0xFC,0x98,0x30,0x64,0xFC,0x00}, /* 0x7A 'z'  */
    {0x1C,0x30,0x30,0x60,0x30,0x30,0x1C,0x00}, /* 0x7B '{'  */
    {0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x00}, /* 0x7C '|'  */
    {0x70,0x18,0x18,0x0C,0x18,0x18,0x70,0x00}, /* 0x7D '}'  */
    {0x00,0x00,0x00,0x8C,0xDE,0x72,0x00,0x00}, /* 0x7E '~'  */
};

/* ---- CP437 上半区（0x80..0xFF）合理回退 -------------------------------- *
 * 不引入任何外部字体资源：把 DOS 程序常用的方块/阴影/单双线框字符画成对应
 * 密度的点阵，其余扩展字符回退成 '?'，保证屏幕始终有可见、可辨认的内容。 */
static const uint8_t kExtShadeLight[DOS_GLYPH_H]  = /* 0xB0 ░ */
    {0x22,0x88,0x22,0x88,0x22,0x88,0x22,0x88};
static const uint8_t kExtShadeMedium[DOS_GLYPH_H] = /* 0xB1 ▒ */
    {0x55,0xAA,0x55,0xAA,0x55,0xAA,0x55,0xAA};
static const uint8_t kExtShadeDark[DOS_GLYPH_H]   = /* 0xB2 ▓ */
    {0xDD,0x77,0xDD,0x77,0xDD,0x77,0xDD,0x77};
static const uint8_t kExtVLine[DOS_GLYPH_H]       = /* 0xB3 │ */
    {0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18};
static const uint8_t kExtTeeRight[DOS_GLYPH_H]    = /* 0xB4 ┤ */
    {0x18,0x18,0x18,0xF8,0x18,0x18,0x18,0x18};
static const uint8_t kExtDoubleVLine[DOS_GLYPH_H] = /* 0xBA ║ */
    {0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66};
static const uint8_t kExtCornerTR[DOS_GLYPH_H]    = /* 0xBF ┐ */
    {0x00,0x00,0x00,0xF8,0x18,0x18,0x18,0x18};
static const uint8_t kExtCornerBL[DOS_GLYPH_H]    = /* 0xC0 └ */
    {0x18,0x18,0x18,0x1F,0x00,0x00,0x00,0x00};
static const uint8_t kExtTeeUp[DOS_GLYPH_H]       = /* 0xC1 ┴ */
    {0x18,0x18,0x18,0xFF,0x00,0x00,0x00,0x00};
static const uint8_t kExtTeeDown[DOS_GLYPH_H]     = /* 0xC2 ┬ */
    {0x00,0x00,0x00,0xFF,0x18,0x18,0x18,0x18};
static const uint8_t kExtTeeLeft[DOS_GLYPH_H]     = /* 0xC3 ├ */
    {0x18,0x18,0x18,0x1F,0x18,0x18,0x18,0x18};
static const uint8_t kExtHLine[DOS_GLYPH_H]       = /* 0xC4 ─ */
    {0x00,0x00,0x00,0xFF,0x00,0x00,0x00,0x00};
static const uint8_t kExtCross[DOS_GLYPH_H]       = /* 0xC5 ┼ */
    {0x18,0x18,0x18,0xFF,0x18,0x18,0x18,0x18};
static const uint8_t kExtDoubleHLine[DOS_GLYPH_H] = /* 0xCD ═ */
    {0x00,0x00,0xFF,0x00,0x00,0xFF,0x00,0x00};
static const uint8_t kExtCornerBR[DOS_GLYPH_H]    = /* 0xD9 ┘ */
    {0x18,0x18,0x18,0xF8,0x00,0x00,0x00,0x00};
static const uint8_t kExtCornerTL[DOS_GLYPH_H]    = /* 0xDA ┌ */
    {0x00,0x00,0x00,0x1F,0x18,0x18,0x18,0x18};
static const uint8_t kExtFullBlock[DOS_GLYPH_H]   = /* 0xDB █ */
    {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
static const uint8_t kExtLowerHalf[DOS_GLYPH_H]   = /* 0xDC ▄ */
    {0x00,0x00,0x00,0x00,0xFF,0xFF,0xFF,0xFF};
static const uint8_t kExtLeftHalf[DOS_GLYPH_H]    = /* 0xDD ▌ */
    {0xF0,0xF0,0xF0,0xF0,0xF0,0xF0,0xF0,0xF0};
static const uint8_t kExtRightHalf[DOS_GLYPH_H]   = /* 0xDE ▐ */
    {0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F};
static const uint8_t kExtUpperHalf[DOS_GLYPH_H]   = /* 0xDF ▀ */
    {0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x00};

/* 按 CP437 码位返回 8x8 点阵。ASCII 可见区直接查表；控制字符画空格；
 * 上半区按类别回退到方块/框线点阵，其余回退 '?'。 */
static const uint8_t* glyph_for(uint8_t ch)
{
    if (ch >= 0x20u && ch <= 0x7Eu)
        return kAsciiGlyphs[ch - 0x20u];
    switch (ch) {
    case 0xB0u: return kExtShadeLight;
    case 0xB1u: return kExtShadeMedium;
    case 0xB2u: return kExtShadeDark;
    case 0xB3u: return kExtVLine;
    case 0xB4u: return kExtTeeRight;
    case 0xB9u: return kExtTeeRight;    /* ╣ 近似 */
    case 0xBAu: return kExtDoubleVLine;
    case 0xBBu: return kExtCornerTR;    /* ╗ 近似 */
    case 0xBCu: return kExtCornerBR;    /* ╝ 近似 */
    case 0xBFu: return kExtCornerTR;
    case 0xC0u: return kExtCornerBL;
    case 0xC1u: return kExtTeeUp;
    case 0xC2u: return kExtTeeDown;
    case 0xC3u: return kExtTeeLeft;
    case 0xC4u: return kExtHLine;
    case 0xC5u: return kExtCross;
    case 0xC8u: return kExtCornerBL;    /* ╚ 近似 */
    case 0xC9u: return kExtCornerTL;    /* ╔ 近似 */
    case 0xCAu: return kExtTeeUp;       /* ╩ 近似 */
    case 0xCBu: return kExtTeeDown;     /* ╦ 近似 */
    case 0xCCu: return kExtTeeLeft;     /* ╠ 近似 */
    case 0xCDu: return kExtDoubleHLine;
    case 0xCEu: return kExtCross;       /* ╬ 近似 */
    case 0xD9u: return kExtCornerBR;
    case 0xDAu: return kExtCornerTL;
    case 0xDBu: return kExtFullBlock;
    case 0xDCu: return kExtLowerHalf;
    case 0xDDu: return kExtLeftHalf;
    case 0xDEu: return kExtRightHalf;
    case 0xDFu: return kExtUpperHalf;
    default:
        /* 0x00..0x1F / 0x7F 控制字符画空格；其余扩展字符回退 '?'。 */
        if (ch < 0x20u || ch == 0x7Fu)
            return kAsciiGlyphs[0];
        return kAsciiGlyphs[0x3Fu - 0x20u];
    }
}

/* ---- 共享内容区包装 ----------------------------------------------------- */
struct DosSurface {
    uint32_t* pixels;   /* contentVA（内容区左上角）                */
    uint32_t  pitch;    /* 每扫描线像素数（surface stride，含阴影） */
    uint32_t  width;    /* 内容区宽（不越界到阴影/圆角）           */
    uint32_t  height;   /* 内容区高                                 */
};

/* 带边界裁剪的单个像素写入：越界直接丢弃，绝不污染 WM 的阴影边距。 */
static inline void surface_put(DosSurface* s, int32_t x, int32_t y, uint32_t color)
{
    if (!s || !s->pixels || x < 0 || y < 0)
        return;
    if ((uint32_t)x >= s->width || (uint32_t)y >= s->height)
        return;
    s->pixels[(uint64_t)(uint32_t)y * s->pitch + (uint32_t)x] = color;
}

/* 用内置 8x8 点阵画一行 ASCII 文本（逐像素直写，含背景色）。 */
static void draw_text8(DosSurface* s, int32_t x, int32_t y, const char* text,
                       uint32_t fg, uint32_t bg)
{
    if (!s || !text)
        return;
    int32_t col = 0;
    for (const char* p = text; *p != '\0'; ++p, ++col) {
        const int32_t bx = x + col * DOS_GLYPH_W;
        if (bx + DOS_GLYPH_W > (int32_t)s->width)
            break;  /* 右边界裁剪，不画穿内容区 */
        const uint8_t* glyph = glyph_for((uint8_t)*p);
        for (int32_t r = 0; r < DOS_GLYPH_H; ++r) {
            const uint8_t bits = glyph[r];
            for (int32_t c = 0; c < DOS_GLYPH_W; ++c) {
                const uint32_t color = (bits & (0x80u >> c)) ? fg : bg;
                surface_put(s, bx + c, y + r, color);
            }
        }
    }
}

/* 小端 8 字节读取（按字节组装，不依赖对齐与 libc memcpy）。 */
static uint64_t read_u64_le(const uint8_t* p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i)
        v = (v << 8) | (uint64_t)p[i];
    return v;
}

/* ---- 快照状态 ----------------------------------------------------------- */
enum ReadState : uint32_t {
    READ_INIT   = 0u,   /* 尚未读到任何一次 210 结果 */
    READ_VALID  = 1u,   /* 210 返回 4016，cells 可绘制 */
    READ_EAGAIN = 2u,   /* -EAGAIN：还没跑过 / 正在切换 */
    READ_ERROR  = 3u,   /* 其它负 errno */
    READ_SHORT  = 4u,   /* 非负但不是 4016（短读） */
};

struct Snapshot {
    ReadState state;
    long      read_rc;      /* syscall 210 原始返回值 */
    uint32_t  generation;
    uint32_t  flags;
    uint32_t  last_cpu;
    uint32_t  trips;
};

static bool snapshot_changed(const Snapshot* a, const Snapshot* b)
{
    if (!a || !b)
        return true;
    return a->state != b->state ||
           a->read_rc != b->read_rc ||
           a->generation != b->generation ||
           a->flags != b->flags ||
           a->last_cpu != b->last_cpu ||
           a->trips != b->trips;
}

/* ---- 文件级状态 --------------------------------------------------------- */
static volatile uint64_t* g_q = nullptr;
static FrameBuffer g_cb;                          /* 照抄 settings 的初始化 */
static uint8_t g_buf[DOS_BUFSZ] __attribute__((aligned(8))); /* 4016B，8 字节对齐 */
static Snapshot   g_snap;                         /* 零初始化：state=READ_INIT */
static char       g_status[192];
static long       g_enter_rc = 0;                 /* 最近一次 syscall 211 返回码 */
static bool       g_need_redraw = false;
static bool       g_quit = false;

/* 串口诊断：门禁/回归用一行 [dos] init 定位窗口是否成功启动。 */
static void dos_log_init(uint32_t contentW, uint32_t contentH, uint32_t pitch)
{
    char buf[176];
    int n = snprintf(buf, sizeof(buf),
                     "[dos] init: content=%ux%u pitch=%u 80x25 buf=%u "
                     "syscall_read=210 syscall_enter=211\n",
                     contentW, contentH, pitch, (unsigned)DOS_BUFSZ);
    if (n > 0) {
        if (n > (int)sizeof(buf) - 1)
            n = (int)sizeof(buf) - 1;
        syscall(SYS_DBGSOUT, (uint64_t)(uintptr_t)buf, (uint64_t)n,
                0, 0, 0, 0);
    }
}

/* .COM 映像暂存区：48 KiB 放 bss，不占 elf 文件体积。1 = 已装载，
 * -1 = 没有可装载的文件（继续用内核内置 banner），0 = 还没试过。 */
static char       g_com_img[DOS_COM_MAX] __attribute__((aligned(16)));
static int8_t     g_com_load = 0;

/* 依次尝试的映像路径。os.pkg 把示例 .COM 装在 /mp/system/dos/ 下，
 * /mp/boot.com 留给用户手动替换；两者都没有就回到内核 banner。 */
static const char* const kBootCandidates[] = {
    DOS_COM_PATH,               /* /mp/boot.com */
    "/mp/system/dos/hello.com",
    "/mp/system/boot.com",
    "/mp/dos/boot.com",
};
static const size_t kBootCandidateCount =
    sizeof(kBootCandidates) / sizeof(kBootCandidates[0]);

/* syscall 212：把 /mp/boot.com 交给内核的 .COM 加载区。只尝试一次 ——
 * 没有这个文件就保持旧行为，让内核跑它自己的 banner 演示。映像由内核在
 * 低内存 0x71100 摆好，并建出 PSP（+0 = int 20h，+2 = 段顶，+0x80 = 命令尾）。*/
static void stage_com_once(void)
{
    if (g_com_load != 0) return;
    g_com_load = -1;

    for (size_t i = 0; i < kBootCandidateCount; i++) {
        const char* path = kBootCandidates[i];
        FILE* f = fopen(path, "rb");
        if (!f) continue;

        const size_t n = fread(g_com_img, 1, DOS_COM_MAX, f);
        fclose(f);
        if (n == 0) continue;

        const long rc = (long)syscall(DOS_SYSCALL_STAGE,
                                      (uint64_t)(uintptr_t)g_com_img,
                                      (uint64_t)n, 0, 0, 0, 0);
        char b[112];
        if (rc == 0) {
            g_com_load = 1;
            const int k = snprintf(b, sizeof(b),
                                   "[dos] staged %s: %u bytes (syscall 212)\n",
                                   path, (unsigned)n);
            if (k > 0)
                syscall(SYS_DBGSOUT, (uint64_t)(uintptr_t)b, (uint64_t)k,
                        0, 0, 0, 0);
            return;
        }
        const int k = snprintf(b, sizeof(b), "[dos] stage %s failed rc=%ld\n",
                               path, rc);
        if (k > 0)
            syscall(SYS_DBGSOUT, (uint64_t)(uintptr_t)b, (uint64_t)k,
                    0, 0, 0, 0);
    }
}

/* ---------------------------------------------------------------------------
 * 低内存窗口（syscall 214）。16 位程序的映像、PSP、挂起上下文和文件请求块
 * 都住在那 64 KiB 里，用户态看不见物理内存，这是唯一通道。linear 是实模式
 * 线性地址（段 * 16 + 偏移），内核只接受 0x70000..0x80000 这一段。
 * ------------------------------------------------------------------------- */
static bool dos_low_move(void* buf, uint32_t linear, uint32_t n, bool to_low)
{
    if (n == 0 || n > 512u) return false;
    if (linear < DOS_LOW_BASE) return false;
    const uint32_t off = linear - DOS_LOW_BASE;
    if (off + n > 0x10000u) return false;
    const long rc = (long)syscall(DOS_SYSCALL_MEM, (uint64_t)off,
                                  (uint64_t)(uintptr_t)buf, (uint64_t)n,
                                  to_low ? 1u : 0u, 0, 0);
    return rc == 0;
}

/* 宿主句柄表。0 = 空，句柄号对外是 slot + 1。 */
static FILE* g_fh[8];

/* 从 DS:DX 读一个以 NUL 结尾的路径。 */
static bool dos_read_cstr(uint16_t seg, uint16_t off, char* out, size_t cap)
{
    uint8_t raw[96];
    const uint32_t lin = (uint32_t)seg * 16u + (uint32_t)off;
    if (!dos_low_move(raw, lin, (uint32_t)sizeof(raw), false)) return false;
    size_t i = 0;
    for (; i < sizeof(raw) && i + 1 < cap; i++) {
        const char c = (char)raw[i];
        if (c == '\0') break;
        out[i] = c;
    }
    if (i >= sizeof(raw)) return false;      /* 截断且没看到终止符 */
    out[i] = '\0';
    return i > 0;
}

/* "C:\DOS\A.TXT" -> "/mp/DOS/A.TXT"；盘符和开头的分隔符直接丢掉。 */
static bool dos_to_host_path(const char* in, char* out, size_t cap)
{
    const char* p = in;
    if (p[0] && p[1] == ':') p += 2;
    while (*p == '\\' || *p == '/') p++;
    if (!*p) return false;

    char body[112];
    size_t n = 0;
    for (; *p && n + 1 < sizeof(body); p++) {
        char c = *p;
        if (c == '\\') c = '/';
        body[n++] = c;
    }
    body[n] = '\0';
    snprintf(out, cap, "/mp/%s", body);
    return true;
}

/* 把挂起的文件请求跑完：读请求块 -> 对真 VFS 执行 -> 写回 AX/CF 并把
 * S_FSACT 置 2（表示"结果就绪"），下一次 com_resume 会直接从帧里取用。 */
static void dos_serve_fs(void)
{
    struct {
        uint16_t act, func, hand, cnt, off, sseg, res, cf, err;
    } r;
    if (!dos_low_move(&r, DOS_FS_BLOCK, (uint32_t)sizeof(r), false)) return;
    if (r.act != 1) return;

    uint16_t ax = 1, cf = 1;
    const uint16_t fn  = (uint16_t)(r.func >> 8);
    const int      idx = (int)r.hand - 1;

    switch (fn) {
    case 0x3C:   /* create */
    case 0x3D: { /* open */
        char dos[96];
        char host[128];
        if (dos_read_cstr(r.sseg, r.off, dos, sizeof(dos)) &&
            dos_to_host_path(dos, host, sizeof(host))) {
            FILE* f = fopen(host, (fn == 0x3C) ? "wb" : "rb");
            if (!f && fn == 0x3D) {
                /* VFS 多半是大小写不敏感的；退一步把名字压成小写再试。 */
                for (size_t i = 0; host[i]; i++)
                    if (host[i] >= 'A' && host[i] <= 'Z')
                        host[i] = (char)(host[i] + 32);
                f = fopen(host, "rb");
            }
            if (f) {
                int slot = -1;
                for (int i = 0; i < 8; i++)
                    if (!g_fh[i]) { slot = i; break; }
                if (slot >= 0) {
                    g_fh[slot] = f;
                    ax = (uint16_t)(slot + 1);
                    cf = 0;
                } else {
                    fclose(f);        /* 句柄表满 */
                }
            }
        }
        break;
    }
    case 0x3E:   /* close */
        if (idx >= 0 && idx < 8 && g_fh[idx]) {
            fclose(g_fh[idx]);
            g_fh[idx] = nullptr;
            ax = 0;
            cf = 0;
        }
        break;
    case 0x3F: { /* read: file -> DS:DX */
        if (idx >= 0 && idx < 8 && g_fh[idx] && r.cnt <= 512) {
            uint8_t tmp[512];
            const size_t got = fread(tmp, 1, (size_t)r.cnt, g_fh[idx]);
            const uint32_t dst = (uint32_t)r.sseg * 16u + (uint32_t)r.off;
            if (got == 0 || dos_low_move(tmp, dst, (uint32_t)got, true)) {
                ax = (uint16_t)got;
                cf = 0;
            }
        }
        break;
    }
    case 0x40: { /* write: DS:DX -> file */
        if (idx >= 0 && idx < 8 && g_fh[idx] && r.cnt <= 512) {
            uint8_t tmp[512];
            const uint32_t src = (uint32_t)r.sseg * 16u + (uint32_t)r.off;
            if (dos_low_move(tmp, src, (uint32_t)r.cnt, false)) {
                const size_t put = fwrite(tmp, 1, (size_t)r.cnt, g_fh[idx]);
                ax = (uint16_t)put;
                cf = 0;
            }
        }
        break;
    }
    default:
        /* 0x41 delete / 0x42 lseek：宿主 VFS 没有对应操作，回 CF=1。 */
        break;
    }

    r.res = ax;
    r.cf  = cf;
    r.err = 0;
    r.act = 2;                      /* 结果就绪 */
    dos_low_move(&r.res, DOS_FS_BLOCK + 12u, 2u, true);
    dos_low_move(&r.cf,  DOS_FS_BLOCK + 14u, 2u, true);
    dos_low_move(&r.act, DOS_FS_BLOCK +  0u, 2u, true);
}

/* syscall 211：让内核自动挑一个非 BSP 的 AP 做 long->real->long 往返。
 * 回来时如果程序挂起等文件，就地服务并再进一次；挂起等键则直接返回，
 * 等键盘事件到来再进 —— 绝不在这里忙等。 */
static void trigger_enter(void)
{
    stage_com_once();

    long rc = (long)syscall(DOS_SYSCALL_ENTER, 0, 0, 0, 0, 0, 0);
    for (int guard = 0; guard < 32 && rc == DOS_RC_NEED_FS; guard++) {
        dos_serve_fs();
        rc = (long)syscall(DOS_SYSCALL_ENTER, 0, 0, 0, 0, 0, 0);
    }

    g_enter_rc = rc;
    g_need_redraw = true;   /* 状态栏要立刻显示本次返回码 */
}

/* 把一个按键塞进 16 位键盘队列；返回是否真的接受。 */
static bool dos_push_key(uint16_t key)
{
    if (key == 0 || key >= 0x100u) return false;
    uint16_t k = key;               /* (scan << 8) | ascii, scan 未知填 0 */
    const long rc = (long)syscall(DOS_SYSCALL_KEYS, (uint64_t)(uintptr_t)&k,
                                  1, 0, 0, 0, 0);
    return rc == 0;
}

/* syscall 210：读 4016B 快照；只有状态/header 变化才置脏。 */
static void poll_snapshot(void)
{
    Snapshot now;
    now.state      = READ_INIT;
    now.read_rc    = 0;
    now.generation = 0;
    now.flags      = 0;
    now.last_cpu   = 0;
    now.trips      = 0;

    now.read_rc = (long)syscall(DOS_SYSCALL_READ,
                                (uint64_t)(uintptr_t)g_buf,
                                (uint64_t)DOS_BUFSZ,
                                0, 0, 0, 0);
    if (now.read_rc == (long)DOS_BUFSZ) {
        /* 只有完整 4016B 才算有效快照；按契约解析 2 x uint64 header。 */
        const uint64_t h0 = read_u64_le(g_buf);
        const uint64_t h1 = read_u64_le(g_buf + 8);
        now.state      = READ_VALID;
        now.generation = (uint32_t)(h0 & 0xFFFFFFFFull);
        now.flags      = (uint32_t)(h0 >> 32);
        now.last_cpu   = (uint32_t)(h1 & 0xFFFFFFFFull);
        now.trips      = (uint32_t)(h1 >> 32);
    } else if (now.read_rc == -(long)DOS_EAGAIN) {
        now.state = READ_EAGAIN;
    } else if (now.read_rc < 0) {
        now.state = READ_ERROR;
    } else {
        now.state = READ_SHORT;
    }

    if (snapshot_changed(&g_snap, &now)) {
        g_snap = now;
        g_need_redraw = true;
    }
}

/* 按当前读状态生成状态栏文本（纯 ASCII，用位图字体绘制）。 */
static void update_status_text(void)
{
    switch (g_snap.state) {
    case READ_VALID:
        snprintf(g_status, sizeof(g_status),
                 "CPU %u | trip %u | gen %u | %s%s%s | enter(211)=%ld",
                 g_snap.last_cpu, g_snap.trips, g_snap.generation,
                 (g_snap.flags & DOS_FLAG_VALID)   ? "VALID "   : "",
                 (g_snap.flags & DOS_FLAG_RUNNING) ? "RUNNING " : "",
                 (g_snap.flags & DOS_FLAG_REAL_OK) ? "REAL_OK " : "",
                 g_enter_rc);
        break;
    case READ_EAGAIN:
        snprintf(g_status, sizeof(g_status),
                 "waiting for real-mode trip... | read(210)=%ld | enter(211)=%ld",
                 g_snap.read_rc, g_enter_rc);
        break;
    case READ_SHORT:
        snprintf(g_status, sizeof(g_status),
                 "short read %ld of %u | enter(211)=%ld",
                 g_snap.read_rc, (unsigned)DOS_BUFSZ, g_enter_rc);
        break;
    case READ_ERROR:
        snprintf(g_status, sizeof(g_status),
                 "read(210) error %ld (errno %ld) | enter(211)=%ld",
                 g_snap.read_rc, -g_snap.read_rc, g_enter_rc);
        break;
    case READ_INIT:
    default:
        snprintf(g_status, sizeof(g_status),
                 "DOS 80x25 | initialising... | enter(211)=%ld",
                 g_enter_rc);
        break;
    }
}

/* 重绘整屏并在末尾提交一帧（只在调用方确认"真的需要重绘"时调用）。 */
static void render(void)
{
    DosSurface s;
    s.pixels = (uint32_t*)g_cb.BaseAddress;
    s.pitch  = (uint32_t)g_cb.PixelsPerScanLine;
    s.width  = (uint32_t)g_cb.Width;
    s.height = (uint32_t)g_cb.Height;
    if (!s.pixels || !s.pitch || !s.width || !s.height)
        return;

    const uint32_t black = kVgaPalette[0];
    /* 状态栏刻意不用蓝底白字：实模式 payload 的 banner 行用的正是 VGA
     * 属性 0x1F（白字蓝底）。两者同色且仅相隔 8px 时视觉上连成一片，
     * 实测截图里会被误读成"文字镜像/重叠"。改成深灰底 + 亮青字，
     * 与文本屏明确分层。 */
    const uint32_t barBg = 0xFF23232Au;      /* 深灰底 */
    const uint32_t barFg = 0xFF7CE7FFu;      /* 亮青字 */
    const uint32_t hintFg = kVgaPalette[14]; /* 黄字提示 */

    /* 1) 整屏填黑（逐行批量写，比逐像素 surface_put 便宜）。 */
    for (uint32_t y = 0; y < s.height; ++y) {
        uint32_t* row = s.pixels + (uint64_t)y * s.pitch;
        for (uint32_t x = 0; x < s.width; ++x)
            row[x] = black;
    }

    /* 2) 布局：640x200 文本屏 + 8px 状态栏，整体在内容区居中；内容区不足时
     *    从 (0,0) 开始、由 surface_put 裁剪底部（绝不越界写共享页）。 */
    const int32_t totalH = DOS_SCREEN_H + DOS_STATUS_H;
    int32_t ox = ((int32_t)s.width - (int32_t)DOS_SCREEN_W) / 2;
    int32_t oy = ((int32_t)s.height - totalH) / 2;
    if (ox < 0) ox = 0;
    if (oy < 0) oy = 0;
    const int32_t screenY = oy + DOS_STATUS_H;

    /* 3) 顶部状态栏：蓝底 + 白字。 */
    for (int32_t r = 0; r < DOS_STATUS_H; ++r)
        for (int32_t x = 0; x < (int32_t)s.width; ++x)
            surface_put(&s, x, oy + r, barBg);
    draw_text8(&s, ox, oy, g_status, barFg, barBg);

    /* 4) 80x25 cells：char/attr 交替、行优先；每个 cell 8x8 逐像素直写，
     *    前景/背景都取自 VGA16 调色板（attr 低 4 位前景、高 4 位背景）。 */
    if (g_snap.state == READ_VALID) {
        const uint8_t* cells = g_buf + DOS_HDR_BYTES;
        for (uint32_t row = 0; row < DOS_ROWS; ++row) {
            for (uint32_t col = 0; col < DOS_COLS; ++col) {
                const uint32_t off = (row * DOS_COLS + col) * 2u;
                const uint8_t ch   = cells[off];
                const uint8_t attr = cells[off + 1u];
                const uint32_t fg  = kVgaPalette[attr & 0x0Fu];
                const uint32_t bg  = kVgaPalette[(attr >> 4) & 0x0Fu];
                const uint8_t* glyph = glyph_for(ch);
                const int32_t bx = ox + (int32_t)(col * DOS_GLYPH_W);
                const int32_t by = screenY + (int32_t)(row * DOS_GLYPH_H);
                for (int32_t r = 0; r < DOS_GLYPH_H; ++r) {
                    const uint8_t bits = glyph[r];
                    for (int32_t c = 0; c < DOS_GLYPH_W; ++c) {
                        const uint32_t color =
                            (bits & (0x80u >> c)) ? fg : bg;
                        surface_put(&s, bx + c, by + r, color);
                    }
                }
            }
        }
    } else {
        /* 还没拿到有效快照：在文本屏中央画一行醒目提示。 */
        const char* hint = (g_snap.state == READ_EAGAIN)
                               ? "waiting for real-mode trip..."
                               : g_status;
        draw_text8(&s, ox + 8, screenY + 96, hint, hintFg, black);
    }

    /* 5) 帧提交：与 settings/notepad 一致，让 WM 立刻重合成而不是等 2Hz
     *    兜底。调用方保证只有真的重绘才走到这里。 */
    if (g_q)
        __atomic_add_fetch(&g_q[SKYWIN_PROTO_OUT_SEQ], 1u, __ATOMIC_RELEASE);
}

/* ---- 键盘：焦点门控 + 事件环消费（照抄 settings 的写法） ---------------- */
static void handle_key(uint16_t key)
{
    /* 程序挂起等键时，除 Esc（关窗）以外的按键先交给 16 位侧：在程序里
     * Enter、Backspace、字符输入才有和真实 DOS 一致的语义。 */
    if (g_enter_rc == DOS_RC_SUSPENDED && key != 0x1B) {
        if (dos_push_key(key)) {
            trigger_enter();
            poll_snapshot();
            return;
        }
    }

    switch (key) {
    case 0x1B:  /* Esc */
        g_quit = true;
        break;
    case '\n':
    case '\r':
    case ' ':
    case 'r':
    case 'R':
        /* 触发一次实模式往返，并立即读回快照，让画面尽快更新。 */
        trigger_enter();
        poll_snapshot();
        break;
    default:
        break;
    }
}

int main()
{
    /* WM 在启动前已填充协议页：内容区 VA + 几何（不握手）。 */
    g_q = (volatile uint64_t*)SKYWIN_PROTO_PAGE_VA;
    const uint64_t contentVA = g_q[SKYWIN_PROTO_CONTENT_VA];
    const uint32_t pitch     = (uint32_t)g_q[SKYWIN_PROTO_PITCH];
    const uint32_t contentW  = (uint32_t)g_q[SKYWIN_PROTO_CONTENT_W];
    const uint32_t contentH  = (uint32_t)g_q[SKYWIN_PROTO_CONTENT_H];
    if (!contentVA || !contentW || !contentH || !pitch)
        return 1;
    dos_log_init(contentW, contentH, pitch);

    /* 共享键盘事件环 + 本进程读者槽（WM 按点击把焦点指向该槽）。 */
    const uint64_t kbdVA = syscall(SYSCALL_DEV_MMAP, (uint64_t)KBD_DEV_TYPE,
                                   0, 0, 0, 0, 0);
    if ((int64_t)kbdVA <= 0)
        return 1;
    KbdShared* kbd = (KbdShared*)kbdVA;
    const int kbdSlot = kbd_reader_register(kbd, (int32_t)sys_getpid());
    uint64_t cursor = kbdSlot >= 0 ? kbd_reader_pos(kbd, kbdSlot) : 0;

    /* 与 settings 相同的 FrameBuffer 构造；之后直接写 uint32_t* 像素。 */
    g_cb.BaseAddress       = (void*)contentVA;
    g_cb.BufferSize        = g_q[SKYWIN_PROTO_CONTENT_SZ];
    g_cb.PixelsPerScanLine = pitch;      /* surface stride（含阴影边距） */
    g_cb.Width             = contentW;   /* 只画内容区宽度 */
    g_cb.Height            = contentH;

    /* 首帧：先读一次（大概率 -EAGAIN），再自动触发一次 211 让内核跑一趟
     * 实模式，再读一次拿新 generation，然后画首帧并置 READY。这样打开窗口
     * 就能看到 DOS 内容；即便 210/211 尚未注册也只是显示错误码，不会挂起。 */
    poll_snapshot();
    trigger_enter();
    poll_snapshot();
    update_status_text();
    render();                 /* 内部提交一次 OUT_SEQ */
    g_need_redraw = false;
    g_q[SKYWIN_PROTO_CLIENT_READY] = SKYWIN_READY_MAGIC;

    uint64_t last_poll_ms =
        (uint64_t)syscall(SYSCALL_MONOTONIC_MS, 0, 0, 0, 0, 0, 0);

    for (;;) {
        /* 键盘事件：只在焦点允许时处理 DOWN 事件。 */
        const uint64_t h = __atomic_load_n(&kbd->head, __ATOMIC_ACQUIRE);
        cursor = kbd_reader_resync(kbd, cursor);
        const bool allowed = (kbdSlot < 0) || kbd_focus_allows(kbd, kbdSlot);
        while (cursor < h) {
            const KbdEvent e = kbd->ring[cursor & (KBD_RING_CAP - 1u)];
            if (allowed && e.action == KBD_ACTION_DOWN)
                handle_key(e.key);
            cursor++;
        }
        if (kbdSlot >= 0)
            kbd_reader_setpos(kbd, kbdSlot, cursor);
        if (g_quit)
            break;

        /* ~100ms 节流轮询 210：generation/flags/cpu/trips 不变就不重绘，
         * 避免整屏 128000 像素的空转刷屏。 */
        const uint64_t now_ms =
            (uint64_t)syscall(SYSCALL_MONOTONIC_MS, 0, 0, 0, 0, 0, 0);
        if (now_ms - last_poll_ms >= DOS_POLL_INTERVAL_MS) {
            last_poll_ms = now_ms;
            poll_snapshot();
        }

        if (g_need_redraw) {
            update_status_text();
            render();         /* 内部递增 OUT_SEQ 提交这一帧 */
            g_need_redraw = false;
        }
        sys_yield();          /* 事件驱动：空闲时让出 CPU */
    }

    if (kbdSlot >= 0)
        kbd_reader_unregister(kbd, kbdSlot);
    return 0;
}
