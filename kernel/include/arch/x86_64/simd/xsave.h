/*
* SPDX-License-Identifier: GPL-2.0-only
* File: xsave.h
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
#ifndef _x86_64_SIMD_XSAVE_H
#define _x86_64_SIMD_XSAVE_H

#include <stdint.h>
#include <stddef.h>
#include <pdef.h>
#include <arch/x86_64/asm/msr.h>
#include <arch/x86_64/asm/xcr.h>

extern "C++" {

typedef uint32_t xsave_feat_mask_t;

enum xsave_feature : uint8_t {
    XSAVE_FEAT_X87,
    XSAVE_FEAT_SSE,
    XSAVE_FEAT_AVX,
    XSAVE_FEAT_MPX_BNDREGS,
    XSAVE_FEAT_MPX_BNDCSR,
    XSAVE_FEAT_AVX_512_OPMASK,
    XSAVE_FEAT_AVX_512_ZMM_HI256,
    XSAVE_FEAT_AVX_512_HI16_ZMM,
    XSAVE_FEAT_PROC_TRACE,
    XSAVE_FEAT_PKRU,

    // Supervisor only
    XSAVE_FEAT_PASID,
    XSAVE_FEAT_CET_USER,
    XSAVE_FEAT_CET_SUPERVISOR,
    XSAVE_FEAT_HDC,
    XSAVE_FEAT_UINTR,
    XSAVE_FEAT_LBR,
    XSAVE_FEAT_HWP,

    // User only
    XSAVE_FEAT_AMX_TILECFG,
    XSAVE_FEAT_AMX_TILEDATA,

    XSAVE_FEAT_MAX = XSAVE_FEAT_AMX_TILEDATA
};

#define __XSAVE_FEAT_MASK(feat) (1ull << (feat))
#define XSAVE_FEATURE_MASK_FMT PRIx32

static inline void xsave_feat_disable(const enum xsave_feature feat) {
    wrmsr(IA32_MSR_XFD, rdmsr(IA32_MSR_XFD) | __XSAVE_FEAT_MASK(feat));
}

static inline void xsave_feat_enable(const enum xsave_feature feat) {
    wrmsr(IA32_MSR_XFD,
              rm_mask(rdmsr(IA32_MSR_XFD), __XSAVE_FEAT_MASK(feat)));
}


static inline void xsave_set_supervisor_features(const uint32_t features) {
    wrmsr(IA32_MSR_XSS, rdmsr(IA32_MSR_XSS) | features);
}


static inline void xsave_set_user_features(const uint32_t features) {
    const uint64_t xcr = read_xcr(XCR_XSTATE_FEATURES_ENABLED);
    write_xcr(XCR_XSTATE_FEATURES_ENABLED, xcr | features);
}

#define __XSAVE_FEAT_USER_MASK \
    (__XSAVE_FEAT_MASK(XSAVE_FEAT_X87) \
   | __XSAVE_FEAT_MASK(XSAVE_FEAT_SSE) \
   | __XSAVE_FEAT_MASK(XSAVE_FEAT_AVX) \
   | __XSAVE_FEAT_MASK(XSAVE_FEAT_MPX_BNDREGS) \
   | __XSAVE_FEAT_MASK(XSAVE_FEAT_MPX_BNDCSR) \
   | __XSAVE_FEAT_MASK(XSAVE_FEAT_AVX_512_OPMASK) \
   | __XSAVE_FEAT_MASK(XSAVE_FEAT_AVX_512_ZMM_HI256) \
   | __XSAVE_FEAT_MASK(XSAVE_FEAT_AVX_512_HI16_ZMM))

#define __XSAVE_FEAT_SUPERVISOR_MASK \
    (__XSAVE_FEAT_MASK(XSAVE_FEAT_PASID) \
   | __XSAVE_FEAT_MASK(XSAVE_FEAT_CET_USER))

// List of features that are disabled in XSTATE. User programs running these
// instructions will trap, and the xstate space will be enlarged.

#define XSAVE_FEAT_XFD_MASK \
    (__XSAVE_FEAT_MASK(XSAVE_FEAT_AVX_512_OPMASK) \
   | __XSAVE_FEAT_MASK(XSAVE_FEAT_AVX_512_ZMM_HI256) \
   | __XSAVE_FEAT_MASK(XSAVE_FEAT_AVX_512_HI16_ZMM) \
   | __XSAVE_FEAT_MASK(XSAVE_FEAT_AMX_TILECFG) \
   | __XSAVE_FEAT_MASK(XSAVE_FEAT_AMX_TILEDATA))


}

#endif