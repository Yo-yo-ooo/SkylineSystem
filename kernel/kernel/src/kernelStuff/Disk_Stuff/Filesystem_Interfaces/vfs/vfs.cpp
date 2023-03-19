#include "vfs.h"

void vfs_init(struct vfs_info* infoo){
    infoo->fslistc = 0;
}

void vfs_signfilesystem(struct vfs_node* node,const char* fsname){
    info->fslistc += 1;
    info->fsnl[(const int)info->fslistc] = fsname;
    info->fsnode[(const int)info->fslistc] = node;
}
