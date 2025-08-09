#include <acpi/madt.h>
#include <acpi/acpi.h>

volatile madt_ioapic_t *madt_ioapic = nullptr;
volatile madt_iso_t *madt_iso_list[16] = {0};
volatile uint64_t madt_apic_address = 0;

void MADT_Init() {
    //acpi_madt* madt = (acpi_madt*)ACPI::FindTable(ACPI::rootThing,"APIC",ACPI::ACPI_DIV);

    _memset(madt_iso_list, 0, 16 * sizeof(madt_iso_t));
    madt_t *madt = (madt_t*)ACPI::FindTable(ACPI::rootThing,"APIC",ACPI::ACPI_DIV);
    ASSERT(PHYSICAL(madt));
    kpok("Found MADT.\n");
    madt_apic_address = madt->lapic_address;
    uint64_t offset = 0;
    while (offset < madt->header.Length - sizeof(madt_t)) {
        madt_entry_t *entry = (madt_entry_t*)(madt->table + offset);
        if (entry->type == 1) {
            if (madt_ioapic)
                kwarn("More than 1 I/O APICs found.\n");
            else
                madt_ioapic = (madt_ioapic_t*)entry->data;
        } else if (entry->type == 2) {
            madt_iso_t *iso = (madt_iso_t*)entry->data;
            if (madt_iso_list[iso->irq])
                kwarn("Found ISO for (already found) IRQ #%d.\n", iso->irq);
            else {
                kpok("Found ISO for IRQ %d.\n", iso->irq);
                madt_iso_list[iso->irq] = iso;
            }
        } else if (entry->type == 5) {
            kinfo("Found APIC Address Override.\n");
            madt_lapic_ovr_t *lapic_ovr = (madt_lapic_ovr_t*)entry->data;
            madt_apic_address = lapic_ovr->lapic_addr;
        }
        offset += entry->len;
    }
}