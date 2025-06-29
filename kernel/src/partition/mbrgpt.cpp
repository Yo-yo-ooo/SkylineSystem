#include <partition/mbrgpt.h>
#include <drivers/dev/dev.h>
#include <mem/heap.h>

uint8_t IdentifyMBR(uint32_t DriverID){
    if(DriverID > Dev::vsdev_list_idx)
        return 1; //DriverID ERR
    
    DevList ThisInfo = Dev::DevList_[DriverID];
    MBR_DPT dpt; 
    Dev::SetSDev(DriverID);
    if(Dev::ReadBytes(MBR_PARTITION_TABLE_OFFSET,16,&dpt) == false)
        return 2;
    if(dpt.PartitionTypeIndicator == 0xEE && dpt.BootIndicator == 0x00){//GPT
        return 3;
    }else{
        return 0;
    }
    return 4;
}

uint8_t GetPartitionStart(uint32_t DriverID,uint32_t PartitionID,uint64_t PartitionStart){
    if(DriverID > Dev::vsdev_list_idx)
        return 1; //DriverID ERR
    
    DevList ThisInfo = Dev::DevList_[DriverID];
    MBR_DPT dpt; 
    uint32_t buffer;
    Dev::SetSDev(DriverID);
    if(Dev::ReadBytes(MBR_PARTITION_TABLE_OFFSET,16,&dpt) == false)
        return 2;
        //80
    if(Dev::ReadBytes(MBR_TABLE_SIZE + GPT_HEADER_NUMBER_OF_PTE_OFFSET,4,&buffer) == false)
        return 3;

    if(dpt.PartitionTypeIndicator == 0xEE && dpt.BootIndicator == 0x00){//GPT
        if(PartitionID > buffer)
            return 4;
        GPT_PTE gptpte;
        if(Dev::ReadBytes(GPT_PARTITION_TABLE_OFFSET + PartitionID * 128,
                128,&gptpte) == false)
                return 5;
        PartitionStart = gptpte.PartitionStart;
        return 0;
    }else{

        if(PartitionID > MBR_PARTITION_MAX)
            return 6; //PartitionID ERR
        
        /*
        void *table_start = buffer + MBR_PARTITION_TABLE_OFFSET;
        int member_sz = sizeof(MBR_DPT);

        __memcpy(member, table_start + number * member_sz, member_sz);
        */
        MBR_DPT buffer2;
        //Dev::SetSDev(DriverID);
        if(Dev::ReadBytes(MBR_PARTITION_TABLE_OFFSET + PartitionID * 16,
            16,&buffer2) == false)
            return 7;
        
        //CHS To LBA : easy to R/W 
        PartitionStart = buffer2.StartLBA;
        return 0;
    }
    return 0;
}
