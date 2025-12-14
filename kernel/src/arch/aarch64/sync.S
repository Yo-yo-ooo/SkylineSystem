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