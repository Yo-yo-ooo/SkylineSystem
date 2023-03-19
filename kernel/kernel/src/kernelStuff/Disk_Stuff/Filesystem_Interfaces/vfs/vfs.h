#pragma once
#include <stddef.h>

struct VitualFileSystemOPS{
    int (*open)(const char * pathname, int flags);
    int (*read)(int fd, void * buf, size_t count);
    int (*write)(int fd, const void * buf, size_t count);
    int (*create)(const char * pathname, int mode);
    int (*mkdir)(const char *pathname, int mode);
    int (*rmdir)(const char *pathname);
    int (*rename)(const char *old_filename, const char *new_filename);
    int (*mount)(char* dev_name, char* mount_point, char* fs_name);
    int (*unmount)(char* mount_point);
};

struct vfs_node {
    struct VitualFileSystemOPS* ops;
    void* private_data;
};

struct vfs_info{
    int fslistc;
    char* fsnl[100];
    struct vfs_node* fsnode[100];
};

static struct vfs_info* info;

void vfs_init(struct vfs_info* infoo);
void vfs_signfilesystem(struct vfs_node* node,const char* fsname);