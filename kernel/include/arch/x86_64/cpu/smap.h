// SPDX-FileCopyrightText: 2026 Yo-yo-ooo
// SPDX-License-Identifier: GPL-2.0-only
//
// x86_64 Supervisor Mode Access Prevention helpers.
//
// When CR4.SMAP is set, any CPL0 data access to a page whose U/S bit is 1
// (a user page) raises a page fault with the SMAP bit set in the error code.
// A kernel routine that legitimately has to dereference a *user virtual
// address (ELF stack/TLS population, user argv strings, ...) must open a
// short override window with STAC and restore RFLAGS.AC with CLAC. Bulk I/O
// goes through VMM::UserAccess, which touches the HHDM (supervisor) alias
// and therefore needs no override.
#pragma once
#include <stdint.h>

/* Set by enable_smep_smap() once CR4.SMAP is actually enabled. STAC/CLAC
   raise #UD on a CPU without SMAP, so the guard is gated on this flag. */
extern bool g_smap_enabled;

static inline void cpu_stac(void) { __asm__ __volatile__("stac" ::: "memory", "cc"); }
static inline void cpu_clac(void) { __asm__ __volatile__("clac" ::: "memory", "cc"); }
static inline bool cpu_ac_flag(void) {
    uint64_t f;
    __asm__ __volatile__("pushfq\n\tpop %0" : "=r"(f) :: "memory");
    return (f >> 18) & 1;                 /* RFLAGS.AC is bit 18 */
}

/* RAII override window. Re-entrant: the previous AC state is saved and
   restored on destruction, so a guard nested inside an outer guard does
   not close the outer window early. Non-copyable, scope-local. */
class SmapGuard {
    bool enabled_;
    bool prev_ac_;
public:
    SmapGuard() {
        enabled_ = g_smap_enabled;
        if (enabled_) { prev_ac_ = cpu_ac_flag(); cpu_stac(); }
        else prev_ac_ = false;
    }
    ~SmapGuard() {
        if (enabled_) { if (prev_ac_) cpu_stac(); else cpu_clac(); }
    }
    SmapGuard(const SmapGuard&) = delete;
    SmapGuard& operator=(const SmapGuard&) = delete;
};
