#include <drivers/vsdev/vsdev.h>

namespace VsDev{
    VsDevList DevList[MAX_VSDEV_COUNT];
    uint32_t vsdev_list_idx = 0;

    

    u32 ThisDev = 0;
    void Init(){
        vsdev_list_idx = 0;
        ThisDev = 0;
    }

    void AddStorageDevice(VsDevType type, SALOPS* ops)
    {
        DevList[vsdev_list_idx].type = type;
        DevList[vsdev_list_idx].ops = ops;
        vsdev_list_idx++;
    }

    u32 GetSDEVTCount(VsDevType type){
        u32 c;
        for(u32 i = 0; i < vsdev_list_idx; i++){
            if(DevList[i].type == type){
                return c++;
            }
        }
        return c;
    }

    void SetSDev(u32 idx){ThisDev = idx;}

    VsDevInfo GetSDEV(u32 idx){
        VsDevInfo info;
        info.ops = DevList[idx].ops;
        info.type = DevList[idx].type;
        info.MaxSectorCount = DevList[idx].MaxSectorCount;
        info.SectorSize = DevList[idx].SectorSize;
        return info;
    }

    u8 Read(uint64_t lba, uint32_t SectorCount, void* Buffer){
        return DevList[ThisDev].ops->Read(lba, SectorCount, Buffer);
    }

    u8 Write(uint64_t lba, uint32_t SectorCount, void* Buffer){
        return DevList[ThisDev].ops->Write(lba, SectorCount, Buffer);
    }

    u8 ReadBytes(uint64_t address, uint32_t Count, void* Buffer){
        if(DevList[ThisDev].ops->ReadBytes != NULL)
            return DevList[ThisDev].ops->ReadBytes(address, Count, Buffer);
        else {
            if (Count == 0)
                return true;
            if (address + Count > DevList[ThisDev].MaxSectorCount * 512)
                return false;
            AddToStack();
            uint32_t tempSectorCount = ((((address + Count) + 511) / 512) - (address / 512));
            uint8_t* buffer2 = (uint8_t*)kmalloc(tempSectorCount * 512);//"Malloc for Read Buffer"
            _memset(buffer2, 0, tempSectorCount * 512);

            if (!Read((address / 512), tempSectorCount, buffer2))
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
    }

    u8 WriteBytes(uint64_t address, uint32_t Count, void* Buffer){
        if(DevList[ThisDev].ops->WriteBytes != NULL)
            return DevList[ThisDev].ops->WriteBytes(address, Count, Buffer);
        else{
            if (Count == 0)
                return true;
            if (address + Count > DevList[ThisDev].MaxSectorCount * 512)
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
                if (!Read((address / 512), 1, buffer2))
                {
                    kfree(buffer2);
                    RemoveFromStack();
                    return false;
                }

                uint16_t offset = address % 512;
                for (uint64_t i = 0; i < Count; i++)
                    buffer2[i + offset] = ((uint8_t*)Buffer)[i];

                if (!Write((address / 512), 1, buffer2))
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
                    if (!Read((address / 512), 1, buffer2))
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

                    if (!Write((address / 512), 1, buffer2))
                    {
                        kfree(buffer2);
                        RemoveFromStack();
                        return false;
                    }
                }
                {
                    _memset(buffer2, 0, 512);
                    if (!Read(((address + Count) / 512), 1, buffer2))
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

                    if (!Write(((address + Count) / 512), 1, buffer2))
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

                        if (!Write(newSectorStartId, newSectorCount, (void*)((uint64_t)Buffer + addrOffset)))
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
    }
}