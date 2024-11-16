#pragma once

#include "../../../klib/klib.h"
#include "../../../acpi/madt.h"
#include "../../../acpi/acpi.h"

#define IOAPIC_REGSEL 0x0
#define IOAPIC_IOWIN  0x10

#define IOAPIC_ID     0x0
#define IOAPIC_VER    0x01
#define IOAPIC_ARB    0x02
#define IOAPIC_REDTBL 0x10

namespace IOAPIC{
    u64 __init Init();

    void Write(madt_ioapic* ioapic, u8 reg, u32 val);
    u32 Read(madt_ioapic* ioapic, u8 reg);

    void SetEntry(madt_ioapic* ioapic, u8 idx, u64 data);

    void RedirectIRQ(u32 lapic_id, u8 vec, u8 irq, bool mask);
    u32 GetRedirectIRQ(u8 irq);
}