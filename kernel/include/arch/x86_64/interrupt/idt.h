#pragma once

#ifndef _X86_64_IDT_H
#define _X86_64_IDT_H

#include <stdint.h>
#include <stddef.h>
#include <pdef.h>

typedef struct context_t{
    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t r11;
    uint64_t r10;
    uint64_t r9;
    uint64_t r8;
    uint64_t rdi;
    uint64_t rsi;
    uint64_t rbp;
    uint64_t rbx;
    uint64_t rdx;
    uint64_t rcx;
    uint64_t rax;
    uint64_t int_no;
    uint64_t error_code;
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
} context_t;

typedef context_t registers;

typedef struct stackframe_t {
    struct stackframe_t *rbp;
    uint64_t rip;
} stackframe_t;

PACK(typedef struct idt_entry_t{
    uint16_t offset_low;
    uint16_t selector;
    uint8_t ist;
    uint8_t flags;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t resv;
}) idt_entry_t;

PACK(typedef struct idt_desc_t{
    uint16_t size;
    uint64_t address;
}) idt_desc_t;

void idt_init();
void idt_reinit();
void idt_set_ist(uint16_t vector, uint8_t ist);
void idt_install_irq(uint8_t irq, void *handler);


#endif