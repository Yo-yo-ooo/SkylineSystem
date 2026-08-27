//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
// SPDX-License-Identifier: MIT
#include <stdint.h>
#include <syscall.h>
#include <stdlib.h>

/* ELF 4K 扩展区: hw2 自己视角的固定地址 */
#define ELF_SECTION_BASE 0x400000UL

/* 协议布局 (与 TLoad 写入端约定一致):
   proto[0] = 帧缓冲在 hw2 侧的 VA   (TLoad 填好后生效)
   proto[1] = 帧缓冲长度 (字节)      (现在就有值) */
#define PROTO_FB_ADDR   (*(volatile uint64_t*)(ELF_SECTION_BASE + 0))
#define PROTO_FB_LEN    (*(volatile uint64_t*)(ELF_SECTION_BASE + 8))

static void out_str(const char *s) {
    int len = 0;
    while (s[len] != '\0') len++;
    syscall(24, (long)s, len, 0, 0, 0, 0);
}

static void out_u64(const char *tag, uint64_t v) {
    out_str(tag);
    /* 简易 u64 → 十进制串 */
    char buf[21];
    int i = 20;
    buf[20] = '\0';
    if (v == 0) { buf[19] = '0'; out_str(&buf[19]); out_str("\n"); return; }
    while (v > 0 && i > 0) {
        buf[--i] = '0' + (char)(v % 10);
        v /= 10;
    }
    out_str(&buf[i]);
    out_str("\n");
}

int main() {
    out_str("=== hw2 START ===\n");

    /* 1. 读协议 — 证明 desktop 写的数据 hw2 能看见 */
    out_u64("fb_len  = ", PROTO_FB_LEN);
    out_u64("fb_addr = ", PROTO_FB_ADDR);

    if (PROTO_FB_LEN == 0) {
        out_str("protocol empty — desktop didn't write?\n");
        return 1;
    }

    /* 2. 如果 TLoad 已填 proto[0] (非零), 直接画测试图案 */
    if (PROTO_FB_ADDR != 0) {
        volatile uint32_t *fb = (volatile uint32_t*)PROTO_FB_ADDR;
        uint64_t npixels = PROTO_FB_LEN / 4;

        out_str("painting fb...\n");
        for (uint64_t i = 0; i < npixels; i++)
            fb[i] = 0x00FF00FF;          /* 整屏品红 — 画出来一眼可见 */

        out_str("fb painted!\n");
        return 0;
    }

    /* 3. proto[0] 还是 0 — 帧 buffer 地址未协议化。
       退而求其次: 把 desktop 之前 verify 遗留/写入的内容读出来证明
       双向可见, 并提示去填 proto[0] */
    out_str("fb_addr not set (proto[0]==0).\n");
    out_str("readback ELF section first 32 bytes:\n");
    volatile char *p = (volatile char*)ELF_SECTION_BASE;
    for (int i = 0; i < 32; i++) {
        char hex[3];
        hex[0] = "0123456789ABCDEF"[(p[i] >> 4) & 0xF];
        hex[1] = "0123456789ABCDEF"[p[i] & 0xF];
        hex[2] = ' ';
        syscall(24, (long)hex, 3, 0, 0, 0, 0);
    }
    out_str("\n");

    return 0;
}