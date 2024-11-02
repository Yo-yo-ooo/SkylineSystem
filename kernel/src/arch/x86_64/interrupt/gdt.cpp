#include "gdt.h"
#include "../lapic/lapic.h"

gdt_table def_table = {
  {
  0x0000000000000000, // 0x00

  0x00009a000000ffff, // 0x08 16 bit code
  0x000093000000ffff, // 0x10 16 bit data

  0x00cf9a000000ffff, // 0x18 32 bit code
  0x00cf93000000ffff, // 0x20 32 bit data

  0x00af9b000000ffff, // 0x28 64 bit code cs
  0x00af93000000ffff, // 0x30 64 bit data ss

  0x00aff3000000ffff, // 0x38 data ss
  0x00affb000000ffff, // 0x40 user mode code cs
  },
  {
  }
};

tssr tss_list[256]; // One tssr per CPU

void gdt_init() {
    tss_list[lapic_get_id()].iopb = sizeof(tssr);
    uptr tss = (uptr)&tss_list[lapic_get_id()];

    def_table.tss_entry.length = sizeof(tss_entry);
    def_table.tss_entry.base = (u16)(tss & 0xffff);
    def_table.tss_entry.base1 = (u8)((tss >> 16) & 0xff);
    def_table.tss_entry.flags = 0x89;
    def_table.tss_entry.flags1 = 0;
    def_table.tss_entry.base2 = (u8)((tss >> 24) & 0xff);
    def_table.tss_entry.base3 = (u32)(tss >> 32);
    def_table.tss_entry.resv = 0;
    
    // Thanks for Abbix and Solar (from the osdev server) to get me this thing working.

    gdtr gdt = (gdtr){
        .size = (sizeof(gdt_table)) - 1,
        .address = (u64)&def_table
    };

    __asm__ volatile ("lgdt %0\n\t" : : "m"(gdt) : "memory");
    __asm__ volatile ("ltr %0\n\t" : : "r"((u16)0x48));
}