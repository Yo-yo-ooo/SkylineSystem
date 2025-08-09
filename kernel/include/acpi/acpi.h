#pragma once

#include <stdint.h>
#include <stddef.h>

#include <klib/klib.h>


namespace ACPI{
    
    struct RSDP1
    {
        uint8_t Signature[8];
        uint8_t Checksum;
        uint8_t OEM_ID[6];
        uint8_t Revision;
        uint32_t RSDTAddress;
    } __attribute__((packed));

    struct RSDP2
    {
        RSDP1 firstPart;

        uint32_t Length;
        uint64_t XSDTAddress;
        uint8_t ExtendedChecksum;
        uint8_t Reserved[3];

    } __attribute__((packed));



    struct SDTHeader
    {
        uint8_t Signature[4];
        uint32_t Length;
        uint8_t Revision;
        uint8_t Checksum;
        uint8_t OEM_ID[6];
        uint8_t OEM_TABLE_ID[8];
        uint32_t OEM_Revision;
        uint32_t Creator_ID; 
        uint32_t CreatorRevision;
    } __attribute__((packed));

    

    struct DeviceConfig
    {
        uint64_t BaseAddress;
        uint16_t PCISegGroup;
        uint8_t StartBus;
        uint8_t EndBus;
        uint32_t Reserved;
    } __attribute__((packed));

    struct MCFGHeader
    {
        SDTHeader Header;
        uint64_t Reserved;
        DeviceConfig ENTRIES[];
    } __attribute__((packed));

    typedef struct SDT{
        ACPI::SDTHeader header;
        char table[];
    } __attribute__((packed)) SDT;

    extern ACPI::MCFGHeader* mcfg;
    extern ACPI::SDTHeader* rootThing;
    extern int32_t ACPI_DIV;
    extern volatile bool UseXSDT;

    u64 Init(void *addr);
    void* FindTable(SDTHeader* sdtHeader, const char* signature, int32_t div);
    void* FindTable(const char* signature);
}