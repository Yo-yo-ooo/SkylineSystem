#pragma once

#include <klib/klib.h>
#include <klib/time.h>

#include <klib/algorithm/hash_table.h>
#include <drivers/dev/dev.h>
#include <klib/mutex/mutex.h>

PACK(typedef struct VFSTimeDesc{
    uint16_t _unused Align : 6;
    uint16_t Second        : 6;
    uint16_t Minute        : 6;
    uint16_t Hour          : 5;
    uint16_t Day           : 5;
    uint16_t Month         : 4;
    uint32_t Year;
})VFSTimeDesc;

#define FS_FILE 0x1
#define FS_DIR  0x2

#define POLLIN   0x001
#define POLLPRI  0x002
#define POLLOUT  0x004
#define POLLERR  0x008
#define POLLHUP  0x010
#define POLLNVAL 0x020

typedef struct vnode_t {
    uint8_t Type;
    uint32_t Size;
    uint32_t Inode;
    char Name[256];
    int32_t RefCount;
    mutex_t *Mutex;
    int32_t Major;
    int32_t Minor;
    VFSTimeDesc *CreationTime;
    VFSTimeDesc *ChangeTime;
    VFSTimeDesc *LastAccessTime;
    struct vnode_t *Parent;
    struct vnode_t *Child;
    struct vnode_t *Next;
    size_t (*read)(struct vnode_t *Node, uint8_t *buffer, size_t off, size_t len);
    size_t (*write)(struct vnode_t *Node, uint8_t *buffer, size_t off, size_t len);
    struct vnode_t *(*lookup)(struct vnode_t *Node, const char *Name);
    int32_t (*ioctl)(struct vnode_t *Node, uint64_t Request, void *Arg);
    int32_t (*poll)(struct vnode_t *Node, int32_t events);
    void (*populate)(struct vnode_t *Node);
    void *mnt_info;
} vnode_t;

typedef vnode_t vfs_node_t;

namespace VFS{ 
    extern vnode_t *root_node;
    void Init();
    void AddNode(vnode_t *Parent, vnode_t *Child);
    vnode_t *Open(vnode_t *Root,const char* Path);
    size_t Read(vnode_t *Node, uint8_t *Buffer,size_t Offset,size_t Length);
    size_t Write(vnode_t *Node, uint8_t *Buffer,size_t Offset,size_t Length);
    void Close(vnode_t *Node);

    vnode_t *lookup(vnode_t *Node, const char *name);
    int32_t ioctl(vnode_t *Node, uint64_t Request, void *Arg);
    int32_t poll(vnode_t *Node, int32_t Events);
    void populate(vnode_t *Node);
}