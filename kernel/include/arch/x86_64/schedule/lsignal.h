/*
* SPDX-License-Identifier: GPL-2.0-only
* File: lsignal.h
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
#pragma once
#ifndef _LINUX_SIGNALS_H_
#define _LINUX_SIGNALS_H_

#include <stdint.h>
#include <stddef.h>

/*Linux signals (use kill -l to see list)
 1) SIGHUP       2) SIGINT       3) SIGQUIT      4) SIGILL       5) SIGTRAP
 6) SIGABRT      7) SIGBUS       8) SIGFPE       9) SIGKILL     10) SIGUSR1
11) SIGSEGV     12) SIGUSR2     13) SIGPIPE     14) SIGALRM     15) SIGTERM
16) SIGSTKFLT   17) SIGCHLD     18) SIGCONT     19) SIGSTOP     20) SIGTSTP
21) SIGTTIN     22) SIGTTOU     23) SIGURG      24) SIGXCPU     25) SIGXFSZ
26) SIGVTALRM   27) SIGPROF     28) SIGWINCH    29) SIGIO       30) SIGPWR
31) SIGSYS      34) SIGRTMIN    35) SIGRTMIN+1  36) SIGRTMIN+2  37) SIGRTMIN+3
38) SIGRTMIN+4  39) SIGRTMIN+5  40) SIGRTMIN+6  41) SIGRTMIN+7  42) SIGRTMIN+8
43) SIGRTMIN+9  44) SIGRTMIN+10 45) SIGRTMIN+11 46) SIGRTMIN+12 47) SIGRTMIN+13
48) SIGRTMIN+14 49) SIGRTMIN+15 50) SIGRTMAX-14 51) SIGRTMAX-13 52) SIGRTMAX-12
53) SIGRTMAX-11 54) SIGRTMAX-10 55) SIGRTMAX-9  56) SIGRTMAX-8  57) SIGRTMAX-7
58) SIGRTMAX-6  59) SIGRTMAX-5  60) SIGRTMAX-4  61) SIGRTMAX-3  62) SIGRTMAX-2
63) SIGRTMAX-1  64) SIGRTMAX
*/
/* constexpr char* signals_to_str[] = {
  "?","?","?","?","?","?","?","?","?","?",
  "SIGHUP","SIGINT","SIGQUIT","SIGILL","SIGTRAP","SIGABRT",
    "SIGBUS","SIGFPE","SIGKILL","SIGUSR1","SIGSEGV",
    "SIGUSR2","SIGPIPE","SIGALRM","SIGTERM","SIGSTKFLT",
    "SIGCHLD","SIGCONT","SIGSTOP","SIGTSTP","SIGTTIN",
    "SIGTTOU","SIGURG","SIGXCPU","SIGXFSZ","SIGVTALRM",
    "SIGPROF","SIGWINCH","SIGIO","SIGPWR","SIGSYS",

}; */
#define LINUX_SIGHUP       1
#define LINUX_SIGINT       2
#define LINUX_SIGQUIT      3
#define LINUX_SIGILL       4
#define LINUX_SIGTRAP      5
#define LINUX_SIGABRT      6
#define LINUX_SIGBUS       7
#define LINUX_SIGFPE       8
#define LINUX_SIGKILL      9
#define LINUX_SIGUSR1     10
#define LINUX_SIGSEGV     11
#define LINUX_SIGUSR2     12
#define LINUX_SIGPIPE     13
#define LINUX_SIGALRM     14
#define LINUX_SIGTERM     15
#define LINUX_SIGSTKFLT   16
#define LINUX_SIGCHLD     17
#define LINUX_SIGCONT     18
#define LINUX_SIGSTOP     19
#define LINUX_SIGTSTP     20
#define LINUX_SIGTTIN     21
#define LINUX_SIGTTOU     22
#define LINUX_SIGURG      23
#define LINUX_SIGXCPU     24
#define LINUX_SIGXFSZ     25
#define LINUX_SIGVTALRM   26
#define LINUX_SIGPROF     27
#define LINUX_SIGWINCH    28
#define LINUX_SIGIO       29
#define LINUX_SIGPWR      30
#define LINUX_SIGSYS      31
#define LINUX_SIGRTMIN    34
#define LINUX_SIGRTMIN_PLUS_1  35
#define LINUX_SIGRTMIN_PLUS_2  36
#define LINUX_SIGRTMIN_PLUS_3  37
#define LINUX_SIGRTMIN_PLUS_4  38
#define LINUX_SIGRTMIN_PLUS_5  39
#define LINUX_SIGRTMIN_PLUS_6  40
#define LINUX_SIGRTMIN_PLUS_7  41
#define LINUX_SIGRTMIN_PLUS_8  42
#define LINUX_SIGRTMIN_PLUS_9  43
#define LINUX_SIGRTMIN_PLUS_10 44
#define LINUX_SIGRTMIN_PLUS_11 45
#define LINUX_SIGRTMIN_PLUS_12 46
#define LINUX_SIGRTMIN_PLUS_13 47
#define LINUX_SIGRTMIN_PLUS_14 48
#define LINUX_SIGRTMIN_PLUS_15 49
#define LINUX_SIGRTMAX_MINUS_14 50
#define LINUX_SIGRTMAX_MINUS_13 51
#define LINUX_SIGRTMAX_MINUS_12 52
#define LINUX_SIGRTMAX_MINUS_11 53
#define LINUX_SIGRTMAX_MINUS_10 54
#define LINUX_SIGRTMAX_MINUS_9  55
#define LINUX_SIGRTMAX_MINUS_8  56
#define LINUX_SIGRTMAX_MINUS_7  57
#define LINUX_SIGRTMAX_MINUS_6  58
#define LINUX_SIGRTMAX_MINUS_5  59
#define LINUX_SIGRTMAX_MINUS_4  60
#define LINUX_SIGRTMAX_MINUS_3  61
#define LINUX_SIGRTMAX_MINUS_2  62
#define LINUX_SIGRTMAX_MINUS_1  63
#define LINUX_SIGRTMAX    64

#endif