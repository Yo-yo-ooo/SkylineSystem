#include "../../kernelStuff/diskStuff/Disk_Interfaces/generic/genericDiskInterface.h"
#include "../../fsStuff/fsStuff.h"
#include "../../osData/osData.h"
#include "../Fat/fat.h"

class VFS
{
private:
    /* data */
    int FSTYPE; 
    /*
    1: Marafs
    2: Fat32
    3: fat12
    */
   int hd;
public:
    VFS(int _hd){ 
        hd = _hd;
    }
    bool OpenFile(uint8_t *filename,uint8_t mode);
    bool CreateDir(uint8_t *dirn);
};

