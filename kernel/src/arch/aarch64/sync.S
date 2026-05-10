/*
* SPDX-License-Identifier: GPL-2.0-only
* File: sync.S
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
.arch armv8-a+lse

//	Ends function definition.
//	@cost	saves 1-3 lines of code
.macro	.endfn	name:req bnd vis
 .size	"\name",.-"\name"
 .type	"\name",@function
 .ifnb	\bnd
  .\bnd	"\name"
 .endif
 .ifnb	\vis
  .\vis	"\name"
 .endif
.endm

//	Ends variable definition.
//	@cost	saves 1-3 lines of code
.macro	.endobj	name:req bnd vis
 .size	"\name",.-"\name"
 .type	"\name",@object
 .ifnb	\bnd
  .\bnd	"\name"
 .endif
 .ifnb	\vis
  .\vis	"\name"
 .endif
.endm

.macro .begfn name
	.section .text.\name,"ax",%progbits
	.balign	16
	.ftrace1
\name:
	.ftrace2
.endm

.macro	jnatom	label
	adrp	x16,__aarch64_have_lse_atomics
	ldrb	w16,[x16,:lo12:__aarch64_have_lse_atomics]
	cbz	w16,\label
.endm

.macro	.ftrace1
#ifdef FTRACE
#ifdef __x86_64__
	.rept	9
	nop
	.endr
#elif defined(__aarch64__)
	.rept	8
	nop
	.endr
#endif /* __x86_64__ */
#endif /* FTRACE */
.endm

//	Inserts --ftrace prologue.
//	This goes immediately after the function symbol.
//	@see	.ftrace1
.macro	.ftrace2
#ifdef FTRACE
#ifdef __x86_64__
	xchg	%ax,%ax
#elif defined(__aarch64__)
	nop
#endif /* __x86_64__ */
#endif /* FTRACE */
.endm

.begfn __aarch64_swp4_sync
	jnatom	1f
	swpa	w0,w0,[x1]
	ret
1:	mov	w16,w0
0:	ldxr	w0,[x1]
	stxr	w17,w16,[x1]
	cbnz	w17,0b
	dmb	ish
	ret
.endfn __aarch64_swp4_sync,globl