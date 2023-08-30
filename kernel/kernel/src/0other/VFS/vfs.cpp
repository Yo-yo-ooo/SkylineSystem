#include "vfs.h"
#include "../Fat12/FAT12.h"

bool VFS::OpenFile(uint8_t *filename,uint8_t mode){
    if(FSTYPE == 1){
        return FS_STUFF::OpenFile(filename);
    }
    else if(FSTYPE == 2){
        ThisOS::filesystem::Fat32 fat(hd, 1);
        return (bool)fat.OpenFile(1,filename,mode);
    }else if(FSTYPE == 3){
        if(mode == FILEACCESSMODE_CREATE){
            return (bool)CreateFile(hd,filename,filename,"",REGULAR_TYPE,512);
        }
    }
}