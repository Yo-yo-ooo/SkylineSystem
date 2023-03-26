#pragma once
#include <stddef.h>

struct VitualFileSystemOPS{
    int (*open)(const char * pathname, int flags);
    int (*read)(int fd, void * buf, size_t count);
    int (*write)(int fd, const void * buf, size_t count);
    int (*create)(const char * pathname, int mode);
    int (*mkdir)(const char *pathname);
    int (*rmdir)(const char *pathname);
    int (*rename)(const char *old_filename, const char *new_filename);
    int (*mount)(char* dev_name, char* mount_point, char* fs_name);
    int (*unmount)(char* mount_point);
    int (*lseek)(int fd, int offset, int whence);
    int (*close)(int fd);
    int (*sync)(void);
    int (*chdir)(const char *path);
    int (*fchdir)(int fd);
};

#define VFS_REG(Pos,function) .Pos = function 

#define VFS_MAX 100

struct vfs_info{
    int fslistc;
    const char* fsnl[VFS_MAX];
    struct VitualFileSystemOPS* ops[VFS_MAX];
};

static struct vfs_info* info;

void vfs_init(struct vfs_info* infoo);
void vfs_signfilesystem(struct VitualFileSystemOPS* opss,const char* fsname);