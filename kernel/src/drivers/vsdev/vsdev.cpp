#include "vsdev.h"

VsDev::VsDev(){vsdev_list_idx = 0;}

VsDev::~VsDev(){}

void VsDev::AddStorageDevice(VsDevType type, SALOPS* ops)
{
    DevList[vsdev_list_idx].type = type;
    DevList[vsdev_list_idx].ops = ops;
    vsdev_list_idx++;
}

u32 VsDev::GetSDEVTCount(VsDevType type){
    u32 c;
    for(u32 i = 0; i < vsdev_list_idx; i++){
        if(DevList[i].type == type){
            return c++;
        }
    }
    return c;
}