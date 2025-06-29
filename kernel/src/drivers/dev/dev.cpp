#include <drivers/dev/dev.h>
#pragma GCC push_options
#pragma GCC optimize ("O0")
namespace Dev{
    VDL DevList_[MAX_VSDEV_COUNT] = {0};
    uint32_t vsdev_list_idx = 0;

    u32 ThisDev = 0;
    void Init(){
        vsdev_list_idx = 0;
        ThisDev = 0;
        _memset(&ThisDev,0,sizeof(VsDevInfo));
    }

    char* TypeToString(VsDevType type){
        switch(type){
            case SATA:
                return "sata";
            case IDE:
                return "ide";
            case NVME:
                return "nvme";
            case SAS:
                return "sas";
            case Undefined:
                return "UNDEF";
        }
    }

    void AddStorageDevice(VsDevType type, DevOPS ops,u32 SectorCount = 0,void* Class = nullptr)
    {
        lock(&DevList_->lock);
        DevList_[vsdev_list_idx].Name = (char*)kmalloc(strlen(TypeToString(type)) + strlen((char*)to_string((uint64_t)vsdev_list_idx)) + 1);
        char* temp_str = StrCombine(TypeToString(type),to_string((uint64_t)vsdev_list_idx));
        DevList_[vsdev_list_idx].Name = temp_str;
        DevList_[vsdev_list_idx].type = type;
        
        if(Class != nullptr){
            DevList_[vsdev_list_idx].buf = true;
            DevList_[vsdev_list_idx].classp = Class;
        }else{
            DevList_[vsdev_list_idx].buf = false;
            DevList_[vsdev_list_idx].classp = nullptr;
        }
        DevList_[vsdev_list_idx].ops.GetMaxSectorCount = ops.GetMaxSectorCount;
        DevList_[vsdev_list_idx].ops.Read = ops.Read;
        DevList_[vsdev_list_idx].ops.Write = ops.Write;
        DevList_[vsdev_list_idx].ops.ReadBytes = ops.ReadBytes;
        DevList_[vsdev_list_idx].ops.WriteBytes = ops.WriteBytes;
        DevList_[vsdev_list_idx].MaxSectorCount = SectorCount;
        vsdev_list_idx++;
        kfree(temp_str);
        debugpln("[AddStorageDevice]END");
        
        unlock(&DevList_->lock);
    }

    u32 GetSDEVTCount(VsDevType type){
        u32 c;
        for(u32 i = 0; i < vsdev_list_idx; i++){
            if(DevList_[i].type == type){
                return c++;
            }
        }
        return c;
    }

    void SetSDev(u32 idx){ThisDev = idx;}

    VsDevInfo GetSDEV(u32 idx){
        if(DevList_[idx].type != VsDevType::NSDEV || 
            DevList_[idx].type != VsDevType::Undefined)
            return DevList_[idx];
    }


    u8 Read(uint64_t lba, uint32_t SectorCount, void* Buffer){
        if(DevList_[ThisDev].type != VsDevType::Undefined)
            return DevList_[ThisDev].ops.Read(DevList_[ThisDev].classp,lba, SectorCount, Buffer);
        else
            return false;
    }

    u8 Write(uint64_t lba, uint32_t SectorCount, void* Buffer){
        if(DevList_[ThisDev].type != VsDevType::Undefined)
            return DevList_[ThisDev].ops.Write(DevList_[ThisDev].classp,lba, SectorCount, Buffer);
        else
            return false;
    }

    u8 ReadBytes(uint64_t address, uint32_t Count, void* Buffer){
        if(DevList_[ThisDev].type != VsDevType::Undefined){
            if(DevList_[ThisDev].ops.ReadBytes != nullptr)
                return DevList_[ThisDev].ops.ReadBytes(DevList_[ThisDev].classp,address, Count, Buffer);
            else {
                if (Count == 0)
                    return true;
                if (address + Count > DevList_[ThisDev].MaxSectorCount * 512)
                    return false;
                AddToStack();
                uint32_t tempSectorCount = ((((address + Count) + 511) / 512) - (address / 512));
                uint8_t* buffer2 = (uint8_t*)kmalloc(tempSectorCount * 512);//"Malloc for Read Buffer"
                _memset(buffer2, 0, tempSectorCount * 512);

                if (!DevList_[ThisDev].ops.Read(DevList_[ThisDev].classp,(address / 512), tempSectorCount, buffer2))
                {
                    uint16_t offset = address % 512;
                    for (uint64_t i = 0; i < Count; i++)
                        ((uint8_t*)Buffer)[i] = buffer2[i + offset];

                    kfree(buffer2);
                    RemoveFromStack();
                    return false;
                }

                uint16_t offset = address % 512;
                for (uint64_t i = 0; i < Count; i++)
                    ((uint8_t*)Buffer)[i] = buffer2[i + offset];
                        
                kfree(buffer2);
                RemoveFromStack();
                return true;
            }
        }else{
            return false;
        }
    }

