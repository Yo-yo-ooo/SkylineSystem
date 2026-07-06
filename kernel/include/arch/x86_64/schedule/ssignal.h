//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
#pragma once
#ifndef _SKYLINE_SYSTEM_SIGNALS_H_
#define _SKYLINE_SYSTEM_SIGNALS_H_

#include <stdint.h>
#include <stddef.h>

/*
SkylineSystem SIGNALS Global Definition Specification
Abstract layered architecture: Hardware Device Layer / Kernel Scheduler Layer / IO Resource Layer
Window Interaction Layer / Process Fault Layer / User Custom Layer
All concrete hardware details are hidden, only generic abstract semantics are exposed
*/

// ========== 1. Hardware Device Abstract Signals (HD: Hardware Device Low-Level IRQ Events) ==========
/* [SS_SIG_HD] Generic hardware device events, no differentiation of keyboard/mouse/storage/display peripherals */
#define SS_SIG_HD_BASE         0
#define SS_SIG_HD_DEV_IN       (SS_SIG_HD_BASE + 0)  // Generic peripheral input event
#define SS_SIG_HD_DEV_OUT      (SS_SIG_HD_BASE + 1)  // Peripheral output completion event
#define SS_SIG_HD_DISP_SYNC    (SS_SIG_HD_BASE + 2)  // Display vertical synchronization signal
#define SS_SIG_HD_STORAGE_IO   (SS_SIG_HD_BASE + 3)  // Storage device IO completion event
#define SS_SIG_HD_COMM_TRANS   (SS_SIG_HD_BASE + 4)  // Generic bus communication transfer event
#define SS_SIG_HD_DEV_ERR      (SS_SIG_HD_BASE + 5)  // Hardware device error notification
#define SS_SIG_HD_CNT          6    // Total count of hardware signals

// ========== 2. Kernel Scheduler Abstract Signals (SCHED: Scheduler Timing & Lifecycle Events) ==========
/* [SS_SIG_SCHED] Thread timeslice, sleep & wakeup abstract scheduler events */
#define SS_SIG_SCHED_BASE      8
#define SS_SIG_SCHED_QUANTUM   (SS_SIG_SCHED_BASE + 0) // Timeslice expiration preemption notice
#define SS_SIG_SCHED_WAKEUP    (SS_SIG_SCHED_BASE + 1) // Blocked thread wakeup signal
#define SS_SIG_SCHED_CNT       2

// ========== 3. IO Resource Abstract Signals (IO: Synchronous Resource Operation Events) ==========
/* [SS_SIG_IO] Synchronous file/handle operation complete events, resource-agnostic abstraction */
#define SS_SIG_IO_BASE         10
#define SS_SIG_IO_READ_FIN     (SS_SIG_IO_BASE + 0) // Synchronous read operation finished
#define SS_SIG_IO_WRITE_FIN    (SS_SIG_IO_BASE + 1) // Synchronous write operation finished
#define SS_SIG_IO_HANDLE_CLOSE (SS_SIG_IO_BASE + 2) // IO resource handle closed notification
#define SS_SIG_IO_CNT          3

// ========== 4. Window Surface Abstract Signals (WIN: GUI Canvas Interaction Events) ==========
/* [SS_SIG_WIN] Generic window/canvas event, no direct low-level input exposure */
#define SS_SIG_WIN_BASE        13
#define SS_SIG_WIN_CLOSE_REQ   (SS_SIG_WIN_BASE + 0) // Canvas close request
#define SS_SIG_WIN_SIZE_CHG    (SS_SIG_WIN_BASE + 1) // Canvas dimension resize event
#define SS_SIG_WIN_FOCUS_SWAP  (SS_SIG_WIN_BASE + 2) // Canvas focus state switch
#define SS_SIG_WIN_REDRAW_TRIG (SS_SIG_WIN_BASE + 3) // Canvas refresh redraw trigger
#define SS_SIG_WIN_CNT         4

// ========== 5. Process Runtime Fault Abstract Signals (FAULT: User Process Runtime Exceptions) ==========
/* [SS_SIG_FAULT] Unified runtime error abstraction for memory, arithmetic & resource violations */
#define SS_SIG_FAULT_BASE      17
#define SS_SIG_FAULT_MEM_VIOL  (SS_SIG_FAULT_BASE + 0) // Illegal memory access violation
#define SS_SIG_FAULT_ARITH_ERR (SS_SIG_FAULT_BASE + 1) // Invalid arithmetic operation error
#define SS_SIG_FAULT_ILL_EXEC  (SS_SIG_FAULT_BASE + 2) // Illegal instruction execution
#define SS_SIG_FAULT_RES_LIMIT (SS_SIG_FAULT_BASE + 3) // Process resource exhaustion warning
#define SS_SIG_FAULT_CNT       4

// ========== 6. User Custom Abstract Signal Range (USER: Application Defined Business Events) ==========
/* [SS_SIG_USER] Reserved continuous signal range for user-space custom events, kernel does not interpret payload semantics */
#define SS_SIG_USER_BASE       21
#define SS_SIG_USER_CNT        4

// Global maximum signal index, compatible with uint64_t pending signal bitmask
#define SS_SIG_MAX_TOTAL      25

#endif