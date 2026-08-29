//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
#pragma once
#ifndef _X86_64_SCHEDULE_SIGNAL_H
#define _X86_64_SCHEDULE_SIGNAL_H

#include <stdint.h>
#include <stddef.h>
#include <klib/types.h>
#include <arch/x86_64/schedule/ssignal.h>

enum SigType : uint32_t{
    SIG_CPU_EXCEPTION           = 0,
    SIG_CPU_INTURRUPT           = 1,
    SIG_PROC_COMMUNICATE        = 2
};

namespace Schedule{
    namespace Signal{
        void SigRegister(SigType Type, void (*SIGHandler)(SigType Type));
    }
} // namespace Schedule



#endif// _X86_64_SCHEDULE_SIGNAL_H