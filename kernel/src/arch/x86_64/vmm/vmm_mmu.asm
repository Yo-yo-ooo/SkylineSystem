; SPDX-FileCopyrightText: 2026 Yo-yo-ooo
; SPDX-License-Identifier: GPL-2.0-only

[bits 64]
[section .text]
[global mmu_invlpg]

mmu_invlpg:
    invlpg [rdi]
    ret