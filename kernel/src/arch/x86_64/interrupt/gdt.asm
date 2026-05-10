; SPDX-License-Identifier: GPL-2.0-only
; File: gdt.asm
; Copyright (C) 2026 Yo-yo-ooo
;
; This file is part of SkylineSystem.
;
; SkylineSystem is free software; you can redistribute it and/or modify
; it under the terms of the GNU General Public License as published by
; the Free Software Foundation; either version 2 of the License, or
; (at your option) any later version.
;
; This program is distributed in the hope that it will be useful,
; but WITHOUT ANY WARRANTY; without even the implied warranty of
; MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
; GNU General Public License for more details.
;
; You should have received a copy of the GNU General Public License
; along with this program; if not, write to the Free Software
; Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.

[global gdt_reload_seg]
gdt_reload_seg:
    push 0x08
    lea rax, [rel .gdt_reload_cs]
    push rax
    retfq
.gdt_reload_cs:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    ret
