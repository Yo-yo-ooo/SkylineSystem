#include "vfs.h"
#include "../../../../memory/heap.h"

void Memset(void* start, uint8_t value, uint64_t num)
{
    for (uint64_t i = 0; i < num; i++)
        *(uint8_t*)((uint64_t)start + i) = value;
}

void vfs_init(){
    info->fslistc = 0;
    Memset(info->fsnl,0,sizeof(info->fsnl));
    Memset(info->ops,0,sizeof(info->ops));
}

void vfs_signfilesystem(struct VitualFileSystemOPS* opss,const char* fsname){
    info->fslistc + 1;
    info->fsnl[(const int)info->fslistc] = fsname;
    info->ops[(const int)info->fslistc] = opss;
}
