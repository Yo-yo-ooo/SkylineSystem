; SPDX-FileCopyrightText: 2026 Yo-yo-ooo
; SPDX-License-Identifier: GPL-2.0-only
; ============================================================================
; trampoline.nasm -- one reversible IA-32e -> real mode -> IA-32e round trip
; ----------------------------------------------------------------------------
; Flat binary, ORG 0x70000, copied verbatim to PHYSICAL/linear 0x70000 by the
; Tier1 builtin component.  vmm.cpp maps low 4 GiB identity (MM_READ|MM_WRITE,
; no NX), so VA 0x70000 == PA 0x70000 and this code is fetchable both with and
; without paging enabled.
;
; WHY 0x70000: 0x70000 is 64 KiB aligned, so its low 16 bits are ZERO.  In
; BITS 16 real mode CS=0x7000 (base 0x70000) and an `org 0x70000` label's low
; 16 bits are exactly the segment-relative offset.  One blob can therefore be
; addressed from 64-, 32- and 16-bit code without any relocation fixups.
;
; WHY NOT VM86 / WHY LOW MEMORY IS STILL NEEDED:
;   - EFLAGS.VM is ignored in IA-32e mode; VM86 does not exist in long mode.
;   - Leaving IA-32e requires clearing CR0.PG, and the instant PG=0 the
;     higher-half kernel (0xFFFF8...) is unmapped.  The instructions that
;     perform the switch MUST live in identity-mapped memory below 4 GiB.
;   - Real mode and VM86 both use 20-bit segment:offset addressing, so neither
;     can reach a high physical address.  Keeping the trampoline/GDT/shadow in
;     one 64 KiB window is the cheapest option; the guest<-host data transfer
;     is done by the C++ side via CopyToUser, not by the 16-bit code.
;
; MODE SEQUENCE (Intel SDM Vol.3A):
;   long(64) --retfq to L=0/D=1 segment--> compatibility(32, PG=1)
;   clear CR4.PCIDE (required: PCIDE=1 with PG=0 is #GP)
;   clear CR0.PG                        --> legacy protected(32, PG=0)
;                                          (EFER.LMA := LME & PG becomes 0;
;                                           EFER.LME is deliberately LEFT SET)
;   far jmp to D=0 16-bit segment      --> protected(16)
;   clear CR0.PE                       --> REAL MODE
;   ... run 16-bit DOS payload, write shadow screen ...
;   set CR0.PE                         --> protected(16)
;   far jmp to D=1 segment             --> protected(32)
;   CR3 := clean PML4; set CR0.PG      --> compatibility(32) (LME still 1)
;   retf to L=1 segment                --> long(64)
;   restore GDTR/IDTR/MSRs/GPRs/RFLAGS/RSP, ret to C++
;
; Keeping EFER.LME set across the whole trip removes two WRMSR steps (and two
; triple-fault opportunities) on the return path.  LME=1 with PG=0 is a legal
; "IA-32e disabled but enabled-for-later" state.
;
; HARD REQUIREMENTS enforced by the caller (Tier1 IPI handler):
;   * cpu is marked offline (Tier0 filter drops every IPI except vector 0xF0)
;   * IF=0, local LAPIC timer stopped, LVTs masked, TPR raised
;   * no NMI/SMI/#MC during the window (documented limitation)
;   * GS.base / KERNEL_GS_BASE are saved+restored here (IDT stub uses gs:[InIntr])
;
; STATE AREA LAYOUT (absolute addresses, must match dosvm.cpp constexprs):
;   0x7E000..0x7E07F  16 GPRs (8 B each)
;   0x7E080  rflags    0x7E088  cr0   0x7E090  cr3   0x7E098  cr4
;   0x7E0A0  fs.base   0x7E0A8  gs.base  0x7E0B0  kernel_gs_base
;   0x7E0B8  star      0x7E0C0  lstar    0x7E0C8  fmask    0x7E0D0  pat
;   0x7E0D8  GDTR(10)  0x7E0E8  IDTR(10)
;   0x7E0F8  cr3_clean (physical PML4, low 12 bits zero, prepared by C++)
;   0x7E100  scratch byte (putc16 parks the character being printed here)
;   0x7E120  magic (written on successful return)
;   0x7E128  flags (bit0 = reached real mode)
;   0x7E130  stage (1..11 progress marker)
;   0x7E138  runmode (0 = banner, 1 = .COM, 2 = .EXE)            [C++ writes]
;   0x7E13C  exit code, 0x7E140 wants-a-key byte, 0x7E142 exited byte
;   0x7E144  cursor column, 0x7E146 cursor row
;   0x7E148  queued key count, 0x7E14A next slot          [C++ writes before trip]
;   0x7E150  32 x u16 key slots (AH=scan, AL=ascii)       [C++ writes before trip]
;   0x7E190  resume flag (bit0), 0x7E192 pending-int id
;   0x7E196  suspended GPRs (ax,bx,cx,dx,si,di,bp,sp), 0x7E1A6 segment regs,
;   0x7E1AE saved iret frame (ip,cs,flags), 0x7E1B6 AX-on-resume
;   0x7E1B8  host file request block (func/handle/count/off/seg/result/cf/err)
;   0x7E1CA  .EXE entry CS:IP and SS:SP                          [C++ writes]
; Every byte above is also mirrored as a constexpr in dosvm.cpp.
; SHADOW SCREEN at 0x7F000: [0..15] header (filled by C++), [16..4015] cells.
; ============================================================================

%define LOW        0x70000
%define LOW_SEG    0x7000
%define STATE      0x7E000
%define SHADOW     0x7F000
%define STACK_TOP  0x74FF0
%define STACK_SP16 0x4FF0

; Temporary GDT selectors (filled into GDT by C++ at 0x72000).
%define SEL_C32    0x08
%define SEL_D32    0x10
%define SEL_C16    0x18
%define SEL_D16    0x20
%define SEL_C64    0x28

%define S_RFLAGS   0x7E080
%define S_CR0      0x7E088
%define S_CR3      0x7E090
%define S_CR4      0x7E098
%define S_FSBASE   0x7E0A0
%define S_GSBASE   0x7E0A8
%define S_KGSBASE  0x7E0B0
%define S_STAR     0x7E0B8
%define S_LSTAR    0x7E0C0
%define S_FMASK    0x7E0C8
%define S_PAT      0x7E0D0
%define S_GDTR     0x7E0D8
%define S_IDTR     0x7E0E8
%define S_CR3CLEAN 0x7E0F8
%define S_MAGIC    0x7E120
%define S_FLAGS    0x7E128
%define S_STAGE    0x7E130   ; progress marker, written at each mode transition

; --- DOS environment state (C++ writes some of these before the trip) -----
%define S_SCRATCH  0x7E100   ; 1 byte, putc16 parks the character here
%define S_RUNMODE  0x7E138   ; u32: 0 = banner, 1 = .COM, 2 = .EXE
%define S_EXITCODE 0x7E13C   ; u32: exit code (int 21h AH=4Ch / int 20h)
%define S_WAITKEY  0x7E140   ; u8 : program suspended waiting for a key
%define S_EXITED   0x7E142   ; u8 : program terminated itself (not a fault)
%define S_CURX     0x7E144   ; u16: text cursor column 0..79
%define S_CURY     0x7E146   ; u16: text cursor row    0..24
%define S_KBN      0x7E148   ; u16: queued key count
%define S_KBI      0x7E14A   ; u16: index of the next key slot
%define S_KBUF     0x7E150   ; 32 x u16, AH=scan code, AL=ascii

; --- suspend / resume -----------------------------------------------------
; A 16-bit program can only give the core back at an int 21h / int 16h entry
; point.  At that moment the whole architectural state is parked here and the
; caller (C++) is free to hand new keys or file data over, then re-enter: the
; trampoline rebuilds the iret frame and returns to the instruction right
; after the int, with the service's result already in AX.
%define S_RESUME   0x7E190   ; u16: bit0 = a suspended context is valid
%define S_PEND     0x7E192   ; u16: 16 = int 16h, 21 = int 21h, 0 = plain yield
%define S_CTX      0x7E196   ; ax,bx,cx,dx,si,di,bp,sp  (8 x u16)
%define S_SEGS     0x7E1A6   ; ds,es,ss,pad            (4 x u16)
%define S_FRAME    0x7E1AE   ; ip,cs,flags,pad         (4 x u16)
%define S_RETAX    0x7E1B6   ; u16: fallback value for AX on resume

; --- host file service (int 21h 3Ch/3Dh/3Eh/3Fh/40h) ----------------------
; The 16-bit side cannot call into 64-bit code.  It parks the request here,
; gives the core back, and the DOS client (programs/dos) performs the real
; file operation against SkylineSystem's VFS before re-entering.
%define S_FSACT    0x7E1B8   ; u16: 1 = request waiting for the host
%define S_FSFUNC   0x7E1BA   ; u16: AX as the program passed it (AH = function)
%define S_FSHAND   0x7E1BC   ; u16: BX (handle)
%define S_FSCNT    0x7E1BE   ; u16: CX (byte count)
%define S_FSOFF    0x7E1C0   ; u16: DX (offset inside the program's segment)
%define S_FSSEG    0x7E1C2   ; u16: DS at the time of the call
%define S_FSRES    0x7E1C4   ; u16: AX to return (host fills)
%define S_FSCF     0x7E1C6   ; u16: 1 = host wants CF set (error)
%define S_FSERR    0x7E1C8   ; u16: DOS error code for AX when FSCF = 1

; --- .EXE entry point, computed by C++ from the MZ header ------------------
%define S_EXECS    0x7E1CA   ; u16: absolute CS to start at
%define S_EXEIP    0x7E1CE   ; u16: IP
%define S_EXESS    0x7E1D2   ; u16: absolute SS
%define S_EXESP    0x7E1D6   ; u16: SP

%define IVT_BASE            0x73000
%define IVT_OFF16           0x3000
; C++ pre-fills: an IRET (0xCF) at segment offset 0xC00, and a 6-byte IDTR
; image (limit 0x3FF, base 0x73000) at segment offset 0xC04.  Both live in
; the zero-padded area past the blob.  The blob grew past 0x800 once the
; real-mode DOS services moved in, so the stub moved out to 0xC00; the C++
; side asserts blob_size < DOSVM_COM_OFF (0x1000) which is the real limit.
%define DOSVM_IVT_STUB_OFF  0x0C00
%define DOSVM_IDTR_OFF16    0x0C04

%define MSR_FSBASE   0xC0000100
%define MSR_GSBASE   0xC0000101
%define MSR_KGSBASE  0xC0000102
%define MSR_STAR     0xC0000081
%define MSR_LSTAR    0xC0000082
%define MSR_FMASK    0xC0000084
%define MSR_PAT      0x00000277

%define DOSVM_MAGIC  0x444F53564D544150   ; "DOSVMTAP"

; ---------------------------------------------------------------------------
; 64-bit entry.  Called by C++ as ((void(*)(void))0x70000)().
; ---------------------------------------------------------------------------
BITS 64
ORG LOW

entry64:
    cli
    mov byte [abs S_STAGE], 1
    ; ---- save every GPR (absolute addressing; no scratch register needed) ----
    mov [abs STATE + 0x00], rax
    mov [abs STATE + 0x08], rbx
    mov [abs STATE + 0x10], rcx
    mov [abs STATE + 0x18], rdx
    mov [abs STATE + 0x20], rsi
    mov [abs STATE + 0x28], rdi
    mov [abs STATE + 0x30], rbp
    mov [abs STATE + 0x38], r8
    mov [abs STATE + 0x40], r9
    mov [abs STATE + 0x48], r10
    mov [abs STATE + 0x50], r11
    mov [abs STATE + 0x58], r12
    mov [abs STATE + 0x60], r13
    mov [abs STATE + 0x68], r14
    mov [abs STATE + 0x70], r15
    mov [abs STATE + 0x78], rsp
    pushfq
    pop rax
    mov [abs S_RFLAGS], rax

    ; ---- control registers ----
    mov rax, cr0
    mov [abs S_CR0], rax
    mov rax, cr3
    mov [abs S_CR3], rax
    mov rax, cr4
    mov [abs S_CR4], rax

    ; ---- MSRs (rdmsr clobbers rax/rdx/rcx; all three already saved) ----
%macro SAVE_MSR 2
    mov ecx, %1
    rdmsr
    shl rdx, 32
    or  rax, rdx
    mov [abs %2], rax
%endmacro
    SAVE_MSR MSR_FSBASE,  S_FSBASE
    SAVE_MSR MSR_GSBASE,  S_GSBASE
    SAVE_MSR MSR_KGSBASE, S_KGSBASE
    SAVE_MSR MSR_STAR,    S_STAR
    SAVE_MSR MSR_LSTAR,   S_LSTAR
    SAVE_MSR MSR_FMASK,   S_FMASK
    SAVE_MSR MSR_PAT,     S_PAT

    ; ---- descriptor tables (10-byte form in 64-bit mode) ----
    sgdt [abs S_GDTR]
    sidt [abs S_IDTR]

    ; ---- switch to the identity-mapped low stack ----
    mov rsp, STACK_TOP

    ; ---- load the temporary GDT (contains 64/32/16-bit segments) ----
    lgdt [abs gdt_desc]

    ; ---- far return into 32-bit compatibility mode ----
    ; retfq pops RIP(8) then CS(8); push CS first so RIP is on top.
    mov byte [abs S_STAGE], 2
    push qword SEL_C32
    push qword c32_entry
    db 0x48, 0xCB                      ; REX.W + RETF (retfq)

; ---------------------------------------------------------------------------
; 32-bit compatibility mode (IA-32e, PG=1, CS is L=0/D=1)
; ---------------------------------------------------------------------------
BITS 32
align 16
c32_entry:
    mov byte [S_STAGE], 3
    mov ax, SEL_D32
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov esp, STACK_TOP

    ; PCIDE=1 requires PG=1; clear it BEFORE clearing PG or we take #GP.
    mov eax, cr4
    and eax, 0xFFFDFFFF                 ; ~(1 << 17)
    mov cr4, eax

    ; Clear PG.  EFER.LMA becomes 0 automatically (LMA = LME & PG).
    ; EFER.LME stays set so re-enabling PG returns us to IA-32e directly.
    mov eax, cr0
    and eax, 0x7FFFFFFF                 ; ~(1 << 31)
    mov cr0, eax
    jmp short .flush
.flush:
    mov byte [S_STAGE], 4
    ; Now legacy 32-bit protected mode, no paging.  Far jump to a D=0
    ; 16-bit code segment.  The 0x66 prefix makes the far pointer 16:16.
    db 0x66, 0xEA
    dw (c16_entry - LOW)
    dw SEL_C16

; ---------------------------------------------------------------------------
; 16-bit protected mode (CS base = 0x70000, limit 0xFFFF)
; ---------------------------------------------------------------------------
BITS 16
align 16
c16_entry:
    mov byte [S_STAGE - LOW], 5
    mov ax, SEL_D16
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, STACK_SP16

    ; Clear PE -> real mode.
    mov eax, cr0
    and al, 0xFE
    mov cr0, eax

    ; Reload CS with a real-mode segment value (0x7000 -> base 0x70000).
    db 0xEA
    dw (real_entry - LOW)
    dw LOW_SEG

; ---------------------------------------------------------------------------
; REAL MODE.  This is the actual "core has left x86_64" window.
; ---------------------------------------------------------------------------
align 16
real_entry:
    mov byte [S_STAGE - LOW], 6
    mov ax, LOW_SEG
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, STACK_SP16
    cld

    ; Install a real-mode IVT before anything else.  IDTR still points at the
    ; 64-bit IDT in the higher half; with PG=0 that memory is unreachable, so
    ; a stray NMI/#MC would decode 4-byte real-mode vectors as 16-byte 64-bit
    ; gates and triple-fault.  The C++ side pre-fills all 256 vectors at
    ; 0x73000 to point at a bare IRET at segment offset DOSVM_IVT_STUB_OFF,
    ; and places the 6-byte IDTR image at DOSVM_IDTR_OFF16.  Using fixed
    ; numeric offsets (no labels) keeps BITS 16 relocation-free.
    lidt [DOSVM_IDTR_OFF16]

    ; Record positive evidence that we truly reached real mode.
    mov byte [S_FLAGS - LOW], 1

    ; Run the real-mode DOS environment: either the built-in banner demo or
    ; an external .COM staged at 0x71000, selected by S_RUNMODE.  Nothing
    ; returns through the call stack: a real program owns SS:SP, so every
    ; exit path jumps to real_mode_done instead of relying on a return address
    ; that the program may have trampled.
    jmp dos_entry

real_mode_done:
    mov byte [S_STAGE - LOW], 7

    ; ---- return path: real -> protected(16) ----
    mov eax, cr0
    or  al, 1                          ; PE=1
    mov cr0, eax
    db 0xEA
    dw (p16_return - LOW)
    dw SEL_C16

align 16
p16_return:
    mov byte [S_STAGE - LOW], 8
    mov ax, SEL_D16
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, STACK_SP16
    ; 16-bit code -> 32-bit code: 0x66 far jmp with a 32-bit absolute offset.
    db 0x66, 0xEA
    dd p32_return
    dw SEL_C32

; ---------------------------------------------------------------------------
; Real-mode DOS environment.
;
; IF is clear for the whole trip, so nothing here may ever wait on an event --
; every service is synchronous and reachable through the IVT built below.
;
;   int 10h  video     AH=00 set text mode, 02 set cursor, 03 get cursor,
;                      06 scroll up, 0Eh TTY output
;   int 16h  keyboard  AH=00 read key, 01 peek (ZF).  The queue is filled by
;   *                  C++ before the trip; there is no live keyboard path.
;   int 20h  terminate
;   int 21h  DOS       AH=02 putc, 09 print $-string, 01/06/07/08 read key,
;                      0Bh key status, 0Eh/19h/25h/35h/4Ah accepted as no-ops,
;                      30h version, 4Ch terminate; the file/FCB range fails
;                      with CF=1,AX=1 (no callback path into 64-bit mode)
;
; A .COM that blocks on a key would hang the core past the caller's 2 s
; timeout, so int 16h AH=00 never blocks: on an empty queue it records
; S_WAITKEY and terminates, and the caller replays it with fresh keys.
; ---------------------------------------------------------------------------
%define H_IP  bp+2
%define H_CS  bp+4
%define H_FL  bp+6

%macro H_PROLOG 0
    push bp
    mov  bp, sp
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push ds
    push es
%endmacro

; Write the live registers back into the slots the prolog pushed, then hand
; them to the interrupted program.  Bouncing through the slots (instead of
; popping AX) is what lets a handler return values in AX/BX/CX/DX.
%macro H_EPILOG 0
    mov [bp-2],  ax
    mov [bp-4],  bx
    mov [bp-6],  cx
    mov [bp-8],  dx
    mov [bp-10], si
    mov [bp-12], di
    mov [bp-14], ds
    mov [bp-16], es
    pop es
    pop ds
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    pop bp
    iret
%endmacro

align 16
dos_entry:
    mov ax, LOW_SEG
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, STACK_SP16
    cld

    call install_ivt16             ; idempotent, and must precede any int

    cmp byte [cs:S_RUNMODE - LOW], 0
    je  .banner

    ; A suspended program resumes before anything is re-initialised: the
    ; whole architectural state was parked in the state page last time.
    ; The screen is deliberately NOT cleared -- it still shows the program.
    cmp word [cs:S_RESUME - LOW], 0
    jne com_resume

    call clear_screen16

    ; ---- fresh start: clear per-run bookkeeping ----
    mov word [cs:S_EXITCODE - LOW], 0
    mov byte [cs:S_EXITED   - LOW], 0
    mov byte [cs:S_WAITKEY  - LOW], 0
    mov word [cs:S_CURX     - LOW], 0
    mov word [cs:S_CURY     - LOW], 0
    mov word [cs:S_FSACT    - LOW], 0
    mov word [cs:S_RESUME   - LOW], 0

    cmp byte [cs:S_RUNMODE - LOW], 2
    je  .run_exe

    ; ---- .COM: PSP:0x0100, stack in the reserved low segment ----
    mov ax, 0x7100
    mov ds, ax
    mov es, ax
    mov ax, LOW_SEG
    mov ss, ax
    mov sp, STACK_SP16 - 4

    push word LOW_SEG              ; a plain `ret` in the .COM lands here
    push word (com_exit_stub - LOW)
    push word 0x7100
    push word 0x0100
    retf                           ; -> 7100:0100

    ; ---- .EXE: CS:IP and SS:SP come from the MZ header (C++ resolved) ----
.run_exe:
    mov ax, 0x7100
    mov ds, ax
    mov es, ax
    mov ax, [cs:S_EXESS - LOW]
    mov ss, ax
    mov sp, [cs:S_EXESP - LOW]
    sub sp, 4                      ; leave room for the far `ret` target

    push word LOW_SEG
    push word (com_exit_stub - LOW)
    push word [cs:S_EXECS - LOW]
    push word [cs:S_EXEIP - LOW]
    retf

.banner:
    ; ---- built-in demo: the historical four lines, unchanged ----
    call clear_screen16
    mov di, (SHADOW - LOW) + 16
    mov si, msg_banner
    mov ah, 0x1F
    call putstr16
    mov di, (SHADOW - LOW) + 16 + (2 * 160)
    mov si, msg_real
    mov ah, 0x0A
    call putstr16
    mov di, (SHADOW - LOW) + 16 + (4 * 160)
    mov si, msg_note
    mov ah, 0x07
    call putstr16
    mov di, (SHADOW - LOW) + 16 + (6 * 160)
    mov si, msg_hint
    mov ah, 0x0E
    call putstr16
    mov word [cs:S_CURY - LOW], 7
    mov word [cs:S_CURX - LOW], 0
    jmp real_mode_done

; A .COM gets here via `ret`, int 20h or int 21h/4Ch.  Reset the stack so a
; program that trashed SP cannot derail the return path.
com_exit_stub:
    mov ax, LOW_SEG
    mov ds, ax
    mov es, ax
    jmp real_mode_done             ; SS:SP may belong to the program by now

; ---------------------------------------------------------------------------
; save_ctx: park the interrupted machine state.
; Requires BP to point at the int21/int16 prolog frame, i.e.
;   [bp+2] = ip, [bp+4] = cs, [bp+6] = flags,
;   [bp-2..bp-16] = ax,bx,cx,dx,si,di,ds,es
; ---------------------------------------------------------------------------
save_ctx:
    mov ax, [bp - 2]
    mov [cs:S_CTX - LOW + 0], ax       ; ax
    mov ax, [bp - 4]
    mov [cs:S_CTX - LOW + 2], ax       ; bx
    mov ax, [bp - 6]
    mov [cs:S_CTX - LOW + 4], ax       ; cx
    mov ax, [bp - 8]
    mov [cs:S_CTX - LOW + 6], ax       ; dx
    mov ax, [bp - 10]
    mov [cs:S_CTX - LOW + 8], ax       ; si
    mov ax, [bp - 12]
    mov [cs:S_CTX - LOW + 10], ax      ; di
    mov ax, [bp]
    mov [cs:S_CTX - LOW + 12], ax      ; bp
    lea ax, [bp + 2]
    mov [cs:S_CTX - LOW + 14], ax      ; sp points at the iret frame

    mov ax, [bp - 14]
    mov [cs:S_SEGS - LOW + 0], ax      ; ds
    mov ax, [bp - 16]
    mov [cs:S_SEGS - LOW + 2], ax      ; es
    mov ax, ss
    mov [cs:S_SEGS - LOW + 4], ax      ; ss

    mov ax, [bp + 2]
    mov [cs:S_FRAME - LOW + 0], ax     ; ip
    mov ax, [bp + 4]
    mov [cs:S_FRAME - LOW + 2], ax     ; cs
    mov ax, [bp + 6]
    mov [cs:S_FRAME - LOW + 4], ax     ; flags

    mov word [cs:S_RESUME - LOW], 1
    ret

; ---------------------------------------------------------------------------
; com_resume: rebuild the iret frame on the program's own stack, restore every
; register, and return to the instruction right after the int that suspended
; it.  The frame is rebuilt from the saved copy rather than trusting whatever
; is still sitting on the stack.
; ---------------------------------------------------------------------------
com_resume:
    mov ax, [cs:S_SEGS - LOW + 4]
    mov ss, ax
    mov ax, [cs:S_CTX - LOW + 14]
    mov sp, ax

    mov ax, [cs:S_FRAME - LOW + 4]
    push ax                            ; flags
    mov ax, [cs:S_FRAME - LOW + 2]
    push ax                            ; cs
    mov ax, [cs:S_FRAME - LOW + 0]
    push ax                            ; ip

    mov ax, [cs:S_SEGS - LOW + 0]
    mov ds, ax
    mov ax, [cs:S_SEGS - LOW + 2]
    mov es, ax

    mov ax, [cs:S_CTX - LOW + 12]
    mov bp, ax
    mov ax, [cs:S_CTX - LOW + 10]
    mov di, ax
    mov ax, [cs:S_CTX - LOW + 8]
    mov si, ax
    mov ax, [cs:S_CTX - LOW + 6]
    mov dx, ax
    mov ax, [cs:S_CTX - LOW + 4]
    mov cx, ax
    mov ax, [cs:S_CTX - LOW + 2]
    mov bx, ax

    ; AX and CF: a completed file request takes priority over the key path.
    cmp word [cs:S_FSACT - LOW], 2
    jne .no_fs_result
    mov ax, [cs:S_FSRES - LOW]
    mov word [cs:S_FSACT - LOW], 0
    cmp word [cs:S_FSCF - LOW], 0
    je  .go
    mov bx, sp
    or  word [ss:bx + 4], 1            ; set CF in the rebuilt frame
    jmp .go

.no_fs_result:
    mov ax, [cs:S_RETAX - LOW]
    cmp word [cs:S_PEND - LOW], 0
    je  .go
    cmp word [cs:S_KBN - LOW], 0
    je  .go
    call kbd_pop16
.go:
    mov word [cs:S_RESUME - LOW], 0    ; one-shot
    mov word [cs:S_PEND   - LOW], 0
    mov byte [cs:S_WAITKEY - LOW], 0
    iret

; putstr16: ES:DI = dest cell, DS:SI = NUL-terminated string, AH = attribute.
putstr16:
    mov al, [si]
    or  al, al
    jz  .done
    mov [es:di], al
    mov [es:di+1], ah
    inc di
    inc di
    inc si
    jmp putstr16
.done:
    ret

; Clear 2000 cells to ' ' on light grey, cursor to home.
clear_screen16:
    push ax
    push cx
    push di
    push es
    mov ax, LOW_SEG
    mov es, ax
    mov di, (SHADOW - LOW) + 16
    mov cx, 2000
    mov ax, 0x0720
    cld
    rep stosw
    mov word [cs:S_CURX - LOW], 0
    mov word [cs:S_CURY - LOW], 0
    pop es
    pop di
    pop cx
    pop ax
    ret

; Scroll the 80x25 shadow up one line and blank the bottom row.
scroll_up16:
    push ax
    push cx
    push si
    push di
    push ds
    push es
    mov ax, LOW_SEG
    mov ds, ax
    mov es, ax
    mov si, (SHADOW - LOW) + 16 + 160
    mov di, (SHADOW - LOW) + 16
    mov cx, 1920                   ; 24 rows x 160 bytes, as words
    cld
    rep movsw
    mov di, (SHADOW - LOW) + 16 + 3840
    mov cx, 80
    mov ax, 0x0720
    rep stosw
    pop es
    pop ds
    pop di
    pop si
    pop cx
    pop ax
    ret

; Advance to column 0 of the next row, scrolling if we fall off the bottom.
crlf16:
    push ax
    mov word [cs:S_CURX - LOW], 0
    inc word [cs:S_CURY - LOW]
    mov ax, [cs:S_CURY - LOW]
    cmp ax, 25
    jb  .ok
    call scroll_up16
    mov word [cs:S_CURY - LOW], 24
.ok:
    pop ax
    ret

; putc16: AL = character.  Touches only its own saved registers, so callers
; keep DS/ES across the call -- that is what lets int 21h AH=09h keep reading
; the program's string while printing it.
putc16:
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push ds
    push es
    mov bx, LOW_SEG
    mov ds, bx
    mov es, bx
    mov [cs:S_SCRATCH - LOW], al
    cmp al, 13
    je  .cr
    cmp al, 10
    je  .lf
    cmp al, 8
    je  .bs

    mov cx, [cs:S_CURY - LOW]
    cmp cx, 25
    jb  .row_ok
    call scroll_up16
    mov cx, 24
    mov word [cs:S_CURY - LOW], 24
.row_ok:
    mov dx, [cs:S_CURX - LOW]
    cmp dx, 80
    jb  .col_ok
    call crlf16
    mov cx, [cs:S_CURY - LOW]
    mov dx, 0
    mov word [cs:S_CURX - LOW], 0
.col_ok:
    mov si, dx                     ; keep the column: mul eats DX
    mov ax, cx
    mov di, 160
    mul di                         ; AX = row * 160
    mov di, ax
    shl si, 1
    add di, si
    add di, (SHADOW - LOW) + 16
    mov al, [cs:S_SCRATCH - LOW]
    mov [es:di], al
    mov byte [es:di+1], 0x07
    inc word [cs:S_CURX - LOW]
    jmp .out
.cr:
    mov word [cs:S_CURX - LOW], 0
    jmp .out
.lf:
    call crlf16
    jmp .out
.bs:
    mov ax, [cs:S_CURX - LOW]
    or  ax, ax
    jz  .out
    dec word [cs:S_CURX - LOW]
.out:
    pop es
    pop ds
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; Build the real-mode IVT.  The C++ side only plants the IDTR image and the
; bare-IRET byte; every vector that matters is wired here, where label offsets
; are already segment-relative (= absolute address minus 0x70000, because the
; blob is org 0x70000 on a 64 KiB boundary).
install_ivt16:
    push ax
    push cx
    push di
    push es
    mov ax, 0x7300                 ; IVT phys 0x73000
    mov es, ax
    xor di, di
    mov cx, 256
.fill:
    mov word [es:di],     (ivt_iret16 - LOW)
    mov word [es:di + 2], LOW_SEG
    add di, 4
    loop .fill

    mov di, 0x20 * 4               ; int 20h terminate
    mov word [es:di],     (int20_handler - LOW)
    mov word [es:di + 2], LOW_SEG
    mov di, 0x21 * 4               ; int 21h DOS services
    mov word [es:di],     (int21_handler - LOW)
    mov word [es:di + 2], LOW_SEG
    mov di, 0x10 * 4               ; int 10h video
    mov word [es:di],     (int10_handler - LOW)
    mov word [es:di + 2], LOW_SEG
    mov di, 0x16 * 4               ; int 16h keyboard
    mov word [es:di],     (int16_handler - LOW)
    mov word [es:di + 2], LOW_SEG

    pop es
    pop di
    pop cx
    pop ax
    ret
ivt_iret16: db 0xCF

; ---- key queue ------------------------------------------------------------
; kbd_pop16 -> AX = key, or 0 when empty.  Queue state lives in the low state
; page, so this works no matter what DS the program is using.
kbd_pop16:
    push bx
    push si
    mov bx, [cs:S_KBN - LOW]
    test bx, bx
    jz  .empty
    mov si, [cs:S_KBI - LOW]
    shl si, 1
    mov ax, [cs:S_KBUF - LOW + si]
    inc word [cs:S_KBI - LOW]
    dec word [cs:S_KBN - LOW]
    pop si
    pop bx
    ret
.empty:
    xor ax, ax
    pop si
    pop bx
    ret

; ---- int 20h: terminate ---------------------------------------------------
int20_handler:
    mov byte [cs:S_EXITED - LOW], 1
    jmp com_exit_stub

; ---- int 10h: video -------------------------------------------------------
int10_handler:
    H_PROLOG
    cmp ah, 0x0E
    je  .tty
    cmp ah, 0x03
    je  .getcur
    cmp ah, 0x02
    je  .setcur
    cmp ah, 0x00
    je  .setmode
    cmp ah, 0x06
    je  .scroll
    jmp .out
.tty:
    call putc16
    jmp .out
.setmode:
    cmp al, 3
    ja  .out
    call clear_screen16
    jmp .out
.setcur:
    mov al, dh
    xor ah, ah
    mov [cs:S_CURY - LOW], ax
    mov al, dl
    xor ah, ah
    mov [cs:S_CURX - LOW], ax
    jmp .out
.getcur:
    mov ax, [cs:S_CURY - LOW]
    mov dh, al
    mov ax, [cs:S_CURX - LOW]
    mov dl, al
    mov cx, 0x0607
    jmp .out
.scroll:
    call scroll_up16
    jmp .out
.out:
    H_EPILOG

; ---- int 16h: keyboard ----------------------------------------------------
int16_handler:
    H_PROLOG
    cmp ah, 0x00
    je  .get
    cmp ah, 0x01
    je  .peek
    jmp .out
.get:
    mov ax, [cs:S_KBN - LOW]
    or  ax, ax
    jz  .novice
    call kbd_pop16
    jmp .out
.novice:
    ; Never block, never abort: park the machine so the caller can hand us a
    ; key and re-enter.  `iret` on resume lands right after this int.
    mov word [cs:S_PEND - LOW], 16
    mov byte [cs:S_WAITKEY - LOW], 1
    call save_ctx
    jmp com_exit_stub
.peek:
    mov ax, [cs:S_KBN - LOW]
    or  ax, ax
    jz  .peeknone
    mov si, [cs:S_KBI - LOW]
    shl si, 1
    mov ax, [cs:S_KBUF - LOW + si]
    and word [H_FL], 0xFFBF        ; ZF = 0, a key is waiting
    jmp .out
.peeknone:
    xor ax, ax
    or  word [H_FL], 0x0040        ; ZF = 1, queue empty
.out:
    H_EPILOG

; ---- int 21h: DOS services ------------------------------------------------
int21_handler:
    H_PROLOG
    cmp ah, 0x4C
    je  .exit
    cmp ah, 0x09
    je  .puts
    cmp ah, 0x02
    je  .putc
    cmp ah, 0x30
    je  .version
    cmp ah, 0x01
    je  .waitkey
    cmp ah, 0x06
    je  .waitkey
    cmp ah, 0x07
    je  .waitkey
    cmp ah, 0x08
    je  .waitkey
    cmp ah, 0x0B
    je  .kstat
    cmp ah, 0x19
    je  .curdrv
    cmp ah, 0x0E                   ; select drive: nothing to select
    je  .ok
    cmp ah, 0x25                   ; set vector: IVT is ours, accepted
    je  .ok
    cmp ah, 0x35                   ; get vector
    je  .getvec
    cmp ah, 0x4A                   ; resize block: whole segment, fine
    je  .ok
    ; ---- host file services: park the request for the 64-bit side ----
    cmp ah, 0x3C                   ; create
    je  .fs
    cmp ah, 0x3D                   ; open
    je  .fs
    cmp ah, 0x3E                   ; close
    je  .fs
    cmp ah, 0x3F                   ; read
    je  .fs
    cmp ah, 0x40                   ; write
    je  .fs
    cmp ah, 0x41                   ; delete (host decides)
    je  .fs
    cmp ah, 0x42                   ; lseek
    je  .fs
    jmp .fail

.exit:
    mov [cs:S_EXITCODE - LOW], al
    mov byte [cs:S_EXITED - LOW], 1
    mov sp, bp                     ; discard the saved registers...
    mov bx, sp                     ; ...rebuild the iret frame (SP is not a
    mov word [ss:bx+2], (com_exit_stub - LOW)   ; legal 16-bit base register)
    mov word [ss:bx+4], LOW_SEG
    add sp, 2                      ; iret straight into com_exit_stub
    iret

.puts:
    mov si, dx                     ; DS:SI is the program's string
.loop_puts:
    lodsb
    cmp al, '$'
    je  .ok
    call putc16
    jmp .loop_puts

.putc:
    mov al, dl
    call putc16
    jmp .ok

.version:
    mov ax, 0x0330                 ; "DOS 3.30"
    mov bx, 0
    mov cx, 0
    jmp .ok

.waitkey:
    mov ax, [cs:S_KBN - LOW]
    or  ax, ax
    jz  .novice
    call kbd_pop16
    jmp .okax
.novice:
    mov word [cs:S_PEND - LOW], 21
    mov byte [cs:S_WAITKEY - LOW], 1
    call save_ctx
    jmp com_exit_stub

; Park a file request.  programs/dos reads this block out of the state page,
; performs the operation against the VFS, writes AX/CF back as S_FSRES/S_FSCF
; and sets S_FSACT = 2, then re-enters; com_resume iret's to the caller with
; those values already in AX/CF.
.fs:
    mov ax, [bp - 2]
    mov [cs:S_FSFUNC - LOW], ax    ; whole AX: AH is the function
    mov [cs:S_FSHAND - LOW], bx
    mov [cs:S_FSCNT  - LOW], cx
    mov [cs:S_FSOFF  - LOW], dx
    mov ax, [bp - 14]              ; caller's DS
    mov [cs:S_FSSEG  - LOW], ax
    mov word [cs:S_FSRES - LOW], 0
    mov word [cs:S_FSCF  - LOW], 0
    mov word [cs:S_FSACT - LOW], 1
    mov word [cs:S_PEND  - LOW], 21
    call save_ctx
    jmp com_exit_stub

.kstat:
    mov ax, [cs:S_KBN - LOW]
    or  ax, ax
    jz  .okax
    mov al, 0xFF
    jmp .okax

.curdrv:
    mov ax, 2                      ; C:
    jmp .ok

.getvec:
    mov ax, 0                       ; no vector of ours to read back
    jmp .ok

.ok:                               ; success, CF=0, AX=0
    xor ax, ax
.okax:                             ; success, CF=0, AX untouched
    and word [H_FL], 0xFFFE
    jmp .out

.fail:                             ; CF=1, AX=1 ("invalid function")
    or  word [H_FL], 0x0001
    mov ax, 1
.out:
    H_EPILOG

msg_banner: db "  SkylineSystem DOS VM  -  16-bit REAL MODE  ", 0
msg_real:   db "  This text was written by a real-mode program on a CPU that left IA-32e.", 0
msg_note:   db "  CR0.PG=0, CS=7000:xxxx, EFER.LMA=0. The core returns to long mode next.", 0
msg_hint:   db "  Press ENTER in this window to trigger another long->real->long trip.", 0

; ---------------------------------------------------------------------------
; 32-bit protected mode on the way back (PG=0, PE=1, LME=1)
; ---------------------------------------------------------------------------
BITS 32
align 16
p32_return:
    mov byte [S_STAGE], 9
    mov ax, SEL_D32
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov esp, STACK_TOP

    ; Reload a clean CR3 (low 12 bits zero).  This also flushes the TLB,
    ; addressing Tier0's "stale TLB after offline" concern.  PCIDE is still
    ; cleared here, so CR3[11:0] must be zero.
    mov eax, [dword S_CR3CLEAN]
    mov cr3, eax

    ; Re-enable paging.  Because EFER.LME is still 1 and the current CS is a
    ; 32-bit (L=0) segment, the CPU enters IA-32e COMPATIBILITY mode.
    mov eax, cr0
    or  eax, 0x80000000
    mov cr0, eax
    jmp short .flush
.flush:
    ; Far JUMP to an L=1 code segment -> 64-bit long mode.  This is the same
    ; sequence Linux uses (ljmpl $__KERNEL_CS, $startup_64): a ptr16:32 far
    ; jump from compatibility mode loading an L=1 CS.  Using a far jump rather
    ; than RETF avoids the ambiguity of far-return operand size when leaving
    ; compatibility mode (Intel SDM 9.8.5).
    db 0xEA
    dd ret64
    dw SEL_C64

; ---------------------------------------------------------------------------
; Back in 64-bit long mode (PG=1 again, higher-half is addressable again).
; ---------------------------------------------------------------------------
BITS 64
align 16
ret64:
    mov byte [abs S_STAGE], 10
    ; Restore CR4 first (re-enables PCIDE), then the original CR3 (which may
    ; carry a PCID and the no-flush bit 63).
    mov rax, [abs S_CR4]
    mov cr4, rax
    mov rax, [abs S_CR3]
    mov cr3, rax

    ; Restore the original per-CPU descriptor tables.
    lgdt [abs S_GDTR]
    lidt [abs S_IDTR]

    ; Restore FS/GS/KERNEL_GS bases.  IDT stubs do `inc/dec byte gs:[InIntr]`
    ; and `this_cpu()` reads %gs:0, so GS.base MUST be the original cpu_t*.
%macro LOAD_MSR 2
    mov ecx, %1
    mov rax, [abs %2]
    mov rdx, rax
    shr rdx, 32
    wrmsr
%endmacro
    LOAD_MSR MSR_FSBASE,  S_FSBASE
    LOAD_MSR MSR_GSBASE,  S_GSBASE
    LOAD_MSR MSR_KGSBASE, S_KGSBASE
    LOAD_MSR MSR_STAR,    S_STAR
    LOAD_MSR MSR_LSTAR,   S_LSTAR
    LOAD_MSR MSR_FMASK,   S_FMASK
    LOAD_MSR MSR_PAT,     S_PAT

    ; Kernel data segments.
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax

    ; Reload CS from the ORIGINAL kernel GDT (selector 0x08, L=1).  Without
    ; this the CS selector would still be 0x28, which is the TSS entry in the
    ; kernel GDT.
    push qword 0x08
    push qword cs_reloaded
    db 0x48, 0xCB
cs_reloaded:
    mov byte [abs S_STAGE], 11
    ; Success marker, read by the C++ side to prove the trip really returned.
    mov rax, DOSVM_MAGIC
    mov [abs S_MAGIC], rax

    ; Restore RFLAGS (IF was 0 on entry: the 0xF0 interrupt gate cleared it).
    push qword [abs S_RFLAGS]
    popfq

    ; Restore GPRs, then the original kernel stack, then return to C++.
    mov rbx, [abs STATE + 0x08]
    mov rcx, [abs STATE + 0x10]
    mov rdx, [abs STATE + 0x18]
    mov rsi, [abs STATE + 0x20]
    mov rdi, [abs STATE + 0x28]
    mov rbp, [abs STATE + 0x30]
    mov r8,  [abs STATE + 0x38]
    mov r9,  [abs STATE + 0x40]
    mov r10, [abs STATE + 0x48]
    mov r11, [abs STATE + 0x50]
    mov r12, [abs STATE + 0x58]
    mov r13, [abs STATE + 0x60]
    mov r14, [abs STATE + 0x68]
    mov r15, [abs STATE + 0x70]
    mov rax, [abs STATE + 0x00]
    mov rsp, [abs STATE + 0x78]
    ret

; ---------------------------------------------------------------------------
; Temporary GDT (also used verbatim by C++ to know the descriptor values).
; C++ writes these 6 entries at 0x72000 before the first trip.
;   CODE32/DATA32 : base 0, limit 4 GiB, D=1, G=1
;   CODE16/DATA16 : base 0x70000, limit 0xFFFF, D=0, G=0
;   CODE64        : base 0, limit 4 GiB, L=1, G=1
; ---------------------------------------------------------------------------
align 16
gdt_table:
    dq 0x0000000000000000
    dq 0x00CF9A000000FFFF               ; 0x08 CODE32
    dq 0x00CF92000000FFFF               ; 0x10 DATA32
    dq 0x00009A070000FFFF               ; 0x18 CODE16 (base 0x70000)
    dq 0x000092070000FFFF               ; 0x20 DATA16 (base 0x70000)
    dq 0x00AF9A000000FFFF               ; 0x28 CODE64
gdt_desc:
    dw (gdt_desc - gdt_table - 1)       ; limit = 47
    dq gdt_table                        ; 64-bit linear base
