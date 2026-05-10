[bits 64]
[section .text]
[global mmu_invlpg]

mmu_invlpg:
    invlpg [rdi]
    ret