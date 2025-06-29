#include <fs/fatfs/ff.h>
#include <fs/vfs.h>

#include <klib/klib.h>
#include <mem/heap.h>

FATFS* fs[10];
uint32_t fatfs_idx = 0;


vfs_tnode_t *vfs_fatfs_open(vfs_inode_t * this_inode, const char *path){

    uint8_t PartitionID = path[strlen(path) - 1] - '0';
    uint32_t DriverID = path[strlen(path) - 3] - '0';

    

}

int64_t vfs_fatfs_read(vfs_inode_t * this_inode, uint64_t offset, uint64_t len,void *buff){
    
    
}
