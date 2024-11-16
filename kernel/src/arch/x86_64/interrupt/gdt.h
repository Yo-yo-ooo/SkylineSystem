#pragma once

#include "../../../klib/klib.h"

typedef struct tss_entry_{
    u16 length;
    u16 base;
    u8  base1;
    u8  flags;
    u8  flags1;
    u8  base2;
    u32 base3;
    u32 resv;
} __attribute__((packed)) tss_entry;

typedef struct gdt_table_{
    u64 gdt_entries[9];
    struct tss_entry_ tss_entry;
} __attribute__((packed)) gdt_table;

typedef struct gdtr_{
    u16 size;
    u64 address;
} __attribute__((packed)) gdtr;

typedef struct tssr_{
    u32 resv;
    u64 rsp[3];
    u64 resv1;
    u64 ist[7];
    u64 resv2;
    u16 resv3;
    u16 iopb;
} __attribute__((packed)) tssr; // Per CPU

void gdt_init();

extern tssr tss_list[256];
