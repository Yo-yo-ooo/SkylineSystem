#pragma once

#include <klib/klib.h>
#include <pdef.h>
#include <acpi/acpi.h>

PACK(typedef struct madt_t{
    ACPI::SDTHeader header;
    uint32_t lapic_address;
    uint32_t flags;
    char table[];
}) madt_t;

PACK(typedef struct madt_entry_t{
    uint8_t type;
    uint8_t len;
    char data[];
}) madt_entry_t;

PACK(typedef struct madt_lapic_t{
    uint8_t acpi_cpu_id;
    uint8_t apic_id;
    uint32_t flags;
}) madt_lapic_t;

PACK(typedef struct madt_ioapic_t{
    uint8_t ioapic_id;
    uint8_t resv;
    uint32_t ioapic_addr;
    uint32_t gsi_base;
}) madt_ioapic_t;

PACK(typedef struct madt_iso_t{
    uint8_t bus;
    uint8_t irq;
    uint32_t gsi;
    uint16_t flags;
}) madt_iso_t;

PACK(typedef struct madt_lapic_ovr_t{
    uint16_t resv;
    uint64_t lapic_addr;
}) madt_lapic_ovr_t;

extern volatile madt_ioapic_t *madt_ioapic;
extern volatile madt_iso_t *madt_iso_list[16];
extern volatile uint64_t madt_apic_address;

void MADT_Init();