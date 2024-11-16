#include "acpi.h"
#include "../limine.h"



namespace ACPI{
    bool acpi_use_xsdt = false;

    void* acpi_root_sdt;

    void* FindTable(const char* name) {
        if (acpi_use_xsdt == false) {
            acpi_rsdt* rsdt = (acpi_rsdt*)acpi_root_sdt;
            u32 entries = (rsdt->sdt.len - sizeof(rsdt->sdt)) / 4;

            for (u32 i = 0; i < entries; i++) {
            struct ACPI_SDT* sdt = (struct ACPI_SDT*)HIGHER_HALF(*((u32*)rsdt->table + i));
            if (!_memcmp(sdt->sign, name, 4))
                return (void*)sdt;
            }
            return NULL;
        }

        // Use XSDT
        acpi_xsdt* xsdt = (acpi_xsdt*)acpi_root_sdt;
        u32 entries = (xsdt->sdt.len - sizeof(xsdt->sdt)) / 8;

        for (u32 i = 0; i < entries; i++) {
            struct ACPI_SDT* sdt = (struct ACPI_SDT*)HIGHER_HALF(*((u64*)xsdt->table + i));
            if (!_memcmp(sdt->sign, name, 4)) {
            return (void*)sdt;
            }
        }

        return NULL;
    }

    u64 Init(void *addr) {
        

        //void* addr = (void*)rsdp_request.response->address;
        struct ACPI_RSDP* rsdp = (struct ACPI_RSDP*)addr;

        //if (_memcmp(rsdp->sign, "RSD PTR", 7))
        //    return 0;
        //kinfo("ACPI: Copyed RSDP SIGN\n");
        
        if (rsdp->revision != 0) {
            // Use XSDT
            acpi_use_xsdt = true;
            struct ACPI_XSDP* xsdp = (struct ACPI_XSDP*)addr;
            acpi_root_sdt = (acpi_xsdt*)HIGHER_HALF((u64)xsdp->xsdt_addr);
            return xsdp->xsdt_addr;
        }
        
        acpi_root_sdt = (acpi_rsdt*)HIGHER_HALF((u64)rsdp->rsdt_addr);

        return rsdp->rsdt_addr;
    }
}