    u8 WriteBytes(uint64_t address, uint32_t Count, void* Buffer){
        if(DevList_[ThisDev].type != VsDevType::Undefined){
            if(DevList_[ThisDev].ops.WriteBytes != nullptr)
                return DevList_[ThisDev].ops.WriteBytes(DevList_[ThisDev].classp,address, Count, Buffer);
            else{
                if (Count == 0)
                    return true;
                if (address + Count > DevList_[ThisDev].MaxSectorCount * 512)
                    return false;
                AddToStack();
                uint32_t tempSectorCount = ((((address + Count) + 511) / 512) - (address / 512));
                uint8_t* buffer2 = (uint8_t*)kmalloc(512); //Malloc for Write Buffer
                //window->Log("Writing Bytes...");
                
                if (tempSectorCount == 1)
                {
                    //window->Log("Writing Bytes (1/1)");
                    //uint8_t* buffer2 = (uint8_t*)malloc(512, "Malloc for Read Buffer (1/1)");
                    _memset(buffer2, 0, 512);
                    if (!DevList_[ThisDev].ops.Read(DevList_[ThisDev].classp,(address / 512), 1, buffer2))
                    {
                        kfree(buffer2);
                        RemoveFromStack();
                        return false;
                    }

                    uint16_t offset = address % 512;
                    for (uint64_t i = 0; i < Count; i++)
                        buffer2[i + offset] = ((uint8_t*)Buffer)[i];

                    if (!DevList_[ThisDev].ops.Write(DevList_[ThisDev].classp,(address / 512), 1, buffer2))
                    {
                        kfree(buffer2);
                        RemoveFromStack();
                        return false;
                    }
                    
                    //free(buffer2);
                }
                else
                {
                    //window->Log("Writing Bytes (1/3)");
                    uint64_t newAddr = address;
                    uint64_t newCount = Count;
                    uint64_t addrOffset = 0;
                    {
                        // GlobalRenderer->Clear(Colors.dgray);
                        // while (true);


                        //uint8_t* buffer2 = (uint8_t*)malloc(512, "Malloc for Read Buffer (1/2)");
                        _memset(buffer2, 0, 512);
                        //window->Log("Writing to Sector: {}", to_string((address / 512)), Colors.yellow);
                        if (!DevList_[ThisDev].ops.Read(DevList_[ThisDev].classp,(address / 512), 1, buffer2))
                        {
                            kfree(buffer2);
                            RemoveFromStack();
                            return false;
                        }

                        uint16_t offset = address % 512;
                        uint16_t specialCount = 512 - offset;
                        addrOffset = specialCount;
                        newAddr = address + specialCount;
                        newCount = Count - specialCount;

                        for (uint64_t i = 0; i < specialCount; i++)
                            buffer2[i + offset] = ((uint8_t*)Buffer)[i];

                        if (!DevList_[ThisDev].ops.Write(DevList_[ThisDev].classp,(address / 512), 1, buffer2))
                        {
                            kfree(buffer2);
                            RemoveFromStack();
                            return false;
                        }
                    }
                    {
                        _memset(buffer2, 0, 512);
                        if (!DevList_[ThisDev].ops.Read(DevList_[ThisDev].classp,((address + Count) / 512), 1, buffer2))
                        {
                            kfree(buffer2);
                            RemoveFromStack();
                            return false;
                        }

                        uint16_t specialCount = ((address + Count) % 512);
                        newCount -= specialCount;

                        uint64_t blehus = (Count - specialCount);

                        for (int64_t i = 0; i < specialCount; i++)
                            buffer2[i] = ((uint8_t*)Buffer)[i + blehus];

                        if (!DevList_[ThisDev].ops.Write(DevList_[ThisDev].classp,((address + Count) / 512), 1, buffer2))
                        {
                            kfree(buffer2);
                            RemoveFromStack();
                            return false;
                        }
                    }
                    {
                        uint64_t newSectorCount = newCount / 512;
                        if (newSectorCount != 0)
                        {
                            uint64_t newSectorStartId = newAddr / 512;

                            if (!DevList_[ThisDev].ops.Write(DevList_[ThisDev].classp,newSectorStartId, newSectorCount, (void*)((uint64_t)Buffer + addrOffset)))
                            {
                                kfree(buffer2);
                                RemoveFromStack();
                                return false;
                            }
                        }
                    }


                    
                }
                kfree(buffer2);
                RemoveFromStack();
                return true;
            }
        }else{
            return false;
        }
    }

    
}

#pragma GCC pop_options