#include "../klib/klib.h"

typedef struct {
    char sign[4];
    u32 len;
    u8 revision;
    u8 checksum;
    char oem_id[6];
    char oem_table_id[8];
    u32 oem_revision;
    u32 creator_id;
    u32 creator_revision;

    /* MADT Specs */
    u32 lapic_address;
    u32 flags;

    char table[];
} acpi_madt;

typedef struct {
    u8 type;
    u8 len;
} madt_entry;

typedef struct {
    madt_entry un;
    u8 cpu_id;
    u8 apic_id;
    u32 flags;
} madt_cpu_lapic;

typedef struct {
    madt_entry un;
    u8 apic_id;
    u8 resv;
    u32 apic_addr;
    u32 gsi_base;
} madt_ioapic;

typedef struct {
    madt_entry un;
    u8 bus_src;
    u8 irq_src;
    u32 gsi;
    u16 flags;
} madt_iso;

typedef struct {
    madt_entry un;
    u16 resv;
    u64 phys_lapic;
} madt_lapic_addr;

extern madt_ioapic* madt_ioapic_list[128];
extern madt_iso* madt_iso_list[128];

extern u32 madt_ioapic_len;
extern u32 madt_iso_len;

extern u64* lapic_addr;

void MADT_Init();