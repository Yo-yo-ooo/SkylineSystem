#pragma once

#ifndef _PCI_H
#define _PCI_H

#include <stdint.h>
#include "../../../../klib/klib.h"

#define PCI_BAR0 0x10

#define PCI_MAX_BUS 256
#define PCI_MAX_SLOT 32
#define PCI_MAX_FUNC 8

namespace PCI{
    enum PCI_BAR_TYPE_ENUM 
    {
        NONE = 0,
        MMIO64,
        MMIO32,
        IO
    };

    struct PCI_BAR_TYPE 
    {
        uint64_t mem_address;
        uint16_t io_address;
        PCI_BAR_TYPE_ENUM type;
        uint16_t size;
    };

    typedef struct mcfg_entry_{
        u64 addr;
        u16 seg;
        u8 first_bus;
        u8 last_bus;
        u32 resv;
    }mcfg_entry __attribute__((packed)) ;

    typedef struct acpi_mcfg_{
        char sign[4];
        u32 len;
        u8 revision;
        u8 checksum;
        char oem_id[6];
        char oem_table_id[8];
        u32 oem_revision;
        u32 creator_id;
        u32 creator_revision;
        u64 resv;

        mcfg_entry table[];
    }acpi_mcfg  __attribute__((packed));

    typedef struct {
        u8 bus;
        u8 function;
        u8 kind;
        u8 subclass;
        u16 device_id;
        u16 vendor_id;
        u32 bars[6];
    } pci_device;

    struct PCIDeviceHeader
    {
        uint16_t Vendor_ID;
        uint16_t Device_ID;
        uint16_t Command;
        uint16_t Status;
        uint8_t Revision_ID;
        uint8_t Prog_IF;
        uint8_t SubClass;
        uint8_t Class;
        uint8_t CacheLineSize;
        uint8_t LatencyTimer;
        uint8_t HeaderType;
        uint8_t BIST;
    }__attribute__((packed));

    struct PCIHeader0 {
        PCIDeviceHeader Header;
        uint32_t BAR0;
        uint32_t BAR1;
        uint32_t BAR2;
        uint32_t BAR3;
        uint32_t BAR4;
        uint32_t BAR5;
        uint32_t CardbusCISPtr;
        uint16_t SubsystemVendorID;
        uint16_t SubsystemID;
        uint32_t ExpansionROMBaseAddr;
        uint8_t CapabilitiesPtr;
        uint8_t Rsv0;
        uint16_t Rsv1;
        uint32_t Rsv2;
        uint8_t InterruptLine;
        uint8_t InterruptPin;
        uint8_t MinGrant;
        uint8_t MaxLatency;
    }__attribute__((packed));

    extern pci_device pci_list[128];

    void Init();

    u16 ReadWord(u8 bus, u8 slot, u8 func, u8 offset);
    u32 ReadDword(u8 bus, u8 slot, u8 func, u8 offset);

    void WriteByte(u8 bus, u8 dev, u8 func, u8 offset, u8 value);
    void WriteWord(u8 bus, u8 dev, u8 func, u8 offset, u16 value);
    void WriteDword(u8 bus, u8 slot, u8 func, u8 offset, u32 value);
    
    i8 FindDevice(u8 kind, u8 subclass);
    void AddDevice(u8 bus, u8 func, u8 kind, u8 subclass, u16 device_id, u16 vendor_id, u32* bars);

    extern const char* unknownString;
    const char* GetVendorName(uint16_t vendorID);
    const char* GetDeviceName(uint16_t vendorID, uint16_t deviceID);
    const char* GetClassName(uint8_t classCode);
    const char* GetSubclassName(uint8_t classCode, uint8_t subclassCode);
    const char* GetProgIFName(uint8_t classCode, uint8_t subclassCode, uint8_t progIFCode);
}

#endif