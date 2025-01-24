#pragma once

#include "../klib/klib.h"

#define VFS_FILE 0x1
#define VFS_DIRECTORY 0x2
#define VFS_DEVICE 0x3
#define VFS_SOCKET 0x4

#define VFS_DESTROY 0x1 // Destroy perm

typedef struct {
    char* name;
    u32 ino;
} vfs_dirent;

// TODO: Ref count.

typedef struct vfs_node {
    char* name;
    struct vfs_node* parent;
    atomic_lock lock;
    bool open;
    u32 perms;
    u32 type;
    u32 size;
    u32 ino;
    u64 offset;
    u64 tid; // TID of the task that opened this node.
    void* obj; // Dynamic object, could be anything, example, a socket!
    i32(*read)(struct vfs_node* vnode, u8* buffer, u32 count);
    i32(*write)(struct vfs_node* vnode, u8* buffer, u32 count);
    vfs_dirent*(*readdir)(struct vfs_node* vnode, u32 index);
    struct vfs_node*(*finddir)(struct vfs_node* vnode, char* path);
    u64(*poll)(struct vfs_node* vnode);
} vfs_node;

typedef struct {
    vfs_node* node;
    u32 current_index;
} DIR; // For syscalls

typedef struct {
    vfs_node* vnode;
    u64 offset;
    u16 mode;
    u16 fd_num;
} file_descriptor;

extern vfs_node* vfs_root; // "/" path

void vfs_init();
i32 vfs_write(vfs_node* vnode, u8* buffer, u32 count);
i32 vfs_read(vfs_node* vnode, u8* buffer, u32 count);
vfs_dirent* vfs_readdir(vfs_node* vnode, u32 index);
vfs_node* vfs_finddir(vfs_node* vnode, char* path);
vfs_node* vfs_open(vfs_node* vnode, char* path); // traverse directories
u64 vfs_poll(vfs_node* vnode);
void vfs_destroy(vfs_node* vnode);
