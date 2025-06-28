#include <partition/mbrgpt.h>

#define PARTITION_TYPE_UNKNOWN      0
#define PARTITION_TYPE_EXT4         1
#define PARTITION_TYPE_EXT3         2
#define PARTITION_TYPE_EXT2         3
#define PARTITION_TYPE_FAT32        4
#define PARTITION_TYPE_FAT16        5
#define PARTITION_TYPE_FAT12        6
#define PARTITION_TYPE_EXFAT        7

/*
ErrorCode:
0: [MBR/GPT]    All is OK!
1: [MBR]        Can't MBR Partitions Count 
2: [MBR]        Can't Get Partition Start ADDR
3: [GPT]        Can't Get GPT Partitions Count 
4: [GPT]        Can't Get Partition Start ADDR
5: [FS]         Read Partition Start Addr ERROR
*/
typedef struct FS_TYPE{           
    uint8_t TypeID;     
    uint8_t ErrorCode;
};

FS_TYPE IdentifyFSType(uint32_t DriverID,uint32_t PartitionID);