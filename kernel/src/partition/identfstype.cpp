#include <partition/identfstype.h>
#include <limits.h>
#include <mem/heap.h>
#include <drivers/dev/dev.h>

#include <fs/fatfs/identfat.h>
#include <fs/lwext4/ext4.h>

#include <conf.h>

FS_TYPE_ID IdentifyFSType(uint32_t DriverID,uint32_t PartitionID){
    /*
    uint64_t *PartitionStart;
    uint32_t PartitionCount;
    if(IdentifyMBR(DriverID) == 0){
        uint8_t i = 0;
        uint32_t SectorsInPartition;
        for(i;i < 4;i++){
            if(Dev::DevList_[DriverID].ops.ReadBytes(Dev::DevList_[DriverID].classp,
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
        if(Dev::DevList_[DriverID].ops.ReadBytes(Dev::DevList_[DriverID].classp,
        MBR_TABLE_SIZE + GPT_HEADER_NUMBER_OF_PTE_OFFSET,4,&PartitionCount) == false)
            return {PARTITION_TYPE_UNKNOWN,3};
        PartitionStart = (uint64_t*)kmalloc(PartitionCount * sizeof(uint64_t));

        for(uint8_t k = 0;k < PartitionCount;k++)
            if(GetPartitionStart(DriverID,PartitionID,PartitionStart[k]) != 0)
                return {PARTITION_TYPE_UNKNOWN,4};
    }
                */
    if(IdentifyExtx(DriverID,PartitionID,ENABLE_VIRT_IMAGE).ErrorCode != 0 &&
        IdentifyExtx(DriverID,PartitionID,ENABLE_VIRT_IMAGE).TypeID != PARTITION_TYPE_UNKNOWN)
        return IdentifyExtx(DriverID,PartitionID,ENABLE_VIRT_IMAGE).TypeID;

    elif(IdentifyFat(DriverID,PartitionID,ENABLE_VIRT_IMAGE).ErrorCode != 0 &&
        IdentifyFat(DriverID,PartitionID,ENABLE_VIRT_IMAGE).TypeID != PARTITION_TYPE_UNKNOWN)
        return IdentifyFat(DriverID,PartitionID,ENABLE_VIRT_IMAGE).TypeID;
    else
        return PARTITION_TYPE_UNKNOWN;
}

FF_DESC *fs_desc;

bool FSAllIdentify(){
    uint8_t ff_count;
    fs_desc = (FF_DESC*)kmalloc(10 * sizeof(FF_DESC));

    for(uint32_t i = 0;i < Dev::vsdev_list_idx;i++){
        if(Dev::DevList_[i].type != VsDevType::NSDEV
        &&  Dev::DevList_[i].type != VsDevType::Undefined){
            
            for(uint8_t j = 0;j < GetPartitionCount(i);j++){
                uint8_t type = IdentifyFSType(i,j);
                /*
                if(type == PARTITION_TYPE_EXFAT || type == PARTITION_TYPE_FAT12 ||
                    type == PARTITION_TYPE_FAT16 || type == PARTITION_TYPE_FAT32){
                    
                    //Not Complete
                    if(ff_count >= 10)
                        continue;
                    else
                        ff_count++;
                }
                        */
                fs_desc[ff_count].FSType = type;
                fs_desc[ff_count].DriverID = i;
                fs_desc[ff_count].PartitionID = j;
                fs_desc[ff_count].Prdv = ff_count;
                if(GetPartitionStart(i,j,fs_desc[ff_count].PStart) != 0)
                    return false;
                if(GetPartitionEnd(i,j,fs_desc[ff_count].PEnd) != 0)
                    return false;
                fs_desc[ff_count].Size = fs_desc[ff_count].PEnd - fs_desc[ff_count].PStart;
                if(ff_count > 10){
                    fs_desc = (FF_DESC*)krealloc(fs_desc,ff_count * sizeof(FF_DESC));
                }

                ff_count++;
            }
        }
    }
    return true;
}

//shutdown func:kfree(fs_desc);