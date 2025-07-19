#include <partition/mgr.h>
#include <drivers/dev/dev.h>
#include <partition/identfstype.h>
#include <partition/mbrgpt.h>

namespace PartitionManager
{
    uint32_t CurDriver = 0;
    uint8_t CurPartition = 0;
    uint64_t CurPartitionStart = 0;
    uint64_t CurPartitionEnd = 0;

    void SetCurPartition(uint32_t driverID,uint8_t partitionID){
        CurDriver = driverID;
        CurPartition = partitionID;
#ifdef USE_VIRT_IMAGE
        return;
#else
        if(GetPartitionStart(PartitionManager::CurDriver,
            PartitionManager::CurPartition,PartitionManager::CurPartitionStart) != 0)
            kerrorln("Cannot change to partition %d",partitionID);
        else if(GetPartitionEnd(PartitionManager::CurDriver,
            PartitionManager::CurPartition,PartitionManager::CurPartitionEnd) != 0)
            kerrorln("Cannot change to partition %d",partitionID);
        else return;
#endif
    }

    uint8_t Read(uint64_t lba, uint32_t SectorCount, void* Buffer){
#ifdef USE_VIRT_IMAGE
        return Dev::DevList_[PartitionManager::CurDriver].ops.Read(
                Dev::DevList_[PartitionManager::CurDriver].classp,
                lba,SectorCount,Buffer
        );
#else
        if(((PartitionManager::CurPartitionStart) % 512 == 0 ? 
        (PartitionManager::CurPartitionStart) / 512 :
        (PartitionManager::CurPartitionStart) / 512 + 1) + lba > CurPartitionEnd){
            kerrorln("Address > CurPartitionEnd");
            return false;
        }else{
            if(Dev::DevList_[PartitionManager::CurDriver].ops.Read(
                Dev::DevList_[PartitionManager::CurDriver].classp,
                (PartitionManager::CurPartitionStart) / 512 + lba,Count,Buffer
            ) != Dev::RW_OK)
                return true;
            else return false;
        }
#endif
    }
    uint8_t Write(uint64_t lba, uint32_t SectorCount, void* Buffer){
#ifdef USE_VIRT_IMAGE
        return Dev::DevList_[PartitionManager::CurDriver].ops.Write(
                Dev::DevList_[PartitionManager::CurDriver].classp,
                lba,SectorCount,Buffer
        );
#else
        if(((PartitionManager::CurPartitionStart) % 512 == 0 ? 
        (PartitionManager::CurPartitionStart) / 512 :
        (PartitionManager::CurPartitionStart) / 512 + 1) + lba > CurPartitionEnd){
            kerrorln("Address > CurPartitionEnd");
            return false;
        }else{
            return Dev::DevList_[PartitionManager::CurDriver].ops.Write(
                Dev::DevList_[PartitionManager::CurDriver].classp,
                (PartitionManager::CurPartitionStart) / 512 + lba,Count,Buffer
            );
        }
#endif
    }

    uint8_t ReadBytes(uint64_t address, uint32_t Count, void* Buffer){
#ifdef USE_VIRT_IMAGE
        return Dev::DevList_[PartitionManager::CurDriver].ops.ReadBytes(
                Dev::DevList_[PartitionManager::CurDriver].classp,
                address,Count,Buffer);
        
#else
        if(PartitionManager::CurPartitionStart + address > CurPartitionEnd){
            kerrorln("Address > CurPartitionEnd");
            return false;
        }else{
            if(Dev::DevList_[PartitionManager::CurDriver].ops.ReadBytes(
                Dev::DevList_[PartitionManager::CurDriver].classp,
                PartitionManager::CurPartitionStart + address,Count,Buffer
            ) != Dev::RW_OK)
                return true;
            else return false;
        }
#endif
    }
    uint8_t WriteBytes(uint64_t address, uint32_t Count, void* Buffer){
#ifdef USE_VIRT_IMAGE
        return Dev::DevList_[PartitionManager::CurDriver].ops.WriteBytes(
                Dev::DevList_[PartitionManager::CurDriver].classp,
                address,Count,Buffer
        );
#else
        if(PartitionManager::CurPartitionStart + address > CurPartitionEnd){
            kerrorln("Address > CurPartitionEnd");
            return false;
        }else{
            if(Dev::DevList_[PartitionManager::CurDriver].ops.WriteBytes(
                Dev::DevList_[PartitionManager::CurDriver].classp,
                PartitionManager::CurPartitionStart + address,Count,Buffer
            ) != Dev::RW_OK)
                return true;
            else return false;
        }
#endif
    }

    uint64_t GetMaxSectorCount(){
#ifdef USE_VIRT_IMAGE
        return 
        Dev::DevList_[PartitionManager::CurDriver].
        ops.GetMaxSectorCount(Dev::DevList_[PartitionManager::CurDriver].classp);
#else
        return GetPartitionSize(CurDriver,CurPartition) / 512 + 1;
#endif
    }


} // namespace PartitionManager
