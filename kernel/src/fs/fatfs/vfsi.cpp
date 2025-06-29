#include <fs/fatfs/ff.h>
#include <fs/vfs.h>

FATFS* fs[10];

typedef struct fatfs_ident{
    uint32_t idx;
}FatFs_Ident;

vfs_tnode_t *vfs_fatfs_open(vfs_inode_t * this_inode, const char *path){
    FatFs_Ident* ident = (FatFs_Ident*)this_inode->ident;
}

int64_t vfs_fatfs_read(vfs_inode_t * this_inode, uint64_t offset, uint64_t len,void *buff){
    
    
}
