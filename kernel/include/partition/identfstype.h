#pragma once

#include <partition/mbrgpt.h>

#define PARTITION_TYPE_UNKNOWN      0
#define PARTITION_TYPE_EXT4         1
#define PARTITION_TYPE_EXT3         2
#define PARTITION_TYPE_EXT2         3
#define PARTITION_TYPE_FAT32        4
#define PARTITION_TYPE_FAT16        5
#define PARTITION_TYPE_FAT12        6
#define PARTITION_TYPE_EXFAT        7

typedef uint8_t FS_TYPE_ID;

/*
ErrorCode:
0: [MBR/GPT]    All is OK!
1: [MBR]        Can't MBR Partitions Count 
2: [MBR]        Can't Get Partition Start ADDR
3: [GPT]        Can't Get GPT Partitions Count 
4: [GPT]        Can't Get Partition Start ADDR
5: [FS]         Read Partition Start Addr ERROR
6: [FS]         Read Partition Base INFO ERROR
7: [FS]         Check FS Feature ERROR
*/
typedef struct FS_TYPE_{           
    uint8_t TypeID;     //See Line 5 ~ Line 12
    uint8_t ErrorCode;
}FS_TYPE;

FS_TYPE_ID IdentifyFSType(uint32_t DriverID,uint32_t PartitionID);

bool FSAllIdentify();
void FSPrintDesc();