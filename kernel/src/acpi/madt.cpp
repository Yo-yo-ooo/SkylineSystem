#include "madt.h"
#include "acpi.h"

madt_ioapic* madt_ioapic_list[128] = {0};
madt_iso* madt_iso_list[128] = {0};

u32 madt_ioapic_len = 0;
u32 madt_iso_len = 0;

u64* lapic_addr = NULL;

void MADT_Init() {
    acpi_madt* madt = (acpi_madt*)ACPI::FindTable("APIC");

    u64 off = 0;
    int current_idx = 0;
    madt_ioapic_len = 0;
    madt_iso_len = 0;

    while (true) {
        if (off > madt->len - sizeof(madt))
        break;
        
        madt_entry* entry = (madt_entry*)(madt->table + off);

        switch (entry->type) {
        case 0:
            current_idx++;
            break;
        case 1:
            madt_ioapic_list[madt_ioapic_len++] = (madt_ioapic*)entry;
            break;
        case 2:
            madt_iso_list[madt_iso_len++] = (madt_iso*)entry;
            break;
        case 5:
            lapic_addr = (u64*)((madt_lapic_addr*)entry)->phys_lapic;
            break;
        }

        off += entry->len;
    }
}