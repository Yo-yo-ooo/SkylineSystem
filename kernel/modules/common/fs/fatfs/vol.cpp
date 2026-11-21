//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
// SPDX-License-Identifier: GPL-2.0-only
/* FatFs volume→partition table. 原内核只有 extern 声明，ff.cpp 的
 * FF_MULTI_PARTITION 路径在没有消费者时被 GC；整体迁入 .ko 后需要实体。 */
#include <fs/fatfs/ff.h>

extern "C" PARTITION VolToPart[FF_VOLUMES] = { { 0, 0 } };
