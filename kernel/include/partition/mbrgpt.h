#pragma once

#include <stdint.h>
#include <stddef.h>

#include <pdef.h>

PACK(typedef struct mbr_dpt{
    uint8_t BootIndicator; //state
    uint8_t StartHead;
    uint8_t StartSector : 6;
    uint16_t StartCylinder : 10;
    uint8_t PartitionTypeIndicator;
    uint8_t EndHead;
    uint8_t EndSector : 6;
    uint16_t EndCylinder : 10;
    uint32_t StartLBA;
    uint32_t SectorsInPartition; //Sector number
})MBR_DPT;

#define MBR_TABLE_SIZE             512
#define MBR_PARTITION_MAX          4
#define MBR_PARTITION_TABLE_OFFSET 0x01BE
#define MBR_PARTITION_STATE_BOOTABLE 0x80
#define MBR_PARTITION_STATE_UNBOOTABLE 0x00

PACK(typedef struct gpt_header{
    char Signature[8];
    uint32_t Version;
    uint32_t HeadSizeSum;
    uint32_t CRCChecksum;
    uint32_t Rsvd;
    uint64_t HeadLocation;          //LBA
    uint64_t HeadBackupsLocation;   //LBA
    uint64_t PartitionStart;        //LBA
    uint64_t PartitionEnd;          //LBA
    __uint128_t SectorGUID;
    uint64_t HeadStart;             //LBA
    uint32_t NumberOfPTE;           //Number of partition table entries
    uint32_t TNOFBPPTE;             //The number of bytes per partition table entry
    uint32_t PartitionCRCChecksum;
    uint8_t Rsvd2[420];
})GPT_Header;

#define GPT_HEADER_NUMBER_OF_PTE_OFFSET 80

PACK(typedef struct gpt_pte{
    __uint128_t PartitionTypeGUID;
    __uint128_t PartitionGUID;
    uint64_t PartitionStart;        //LBA
    uint64_t PartitionEnd;          //LBA
    uint64_t PartitionAttr;
    uint16_t Name[36];
})GPT_PTE;

#define GPT_PARTITION_TABLE_OFFSET 1024

uint8_t IdentifyMBR(uint32_t DriverID);
uint8_t GetPartitionStart(uint32_t DriverID,uint32_t PartitionID,uint64_t PartitionStart);
uint8_t GetPartitionEnd(uint32_t DriverID,uint32_t PartitionID,uint64_t PartitionEnd);
uint8_t GetPartitionCount(uint32_t DriverID);