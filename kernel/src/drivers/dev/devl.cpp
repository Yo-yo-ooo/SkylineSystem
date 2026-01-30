#ifdef __x86_64__
#include <drivers/dev/dev.h>
#include <klib/algorithm/map.h>

static void InitListNode(Dev::DeviceList_t *Ptr){
    Ptr->Node = (Dev::DeviceListNL_t*)kmalloc(sizeof(Dev::DeviceListNL_t) * DEV_LIST_NODE_COUNT);
    Ptr->RegistedDevIDX = 0;
    Ptr->Next = nullptr;
    for(size_t i = 0;i < DEV_LIST_NODE_COUNT;i++){
        Ptr->Node[i].list = (VDL*)kmalloc(sizeof(VDL) * DEV_LIST_NODE_OF_VDL_LIST_COUNT);
        Ptr->Node[i].CurrIDX = 0;
        Ptr->Node[i].Next = nullptr;
        Ptr->Node[i].Parent = (uint64_t)Ptr;
    }
}

static Map<VsDevType,uint32_t> T2Index;
static uint32_t TypeIndex = 0;

namespace Dev{
    DeviceList_t DevList;
    void InitDevList(){
        InitListNode(&Dev::DevList);
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
        VDL Vdevice;// = DevList.Node[DeviceType].list[DevList.Node[DeviceType].CurrIDX + 1];
        if(IndexOfType > DEV_LIST_NODE_COUNT){
            DeviceList_t *Ptr = &DevList;
            uint32_t LoopCount = (IndexOfType/DEV_LIST_NODE_COUNT);
            for(uint32_t i = 0;i < LoopCount;i++){
                if(Ptr->Next == nullptr)
                    InitListNode(Ptr);
                Ptr = Ptr->Next;
            }
            uint32_t NodeCurrIDX = IndexOfType - LoopCount * DEV_LIST_NODE_COUNT;
            if(Ptr->Node[NodeCurrIDX].CurrIDX + 1 > DEV_LIST_NODE_OF_VDL_LIST_COUNT){
                Ptr->Node[NodeCurrIDX].Next = (DeviceListNL_t*)kmalloc(sizeof(DeviceListNL_t) * DEV_LIST_NODE_COUNT);
                Ptr->Node[NodeCurrIDX].Next->CurrIDX = 0;
                Ptr->RegistedDevIDX[NodeCurrIDX]++;
                Vdevice = Ptr->Node[NodeCurrIDX].Next->list[0];
                Ptr->Node[NodeCurrIDX].Next->CurrIDX = 1;
            }else{
                Ptr->RegistedDevIDX[NodeCurrIDX]++;
                Vdevice = Ptr->Node[NodeCurrIDX].list[Ptr->Node[NodeCurrIDX].CurrIDX];
                Ptr->Node[NodeCurrIDX].CurrIDX++;
            }
            //Vdevice = Ptr->list[Ptr->CurrIDX++];
        }else{
            DevList.RegistedDevIDX[IndexOfType]++;
            Vdevice = DevList.Node[IndexOfType].list[DevList.Node[IndexOfType].CurrIDX];  
            DevList.Node[IndexOfType].CurrIDX++;  
        }
        Vdevice.Name = DeviceName;
        Vdevice.classp = ClassPtr;
        Vdevice.ops.ioctl = DeviceOperations.ioctl;
        if(ClassPtr != nullptr){
            Vdevice.ops.GetMaxSectorCount = DeviceOperations.GetMaxSectorCount;
            Vdevice.ops.Read = DeviceOperations.Read;
            Vdevice.ops.Write = DeviceOperations.Write;
            Vdevice.ops.ReadBytes = DeviceOperations.ReadBytes;
            Vdevice.ops.WriteBytes = DeviceOperations.WriteBytes;
        }else{
            Vdevice.ops.GetMaxSectorCount_ = DeviceOperations.GetMaxSectorCount_;
            Vdevice.ops.Read_ = DeviceOperations.Read_;
            Vdevice.ops.Write_ = DeviceOperations.Write_;
            Vdevice.ops.ReadBytes_ = DeviceOperations.ReadBytes_;
            Vdevice.ops.WriteBytes_ = DeviceOperations.WriteBytes_;
        }
        return;
    }
}


#endif