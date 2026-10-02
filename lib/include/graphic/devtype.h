//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT
/* P5-97: 设备类型编号单一权威 —— 内核 drivers/dev/dev.h 的
   VsDevType 枚举 (FrameBuffer=6, X86_PS2_MOUSE=7, X86_KEYBOARD=9)。
   用户态任何 dev_mmap/dev_ioctl 的 DevType 参数一律引用此处,
   禁止再散落裸数字。 */
#ifndef _DEVTYPE_H_
#define _DEVTYPE_H_

#define DEV_TYPE_FRAMEBUFFER   6u
#define DEV_TYPE_PS2_MOUSE     7u
#define DEV_TYPE_KEYBOARD      9u

#endif
