#pragma once

#ifndef _SMBIOS_H_
#define _SMBIOS_H_

#include <klib/types.h>
#include <klib/klib.h>
#include <stdint.h>

namespace SMBIOS
{

    PACK(typedef struct SHeader {
        uint8_t type;
        uint8_t length;
        uint16_t handle;
    })Header;
    /*32-bit Entry Point Structure Format
    Offset	Name	                        Size
    0x00	Anchor String	                4 BYTEs
    0x04	Entry Point Checksum	        BYTE
    0x05	Entry Point Lenght	            BYTE
    0x06	SMBIOS Major Version	        BYTE
    0x07	SMBIOS Minor Version	        BYTE
    0x08	SMBIOS Structure Maximum Size	WORD
    0x0A	Entry Point Revision	        BYTE
    0x0B	Formatted Area	                5 BYTEs
    0x10	Intermediate Anchor String	    5 BYTEs
    0x15	Intermediate Checksum	        BYTE
    0x16	Structure Table Length	        WORD
    0x18	Structure Table Address	        DWORD
    0x1C	Number of Structures	        WORD
    0x1E	BCD Revision	                BYTE*/
    PACK(typedef struct ENTRY_POINT_32
    {
        uint8_t Anchor_String[4];
        uint8_t Checksum;
        uint8_t Length;
        uint8_t MajorVersion;
        uint8_t MinorVersion;
        uint16_t MaxStructureSize;
        uint8_t EntryPointRevision;
        uint8_t FormattedArea[5];
        uint8_t IntermediateAnchorString[5];
        uint8_t IntermediateChecksum;
        uint16_t TableLength;
        uint32_t TableAddress;
        uint16_t NumberOfStructures;
        uint8_t BCDRevision;
    })EntryPoint32;

    /*64-bit Entry Point Structure Format
    Offset	Name	                        Size
    0x00	Anchor String	                5 BYTEs
    0x05	Entry Point Checksum	        BYTE
    0x06	Entry Point Lenght	            BYTE
    0x07	SMBIOS Major Version	        BYTE
    0x08	SMBIOS Minor Version	        BYTE
    0x09	SMBIOS Docrev	                BYTE
    0x0A	Entry Point Revision	        BYTE
    0x0B	Reserved	                    BYTE
    0x0C	Structure Table Maximum Size	DWORD
    0x10	Structure Table Adress	        QWORD*/

    typedef struct ENTRY_POINT_64
    {
        uint8_t Anchor_String[5];
        uint8_t Checksum;
        uint8_t Length;
        uint8_t MajorVersion;
        uint8_t MinorVersion;
        uint8_t DocRev;
        uint8_t EntryPointRevision;
        uint8_t Reserved;
        uint32_t MaxStructureSize;
        uint64_t TableAddress;
    }EntryPoint64 __attribute__((packed));
}

#endif