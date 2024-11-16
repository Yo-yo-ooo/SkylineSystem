#pragma once

#include <stdint.h>
#include <stddef.h>

#include "../klib/klib.h"

typedef struct ACPI_RSDP{
    char sign[8];
    u8 checksum;
    char oem_id[6];
    u8 revision;
    u32 rsdt_addr;
} __attribute__((packed)) acpi_rsdp;

typedef struct ACPI_XSDP{
    char sign[8];
    u8 checksum;
    char oem_id[6];
    u8 revision;
    u32 resv;

    u32 length;
    u64 xsdt_addr;
    u8 extended_checksum;
    u8 resv1[3];
} __attribute__((packed)) acpi_xsdp;

typedef struct ACPI_SDT{
    char sign[4];
    u32 len;
    u8 revision;
    u8 checksum;
    char oem_id[6];
    char oem_table_id[8];
    u32 oem_revision;
    u32 creator_id;
    u32 creator_revision;
} __attribute__((packed)) acpi_sdt;

typedef struct ACPI_RSDT{
    struct ACPI_SDT sdt;
    char table[];
} acpi_rsdt;

typedef struct ACPI_XSDT{
    struct ACPI_SDT sdt;
    char table[];
} acpi_xsdt;


namespace ACPI{
    extern bool acpi_use_xsdt;
    extern void* acpi_root_sdt;

    u64 Init(void *addr);
    void* FindTable(const char* name);
}