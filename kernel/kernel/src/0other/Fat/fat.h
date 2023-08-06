#pragma once
#include "../../kernelStuff/diskStuff/Disk_Interfaces/generic/genericDiskInterface.h"
#include "../../fsStuff/fsStuff.h"
#include "../../osData/osData.h"

struct BiosParameterBlock32
{
    uint8_t jump[3];
    uint8_t softName[8];
    uint16_t bytesPerSector;
    uint8_t sectorsPerCluster;
    uint16_t reservedSectors;
    uint8_t fatCopies;
    uint16_t rootDirEntries;
    uint16_t totalSectors;
    uint8_t mediaType;
    uint16_t fatSectorCount;
    uint16_t sectorsPerTrack;
    uint16_t headCount;
    uint32_t hiddenSectors;
    uint32_t totalSectorCount;
    
    uint32_t tableSize;
    uint16_t extFlags;
    uint16_t fatVersion;
    uint32_t rootCluster;
    uint16_t fatInfo;
    uint16_t backupSector;
    uint8_t reserved0[12];
    uint8_t driveNumber;
    uint8_t reserved;
    uint8_t bootSignature;
    uint32_t volumeId;
    uint8_t volumeLabel[11];
    uint8_t fatTypeLabel[8];
} __attribute__((packed));


struct DirectoryEntryFat32
{
    uint8_t name[8];
    uint8_t ext[3];
    uint8_t attributes;
    uint8_t reserved;
    uint8_t cTimeTenth;
    uint16_t cTime;
    uint16_t cDate;
    uint16_t aTime;
    uint16_t firstClusterHi;
    uint16_t wTime;
    uint16_t wDate;
    uint16_t firstClusterLow;
    uint32_t size;
} __attribute__((packed));

struct PartitionTableEntry
{
    uint8_t bootable;

    uint8_t start_head;
    uint8_t start_sector : 6;
    uint16_t start_cylinder : 10;

    uint8_t partition_id;

    uint8_t end_head;
    uint8_t end_sector : 6;
    uint16_t end_cylinder : 10;
    
    uint32_t start_lba;
    uint32_t length;
} __attribute__((packed));

struct MasterBootRecord
{
    uint8_t bootloader[440];
    uint32_t signature;
    uint16_t unused;
    
    PartitionTableEntry primaryPartition[4];
    
    uint16_t magicnumber;
} __attribute__((packed));

struct DirectoryEntry{

    uint8_t    name[8];
    uint8_t    extension[3];
    uint8_t    attributes;
    uint8_t    reserved;

    uint8_t    creationTimeTenth;
    uint16_t   creationTime;
    uint16_t   creationDate;
    uint16_t   lastAccessDate;

    uint16_t   firstClusterHigh;

    uint16_t  lastWriteTime;
    uint16_t  lastWriteDate;

    uint16_t  firstClusterLow;

    uint32_t  size;

} __attribute__((packed));

//DirectoryEntry dirent[50];


//FRBB_t ReadBiosBlock(int disknum, uint32_t partitionOffset);
class FatManager
{
private:
    int NODI;
public:
    FatManager(int NumOfDiskI);
    void RemoveDirectory(char *name);
    void MakeDirectory(char *name);
    ~FatManager();
};
