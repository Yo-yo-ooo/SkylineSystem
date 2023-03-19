#include "vfs.h"

void vfs_init(struct vfs_info* infoo){
    infoo->fslistc = 0;
    infoo->fsnl = { 0 };
    infoo->fsnode = { 0 };
}

void vfs_signfilesystem(struct vfs_node* node,const char* fsname){
    info->fslistc += 1;
    info->fsnl[node->fslistc] = fsname;
    info->fsnode[info->fslistc] = node;
}
