//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
#pragma once
#ifndef _X86_64_SCHEDULE_SIGNAL_H
#define _X86_64_SCHEDULE_SIGNAL_H

#include <stdint.h>
#include <stddef.h>
#include <klib/types.h>
#include <arch/x86_64/schedule/ssignal.h>

typedef void (*sig_handler_t)(uint32_t sig_num, void* data);

typedef struct sig_action{
    uint64_t SignalNum;
    sig_handler_t Handler;
}sig_action_t;



#endif// _X86_64_SCHEDULE_SIGNAL_H