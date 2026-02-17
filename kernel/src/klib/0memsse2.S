#ifdef __x86_64__

// Copyright (c) 2022
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
// * Redistributions of source code must retain the above copyright notice, this
//   list of conditions and the following disclaimer.
// * Redistributions in binary form must reproduce the above copyright notice,
//   this list of conditions and the following disclaimer in the documentation
//   and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER BE LIABLE FOR ANY
// DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
// ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
// 'portable' memcmp; uses sse2, so should work on all/any amd64

/*
* SPDX-License-Identifier: GPL-2.0-only
* File: 0memsse2.S
* Copyright (C) 2026 Yo-yo-ooo
*
* This file is part of SkylineSystem.
* This file's author cannot found so if you know the author please connect me. 
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

.intel_syntax noprefix


.globl __sse_memcmp
.type __sse_memcmp, @function

#define LGPAGESZ 12
#define PAGESZ (1<<LGPAGESZ)

// Branchless strategy for <8-byte compares:
// We were asked to read rdx bytes from x (for values of x in {rdi rsi}), but we
// actually want to read 8 bytes starting from x and shift off the extraneous ones.
// Is that a problem?  If x is near the end of a page, and x+rdx is also near
// the end of that page, but x+7 is in the next page, then we read from the next
// page when we shouldn't.  But if x+rdx crosses a page, then we don't care.  So
// what we have to check is whether x+rdx and x+7 are in different pages, which
// is equivalent to checking if they differ in the LGPAGESZth bit.
// If they do so differ, then we can instead read 8 bytes from x+rdx-8, and shift
// the other way around.

// rdi, rsi: strings
// rdx: length
#pragma GCC target("sse2")


.p2align 4,0xcc
__sse_memcmp:
cmp	rdx, 8
jae	.Labove8
test	edx, edx
jz	.Lnought

lea	r8, [rdi + rdx]
lea	r9, [rdi + 7]
xor	r9, r8
test	r9, PAGESZ	// shr+jc would use a shorter immediate, but not fuse
jnz	.Lfixrdi
mov	rax, [rdi]
bswap	rax

// shift off the dummy bits
lea	ecx, [rdx*8]
neg	ecx		// ecx := 64 - 8*edx; but we can skip adding 64, since we use it as a shift

.Lpostrdi:
shr	rax, cl
lea	r8, [rsi + rdx]
lea	r9, [rsi + 7]
xor	r9, r8
test	r9, PAGESZ
jnz	.Lfixrsi

mov	rsi, [rsi]
bswap	rsi

.Lpostrsi:
shr	rsi, cl
xor	edx, edx
sub	rax, rsi
seta	dl
sar	rax, 63
or	eax, edx
ret

.Lnought:
xor	eax, eax
ret

.section .text.cold,"ax"	// good idea or no?
.Lfixrdi:
mov	rdi, [rdi + rdx - 8]
bswap	rdi
lea	ecx, [rdx*8]
neg	ecx
shl	rdi, cl
jmp	.Lpostrdi

.Lfixrsi:
mov	rsi, [rdi + rdx - 8]
bswap	rsi
shl	rsi, cl
jmp	.Lpostrsi

.section .text
.Labove8:
cmp	rdx, 16
ja	.Labove16
.Lbelow16:
mov	r8, [rdi + rdx - 8]
mov	r9, [rsi + rdx - 8]
mov	rax, [rdi]
mov	rsi, [rsi]
bswap	r8
bswap	r9
bswap	rax
bswap	rsi
xor	edx, edx
cmp	r8, r9
sbb	rax, rsi
seta	dl
sar	rax, 63
or	eax, edx
ret

.Labove16:
// straightaway check the first 16 bytes; if they differ (likely), set length to 16 and reuse plain <=16 code
movdqu	xmm0, [rdi]
movdqu	xmm1, [rsi]
mov	eax, 16
pcmpeqb	xmm0, xmm1
pmovmskb ecx, xmm0
cmp	ecx, (1<<16)-1
cmovne	rdx, rax	// must be 64-bit cmov, since on fail, we have to keep the high 32 bits of the length
jne	.Lbelow16

// no dice. Align s1, and hope s2 ends up aligned too
lea	rcx, [rdi + 16]
and	rcx, -16		// compute aligned s1
sub	rdi, rcx		// compute -delta
sub	rsi, rdi		// advance s2
lea	rdx, [rdx + rdi	- 16]	// reduce length by the amount that we advanced s1
				// also, bias it by -16, so when there are <=2 words left, an index will be >= it
test	rdx, rdx
jle	.Lshort			// not enough left for even one iteration of the loop?
				// (This tests rdx<=0, which could also be true if the original input was >2^63.
				//  But that's going to be UB anyway, since you can't have an allocation that large, so...)

xor	edi, edi		// initialise index
.Lloop:
movdqu	xmm0, [rsi + rdi]
pcmpeqb	xmm0, [rcx + rdi]
pmovmskb eax, xmm0
cmp	eax, (1<<16)-1
jne	.Lmismatch
add	rdi, 16
cmp	rdi, rdx
jb	.Lloop

.Lshort:
// last 16 bytes; again, set up registers for the <=16 code and jump to it
// length was previously biased by -16, so all we have to do is add it to get a pointer 16 bytes before the end
lea	rdi, [rcx + rdx]
add	rsi, rdx
mov	edx, 16
jmp	.Lbelow16

.Lmismatch:
tzcnt	eax, eax	// decodes as bsf on non-bmi; same behaviour for nonzero input (which we have ensured), but faster for some bmi cpus
add	rdi, rax
movzx	eax, byte ptr [rsi + rdi]
movzx	ecx, byte ptr [rcx + rdi]
sub	eax, ecx
ret
#pragma GCC pop_options
#endif