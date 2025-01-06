#include "vsdev.h"

VsDev::VsDev(){vsdev_list_idx = 0;}

VsDev::~VsDev(){}

void VsDev::AddStorageDevice(VsDevType type, SALOPS* ops)
{
    DevList[vsdev_list_idx].type = type;
    DevList[vsdev_list_idx].ops = ops;
    vsdev_list_idx++;
}

