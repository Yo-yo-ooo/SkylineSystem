#pragma once

#include <klib/klib.h>
#include <acpi/madt.h>
#include <acpi/acpi.h>

#define IOAPIC_REGSEL 0x0
#define IOAPIC_IOWIN  0x10

#define IOAPIC_ID     0x0
#define IOAPIC_VER    0x01
#define IOAPIC_ARB    0x02
#define IOAPIC_REDTBL 0x10



namespace IOAPIC{
    extern volatile uint64_t ioapic_address;

    void Init();

    uint32_t Read(uint8_t reg);

    void Write(uint8_t reg, uint32_t data);

    void RemapGSI(uint32_t lapic_id, uint32_t gsi, uint8_t vec, uint32_t flags);
    void RemapIRQ(uint32_t lapic_id, uint8_t irq, uint8_t vec, bool masked);
}