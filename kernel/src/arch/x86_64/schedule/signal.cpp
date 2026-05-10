/*
* SPDX-License-Identifier: GPL-2.0-only
* File: signal.cpp
* Copyright (C) 2026 Yo-yo-ooo
*
* This file is part of SkylineSystem.
*
* SkylineSystem is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
*/
#include <arch/x86_64/allin.h>
#include <elf/elf.h>
#include <arch/x86_64/schedule/sched.h>
#include <arch/x86_64/interrupt/idt.h>
#include <arch/x86_64/smp/smp.h>
#include <arch/x86_64/schedule/syscall.h>
#include <arch/x86_64/vmm/vmm.h>
#include <arch/x86_64/simd/simd.h>
#include <klib/algorithm/queue.h>

namespace Schedule{
    namespace Signal{
        int32_t Raise(proc_t *process, int32_t signal){
            // Check if the signal is valid
            if (signal < 1 || signal > 64) {
                return -EINVAL;
            }
            sigaction_t action = process->sig_handlers[signal];
            if(action.handler != nullptr){
                action.handler(signal);
            }else{
                DefaultHandler(signal);
            }
            return 0;
        }

        void DefaultHandler(int32_t signal){
            switch(signal){
                case LINUX_SIGTERM:
                case LINUX_SIGKILL:
                    Schedule::Exit(0);
                    break;
                case LINUX_SIGCHLD:
                    // Ignore
                    break;
                default:
                    // For other signals, just ignore for now
                    break;
            }
        }
    }

}