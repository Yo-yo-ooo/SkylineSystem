#pragma once
#ifndef _X86_64_HPET_H_
#define _X86_64_HPET_H_

#include <stdint.h>
#include <stddef.h>
#include <pdef.h>
#include <acpi/acpi.h>
#include <conf.h>

typedef struct [[gnu::packed]] {
    ACPI::SDTHeader hdr;

    uint8_t hardware_rev_id;
    uint8_t comparator_count:5;
    uint8_t counter_size:1;
    uint8_t reserved:1;
    uint8_t legacy_replace:1;
    uint16_t pci_vendor_id;

    ACPI::gas_t base_addr;

    uint8_t hpet_number;
    uint16_t minimum_tick;
    uint8_t page_protection;
} hpet_sdt_t;

typedef struct[[gnu::packed]] {
    volatile uint64_t config_and_capabilities;
    volatile uint64_t comparator_value;
    volatile uint64_t fsb_interrupt_route;
    volatile uint64_t unused;
} hpet_timer_t;

typedef struct[[gnu::packed]] {
    volatile uint64_t general_capabilities;
    volatile uint64_t unused0;
    volatile uint64_t general_configuration;
    volatile uint64_t unused1;
    volatile uint64_t general_int_status;
    volatile uint64_t unused2;
    volatile uint64_t unused3[2][12];
    volatile uint64_t main_counter_value;
    volatile uint64_t unused4;
    hpet_timer_t timers[];
} hpet_t;

namespace HPET
{
    
    void InitHPET();
    uint64_t GetTimeNS();
    void SleepNS(uint64_t ns);
}

#endif