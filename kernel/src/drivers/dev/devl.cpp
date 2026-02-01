#ifdef __x86_64__
#include <drivers/dev/dev.h>
#include <klib/algorithm/stl/map.h>
#include <klib/algorithm/stl/vector.h>

/* static void InitListNode(Dev::DeviceList_t *Ptr){
    Ptr->Node = (Dev::DeviceListNL_t*)kmalloc(sizeof(Dev::DeviceListNL_t) * DEV_LIST_NODE_COUNT);
    Ptr->RegistedDevIDX = 0;
    Ptr->Next = nullptr;
    for(size_t i = 0;i < DEV_LIST_NODE_COUNT;i++){
        Ptr->Node[i].list = (VDL*)kmalloc(sizeof(VDL) * DEV_LIST_NODE_OF_VDL_LIST_COUNT);
        Ptr->Node[i].CurrIDX = 0;
        Ptr->Node[i].Next = nullptr;
        Ptr->Node[i].Parent = (uint64_t)Ptr;
    }
} */

/* static EasySTL::map<VsDevType,uint32_t> T2Index;
static EasySTL::map<uint32_t,uint64_t> T2Ptr0; //Node IDX (LOOPCOUNT -> Pointer Base Adddress)
static EasySTL::map<uint32_t,uint64_t> T2Ptr1; //list IDX (LOOPCOUNT -> Pointer Base Address)
static EasySTL::map<VsDevType,DevOPS> Type2Device; */
static EasySTL::map<VsDevType,EasySTL::vector<VDL>> DeviceInfos;
static EasySTL::map<VsDevType,DevOPS> Type2DeviceOPS;
/* static uint32_t T2Ptr1_idx = 0;
//static Map<VsDevType,Map<uint32_t,VDL>> DeviceInfoMap;
static uint32_t TypeIndex = 0;
 */
namespace Dev{

    void AddDevice(VDL DeviceInfo,VsDevType DeviceType){
        __memcpy(&Type2DeviceOPS[DeviceType],&DeviceInfo.ops,sizeof(DevOPS));
        DeviceInfos[DeviceType].push_back(DeviceInfo);
    }

    VDL FindDevice(VsDevType DeviceType,uint32_t DeviceIndex){
        return DeviceInfos[DeviceType][DeviceIndex];
    }

    uint8_t DeviceRead(VsDevType DeviceType,uint32_t DevIDX,size_t offset,void* Buffer,size_t nbytes){
        DevOPS curr_operation = Type2DeviceOPS[DeviceType];
        if(curr_operation.ReadBytes != nullptr){
            return curr_operation.ReadBytes(
                FindDevice(DeviceType,DevIDX).classp,
                offset,
                nbytes,
                Buffer
            );
        }else if(curr_operation.ReadBytes_ != nullptr){
            return curr_operation.ReadBytes_(
                offset,
                nbytes,
                Buffer
            );
        }else if(curr_operation.Read == nullptr){
           if (nbytes == 0)
                return true;
            VDL dev = FindDevice(DeviceType,DevIDX);
            if (offset + nbytes > dev.MaxSectorCount * 512)
                return false;
            uint32_t tempSectorCount = ((((offset + nbytes) + 511) / 512) - (offset / 512));
            uint8_t* buffer2 = (uint8_t*)kmalloc(tempSectorCount * 512);
            _memset(buffer2, 0, tempSectorCount * 512);

            if (!curr_operation.Read_((offset / 512), tempSectorCount, buffer2))
            {
                uint16_t offset_ = offset % 512;
                for (uint64_t i = 0; i < nbytes; i++)
                    ((uint8_t*)Buffer)[i] = buffer2[i + offset_];

                kfree(buffer2);
                return false;
            }

            uint16_t offset_ = offset % 512;
            for (uint64_t i = 0; i < nbytes; i++)
                ((uint8_t*)Buffer)[i] = buffer2[i + offset_];
                    
            kfree(buffer2);
            return true;
        }else if(curr_operation.Read_ == nullptr){
            VDL dev = FindDevice(DeviceType,DevIDX);
            if (nbytes == 0)
                return true;
            if (offset + nbytes > dev.MaxSectorCount * 512)
                return false;
            uint32_t tempSectorCount = ((((offset + nbytes) + 511) / 512) - (offset / 512));
            uint8_t* buffer2 = (uint8_t*)kmalloc(tempSectorCount * 512);
            _memset(buffer2, 0, tempSectorCount * 512);

            if (!curr_operation.Read(dev.classp,(offset / 512), tempSectorCount, buffer2))
            {
                uint16_t offset_ = offset % 512;
                for (uint64_t i = 0; i < nbytes; i++)
                    ((uint8_t*)Buffer)[i] = buffer2[i + offset_];

                kfree(buffer2);
                return false;
            }

            uint16_t offset_ = offset % 512;
            for (uint64_t i = 0; i < nbytes; i++)
                ((uint8_t*)Buffer)[i] = buffer2[i + offset_];
                    
            kfree(buffer2);
            return true;
        }
        return false;
    }

