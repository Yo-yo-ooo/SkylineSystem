//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * kernel/include/kmod/contracts/input.h —— 私有契约 kernel.input.v1（草案）
 * ============================================================================
 * 状态：**草案（draft）**。B4 批次不产出该契约的 provider 组件：现有输入核心
 * 仍是 kernel/src/drivers/input/kbd.cpp 的 Kbd::Post/Kbd::Init，被
 * keyboard_init()（PS/2）与 USB HID 回调直接调用（见 init.cpp:238-239）。
 * 本头是给后续 Tier1 组件（kernel.input.core / kernel.input.ps2.kbd /
 * kernel.input.ps2.mouse / kernel.input.usb.hid）的固定机制边界：
 *   - provider 组件实现 kmod_input_ops（事件发布 + 丢失计数）；
 *   - 具体驱动组件（PS/2 ISR、USB HID 回调）导入本契约，把原生报告翻译成
 *     post_key()/post_mouse() 调用，不再直接依赖 Kbd 命名空间；
 *   - 与用户态的共享 ABI 是 lib/include/graphic/kbdshare.h 的事件环（不变）。
 *
 * 与 kbdshare.h 的对齐（事件语义不变）：
 *   - code：可打印键 = ASCII，控制键 = 0x0A/0x09/0x08/0x1B，功能键 = 0x100+；
 *     KBD_KEY_* 数值与 kbdshare.h 完全一致；
 *   - action：0 = UP，1 = DOWN（KBD_ACTION_*）；
 *   - mods：KBD_MOD_* 位掩码。
 *
 * 布局冻结：首部固定 { uint32_t version; uint32_t size; }，static_assert 钉死。
 * ==========================================================================*/
#ifndef _KMOD_CONTRACTS_INPUT_H_
#define _KMOD_CONTRACTS_INPUT_H_

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KMOD_CONTRACT_INPUT_V1  "kernel.input.v1"

#define KMOD_INPUT_OPS_VERSION  1u
#define KMOD_INPUT_EVENT_VERSION 1u

#define KMOD_INPUT_SRC_KEYBOARD 1u
#define KMOD_INPUT_SRC_MOUSE    2u

#define KMOD_INPUT_ACTION_UP    0u
#define KMOD_INPUT_ACTION_DOWN  1u

/* 修饰键位（与 lib/include/graphic/kbdshare.h 的 KBD_MOD_* 一致） */
#define KMOD_INPUT_MOD_LSHIFT   0x01u
#define KMOD_INPUT_MOD_RSHIFT   0x02u
#define KMOD_INPUT_MOD_LCTRL    0x04u
#define KMOD_INPUT_MOD_RCTRL    0x08u
#define KMOD_INPUT_MOD_LALT     0x10u
#define KMOD_INPUT_MOD_RALT     0x20u
#define KMOD_INPUT_MOD_CAPS     0x40u

/* 鼠标按键位（v1 约定） */
#define KMOD_INPUT_BTN_LEFT     (1u << 0)
#define KMOD_INPUT_BTN_RIGHT    (1u << 1)
#define KMOD_INPUT_BTN_MIDDLE   (1u << 2)

/* 非 ASCII 功能键（与 kbdshare.h KBD_KEY_* 一致） */
#define KMOD_INPUT_KEY_LEFT   0x100u
#define KMOD_INPUT_KEY_RIGHT  0x101u
#define KMOD_INPUT_KEY_UP     0x102u
#define KMOD_INPUT_KEY_DOWN   0x103u
#define KMOD_INPUT_KEY_HOME   0x104u
#define KMOD_INPUT_KEY_END    0x105u
#define KMOD_INPUT_KEY_PGUP   0x106u
#define KMOD_INPUT_KEY_PGDN   0x107u
#define KMOD_INPUT_KEY_INS    0x108u
#define KMOD_INPUT_KEY_DEL    0x109u

/* ------------------------------------------------------------------ */
/* 归一化输入事件（跨驱动/跨组件固定结构）。布局冻结：32 字节。       */
/* ------------------------------------------------------------------ */
typedef struct kmod_input_event {
    uint32_t version;                       /* = KMOD_INPUT_EVENT_VERSION */
    uint32_t size;                          /* = sizeof(kmod_input_event_t) */
    uint32_t source;                        /* KMOD_INPUT_SRC_* */
    uint16_t code;                          /* key/btn code（语义见文件头） */
    uint8_t action;                         /* KMOD_INPUT_ACTION_* */
    uint8_t mods;                           /* KMOD_INPUT_MOD_* 位掩码 */
    int32_t x;                              /* 鼠标绝对/累计坐标（键盘填 0） */
    int32_t y;
    uint32_t seq;                           /* 生产者单调序号（丢帧检测用） */
    uint32_t reserved;                      /* 必须为 0 */
} kmod_input_event_t;

/* ------------------------------------------------------------------ */
/* 输入核心契约 ops。布局冻结：40 字节（8 + 4 个函数指针）。          */
/* post_*：驱动在 ISR/回调里发布事件（不得阻塞）；返回 0 = 已入环。    */
/* lost_count：事件环因消费者过慢被覆盖的累计事件数。                  */
/* init：输入核心自检/注册（幂等；provider 组件 init 里调用一次即可）。*/
/* ------------------------------------------------------------------ */
typedef struct kmod_input_ops {
    uint32_t version;                       /* = KMOD_INPUT_OPS_VERSION */
    uint32_t size;                          /* = sizeof(kmod_input_ops_t) */
    int32_t (*post_key)(uint16_t key, uint8_t action, uint8_t mods);
    int32_t (*post_mouse)(int32_t x, int32_t y, uint8_t buttons,
                          uint8_t action);
    int32_t (*init)(void);
    uint64_t (*lost_count)(void);
} kmod_input_ops_t;

#if defined(__cplusplus)
static_assert(sizeof(kmod_input_event_t) == 32u,
              "kmod_input_event_t layout must stay 32 bytes (kernel.input.v1)");
static_assert(sizeof(kmod_input_ops_t) == 40u,
              "kmod_input_ops_t layout must stay 40 bytes (kernel.input.v1)");
#endif

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* _KMOD_CONTRACTS_INPUT_H_ */
