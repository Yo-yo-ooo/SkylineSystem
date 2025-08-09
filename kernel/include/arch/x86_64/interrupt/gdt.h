#pragma once

#include <klib/klib.h>
#include <pdef.h>

PACK(typedef struct {
    uint16_t len;
    uint16_t base;
    uint8_t base1;
    uint8_t flags;
    uint8_t flags1;
    uint8_t base2;
    uint32_t base3;
    uint32_t resv;
}) tss_entry_t;

PACK(typedef struct {
    uint32_t resv;
    uint64_t rsp[3];
    uint64_t resv1;
    uint64_t ist[7];
    uint64_t resv2;
    uint16_t resv3;
    uint16_t iopb;
}) tss_desc_t;

PACK(typedef struct {
    uint64_t entries[5];
    tss_entry_t tss_entry;
}) gdt_table_t;

PACK(typedef struct {
    uint16_t size;
    uint64_t address;
}) gdt_desc_t;

namespace GDT{
    void Init(uint32_t cpu_num);
}
//void gdt_init(uint32_t cpu_num);

namespace TSS{
    void SetRSP(uint32_t cpu_num, int rsp, void *stack);
    void SetIST(uint32_t cpu_num, int ist, void *stack); 
}