    uint8_t DeviceWrite(VsDevType DeviceType,uint32_t DevIDX,size_t offset,void* Buffer,size_t nbytes){
        DevOPS curr_operation = Type2DeviceOPS[DeviceType];
        if(curr_operation.WriteBytes != nullptr){
            return curr_operation.WriteBytes(
                FindDevice(DeviceType,DevIDX).classp,
                offset,
                nbytes,
                Buffer
            );
        }else if(curr_operation.WriteBytes_ != nullptr){
            return curr_operation.WriteBytes_(
                offset,
                nbytes,
                Buffer
            );
        }else if(curr_operation.Read_ == nullptr){
            if (nbytes == 0)
                return true;
            VDL dev = FindDevice(DeviceType,DevIDX);
            if (offset + nbytes > dev.MaxSectorCount * 512)
                return false;
            uint32_t tempSectorCount = ((((offset + nbytes) + 511) / 512) - (offset / 512));
            uint8_t* buffer2 = (uint8_t*)kmalloc(512);

            if (tempSectorCount == 1)
            {
                _memset(buffer2, 0, 512);
                if (!curr_operation.Read(dev.classp,(offset / 512), 1, buffer2))
                {
                    kfree(buffer2);
                    
                    return false;
                }

                uint16_t offset_ = offset % 512;
                for (uint64_t i = 0; i < nbytes; i++)
                    buffer2[i + offset_] = ((uint8_t*)Buffer)[i];

                if (!curr_operation.Write(dev.classp,(offset / 512), 1, buffer2))
                {
                    kfree(buffer2);
                    
                    return false;
                }
            }
            else
            {
                uint64_t newAddr = offset;
                uint64_t newCount = nbytes;
                uint64_t addrOffset = 0;
                {
                    _memset(buffer2, 0, 512);
                    if (!curr_operation.Read(dev.classp,(offset / 512), 1, buffer2))
                    {
                        kfree(buffer2);
                        
                        return false;
                    }

                    uint16_t offset_ = offset % 512;
                    uint16_t specialCount = 512 - offset_;
                    addrOffset = specialCount;
                    newAddr = offset + specialCount;
                    newCount = nbytes - specialCount;

                    for (uint64_t i = 0; i < specialCount; i++)
                        buffer2[i + offset_] = ((uint8_t*)Buffer)[i];

                    if (!curr_operation.Write(dev.classp,(offset / 512), 1, buffer2))
                    {
                        kfree(buffer2);
                        
                        return false;
                    }
                }
                {
                    _memset(buffer2, 0, 512);
                    if (!curr_operation.Read(dev.classp,((offset + nbytes) / 512), 1, buffer2))
                    {
                        kfree(buffer2);
                        
                        return false;
                    }

                    uint16_t specialCount = ((offset + nbytes) % 512);
                    newCount -= specialCount;

                    uint64_t blehus = (nbytes - specialCount);

                    for (int64_t i = 0; i < specialCount; i++)
                        buffer2[i] = ((uint8_t*)Buffer)[i + blehus];


                    if (!curr_operation.Write(dev.classp,((offset + nbytes) / 512), 1, buffer2))
                    {
                        kfree(buffer2);
                        
                        return false;
                    }
                    
                }
                {
                    uint64_t newSectorCount = newCount / 512;
                    if (newSectorCount != 0)
                    {
                        uint64_t newSectorStartId = newAddr / 512;


                        if (!curr_operation.Write(dev.classp,newSectorStartId, newSectorCount, (void*)((uint64_t)Buffer + addrOffset)))
                        {
                            kfree(buffer2);
                            
                            return false;
                        }
                    }
                }


                
            }
            kfree(buffer2);
            return true;
        }else if(curr_operation.Read == nullptr){
            if (nbytes == 0)
                return true;
            VDL dev = FindDevice(DeviceType,DevIDX);
            if (offset + nbytes > dev.MaxSectorCount * 512)
                return false;
            uint32_t tempSectorCount = ((((offset + nbytes) + 511) / 512) - (offset / 512));
            uint8_t* buffer2 = (uint8_t*)kmalloc(512);

            if (tempSectorCount == 1)
            {
                _memset(buffer2, 0, 512);
                if (!curr_operation.Read_((offset / 512), 1, buffer2))
                {
                    kfree(buffer2);
                    
                    return false;
                }

                uint16_t offset_ = offset % 512;
                for (uint64_t i = 0; i < nbytes; i++)
                    buffer2[i + offset_] = ((uint8_t*)Buffer)[i];

                if (!curr_operation.Write_((offset / 512), 1, buffer2))
                {
                    kfree(buffer2);
                    
                    return false;
                }
            }
            else
            {
                uint64_t newAddr = offset;
                uint64_t newCount = nbytes;
                uint64_t addrOffset = 0;
                {
                    _memset(buffer2, 0, 512);
                    if (!curr_operation.Read_((offset / 512), 1, buffer2))
                    {
                        kfree(buffer2);
                        
                        return false;
                    }

                    uint16_t offset_ = offset % 512;
                    uint16_t specialCount = 512 - offset_;
                    addrOffset = specialCount;
                    newAddr = offset + specialCount;
                    newCount = nbytes - specialCount;

                    for (uint64_t i = 0; i < specialCount; i++)
                        buffer2[i + offset_] = ((uint8_t*)Buffer)[i];

                    if (!curr_operation.Write_((offset / 512), 1, buffer2))
                    {
                        kfree(buffer2);
                        
                        return false;
                    }
                    
                    //free(buffer2);
                }

                //window->Log("Writing Bytes (2/3)");
                {
                    //uint8_t* buffer2 = (uint8_t*)malloc(512, "Malloc for Read Buffer (1/2)");
                    _memset(buffer2, 0, 512);
                    //window->Log("Writing to Sector: {}", to_string((offset + nbytes - 1) / 512), Colors.yellow);
                    if (!curr_operation.Read_(((offset + nbytes) / 512), 1, buffer2))
                    {
                        kfree(buffer2);
                        
                        return false;
                    }

                    uint16_t specialCount = ((offset + nbytes) % 512);
                    newCount -= specialCount;

                    uint64_t blehus = (nbytes - specialCount);

                    for (int64_t i = 0; i < specialCount; i++)
                        buffer2[i] = ((uint8_t*)Buffer)[i + blehus];


                    if (!curr_operation.Write_(((offset + nbytes) / 512), 1, buffer2))
                    {
                        kfree(buffer2);
                        
                        return false;
                    }
                    
                }
                {
                    uint64_t newSectorCount = newCount / 512;
                    if (newSectorCount != 0)
                    {
                        uint64_t newSectorStartId = newAddr / 512;


                        if (!curr_operation.Write_(newSectorStartId, newSectorCount, (void*)((uint64_t)Buffer + addrOffset)))
                        {
                            kfree(buffer2);
                            
                            return false;
                        }
                    }
                }


                
            }
            kfree(buffer2);
            return true;
        }
    }

