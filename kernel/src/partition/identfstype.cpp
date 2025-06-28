#include <partition/identfstype.h>
#include <limits.h>
#include <mem/heap.h>
#include <drivers/vsdev/vsdev.h>

#include <fs/fatfs/identfat.h>
#include <fs/lwext4/ext4.h>

FS_TYPE_ID IdentifyFSType(uint32_t DriverID,uint32_t PartitionID){
    /*
    uint64_t *PartitionStart;
    uint32_t PartitionCount;
    if(IdentifyMBR(DriverID) == 0){
        uint8_t i = 0;
        uint32_t SectorsInPartition;
        for(i;i < 4;i++){
            if(VsDev::DevList[DriverID].ops.ReadBytes(VsDev::DevList[DriverID].classp,
                MBR_PARTITION_TABLE_OFFSET + i * 16 + 12,
                4,&SectorsInPartition) == false)
                return {PARTITION_TYPE_UNKNOWN,1};
            if(SectorsInPartition == 0)
                break;
        }
        PartitionStart = (uint64_t*)kmalloc(i * sizeof(uint64_t));
        for(uint8_t j = 0;j < i;j++)
            if(GetPartitionStart(DriverID,PartitionID,PartitionStart[j]) != 0)
                return {PARTITION_TYPE_UNKNOWN,2};
        PartitionCount = i;
    }else{
        //uint32_t PartitionCount;
        if(VsDev::DevList[DriverID].ops.ReadBytes(VsDev::DevList[DriverID].classp,
        MBR_TABLE_SIZE + GPT_HEADER_NUMBER_OF_PTE_OFFSET,4,&PartitionCount) == false)
            return {PARTITION_TYPE_UNKNOWN,3};
        PartitionStart = (uint64_t*)kmalloc(PartitionCount * sizeof(uint64_t));

        for(uint8_t k = 0;k < PartitionCount;k++)
            if(GetPartitionStart(DriverID,PartitionID,PartitionStart[k]) != 0)
                return {PARTITION_TYPE_UNKNOWN,4};
    }
                */
    
    if(IdentifyExtx(DriverID,PartitionID).ErrorCode != 0 &&
        IdentifyExtx(DriverID,PartitionID).TypeID != PARTITION_TYPE_UNKNOWN)
        return IdentifyExtx(DriverID,PartitionID).TypeID;
    elif(IdentifyFat(DriverID,PartitionID).ErrorCode != 0 &&
        IdentifyFat(DriverID,PartitionID).TypeID != PARTITION_TYPE_UNKNOWN)
        return IdentifyFat(DriverID,PartitionID).TypeID;
    
}