    /* EasySTL::pair<VDL,bool> FindDevFormList(VsDevType DeviceType,uint32_t DeviceIndex){
        uint32_t IndexOfType = T2Index[DeviceType];
        if(IndexOfType){
            if(IndexOfType > DEV_LIST_NODE_COUNT){
                uint32_t LoopCount = (IndexOfType/DEV_LIST_NODE_COUNT);
                DeviceList_t *Ptr = (DeviceList_t *)T2Ptr0[LoopCount - 1];
                uint32_t NodeCurrIDX = IndexOfType - LoopCount * DEV_LIST_NODE_COUNT;
                if(DeviceIndex > DEV_LIST_NODE_OF_VDL_LIST_COUNT){
                    LoopCount = (NodeCurrIDX / DEV_LIST_NODE_OF_VDL_LIST_COUNT);

                    DeviceListNL_t *NL = (DeviceListNL_t *)T2Ptr1[LoopCount - 1];
                    uint32_t VDL_CURR = DeviceIndex - LoopCount * DEV_LIST_NODE_OF_VDL_LIST_COUNT;
                    return EasySTL::make_pair<VDL,bool>(NL->list[VDL_CURR],true);
                }else{
                    return EasySTL::make_pair<VDL,bool>(Ptr->Node[NodeCurrIDX].list[DeviceIndex],true);
                }
            }else{
                return EasySTL::make_pair<VDL,bool>(DevList.Node[DeviceType].list[DeviceIndex],true);
            }
        }else{
            return EasySTL::make_pair<VDL,bool>({0},false);
        }
    }



    void ListAddDevice(
        VsDevType DeviceType,char* DeviceName,
        DevOPS DeviceOperations,void *ClassPtr = nullptr
    ){
        uint32_t IndexOfType = 0;
        if(T2Index[DeviceType])
            IndexOfType = T2Index[DeviceType];
        else
            IndexOfType = TypeIndex++,T2Index[DeviceType] = TypeIndex;
        VDL Vdevice;
        Vdevice.Name = DeviceName;
        Vdevice.classp = ClassPtr;
        Vdevice.ops.ioctl = DeviceOperations.ioctl;
        __memcpy(&Type2Device[DeviceType],&DeviceOperations,sizeof(DevOPS));
        if(IndexOfType > DEV_LIST_NODE_COUNT){
            DeviceList_t *Ptr = &DevList;
            uint32_t LoopCount = (IndexOfType/DEV_LIST_NODE_COUNT);
            for(uint32_t i = 0;i < LoopCount;i++){
                if(Ptr->Next == nullptr)
                    InitListNode(Ptr),T2Ptr0[i] = (uint64_t)Ptr->Node;
                Ptr = Ptr->Next;
            }
            uint32_t NodeCurrIDX = IndexOfType - LoopCount * DEV_LIST_NODE_COUNT;
            if(Ptr->Node[NodeCurrIDX].CurrIDX + 1 > DEV_LIST_NODE_OF_VDL_LIST_COUNT){
                Ptr->Node[NodeCurrIDX].Next = (DeviceListNL_t*)kmalloc(sizeof(DeviceListNL_t) * DEV_LIST_NODE_COUNT);
                Ptr->Node[NodeCurrIDX].Next->CurrIDX = 0;
                Ptr->RegistedDevIDX[NodeCurrIDX]++;
                T2Ptr1[T2Ptr1_idx] = (uint64_t)Ptr->Node[NodeCurrIDX].Next->list;
                T2Ptr1_idx++;
                __memcpy(&Ptr->Node[NodeCurrIDX].Next->list[0],&Vdevice,sizeof(VDL));
            }else{
                Ptr->RegistedDevIDX[NodeCurrIDX]++;
                Ptr->Node[NodeCurrIDX].CurrIDX++;
                __memcpy(&Ptr->Node[NodeCurrIDX].list[Ptr->Node[NodeCurrIDX].CurrIDX],&Vdevice,sizeof(VDL));
            }
        }else{
            DevList.RegistedDevIDX[IndexOfType]++;
            DevList.Node[IndexOfType].CurrIDX++;
            __memcpy(&DevList.Node[IndexOfType].list[DevList.Node[IndexOfType].CurrIDX],&Vdevice,sizeof(VDL));
        
        return;
    } */
}


#endif