#pragma once

#include <klib/klib.h>
#include <klib/time.h>

//From HanOS

struct vfs_inode_t;
struct vfs_tnode_t;

typedef enum {
    VFS_NODE_FILE,
    VFS_NODE_SYMLINK,
    VFS_NODE_FOLDER,
    VFS_NODE_BLOCK_DEVICE,
    VFS_NODE_CHAR_DEVICE,
    VFS_NODE_MOUNTPOINT,
    VFS_NODE_INVALID
} vfs_node_type_t;

typedef enum {
    VFS_MODE_READ,
    VFS_MODE_WRITE,
    VFS_MODE_READWRITE
} vfs_openmode_t;

typedef struct {
    int64_t tv_sec;
    int64_t tv_nsec;
} vfs_timespec_t;

typedef struct {
    int64_t st_dev;               /* ID of device containing file */
    uint64_t st_ino;               /* Inode number */
    int32_t st_mode;             /* File type and mode */
    int32_t st_nlink;           /* Number of hard links */
    int32_t st_uid;               /* User ID of owner */
    int32_t st_gid;               /* Group ID of owner */
    int64_t st_rdev;              /* Device ID (if special file) */
    int64_t st_size;              /* Total size, in bytes */
    vfs_timespec_t st_atim;     /* Time of last access */
    vfs_timespec_t st_mtim;     /* Time of last modification */
    vfs_timespec_t st_ctim;     /* Time of last status change */
    int64_t st_blksize;       /* Block size for filesystem I/O */
    int64_t st_blocks;         /* Number of 512B blocks allocated */
} vfs_stat_t;

typedef struct {
    vfs_node_type_t type;
    tm_t tm;
    char name[VFS_MAX_NAME_LEN];
    uint64_t size;
} vfs_dirent_t;

/* Details about FS format */
typedef struct vfs_fsinfo_t {
    char name[16];              /* File system name */
    bool istemp;                /* For ramfs, it is true; for fat32 etc., it is false */
     //vec_struct(void *) filelist;
    struct {                                                          
        uint64_t len;                                                 
        uint64_t capacity;                                            
        void**    data;                                                
    }filelist;

    vfs_inode_t *(*mount)(vfs_inode_t * device);
    vfs_tnode_t *(*open)(vfs_inode_t * this_inode, const char *path);
    int64_t(*mknode) (vfs_tnode_t * this_inode);
    int64_t(*rmnode) (vfs_tnode_t * this_inode);
    int64_t(*read) (vfs_inode_t * this_inode, uint64_t offset, uint64_t len,
                    void *buff);
    int64_t(*write) (vfs_inode_t * this_inode, uint64_t offset, uint64_t len,
                    const void *buff);
    int64_t(*sync) (vfs_inode_t * this_inode);
    int64_t(*refresh) (vfs_inode_t * this_inode);
    int64_t(*getdent) (vfs_inode_t * this_inode, uint64_t pos,
                    vfs_dirent_t * dirent);
    int64_t(*ioctl) (vfs_inode_t * this_inode, int64_t request, int64_t arg);
} vfs_fsinfo_t;

struct vfs_tnode_t {
    char path[VFS_MAX_NAME_LEN];
    vfs_stat_t st;
    vfs_inode_t *inode;
    vfs_inode_t *parent;
};

struct vfs_inode_t {
    vfs_node_type_t type;       /* File type */
    char link[VFS_MAX_NAME_LEN];        /* Target file if file is symlink */
    uint64_t size;              /* File size */
    uint32_t perms;             /* File permission, modified by chmod */
    uint32_t uid;               /* User id */
    uint32_t refcount;          /* Reference count, used by symlink */
    uint32_t readcount;
    uint32_t writecount;
    tm_t tm;
    vfs_fsinfo_t *fs;
    void *ident;
    spinlock_t lock;
    vfs_tnode_t *mountpoint;
    struct {                                                          
        uint64_t len;                                                 
        uint64_t capacity;                                            
        vfs_tnode_t**    data;                                                
    }child;
};

typedef struct {
    char path[VFS_MAX_PATH_LEN];
    vfs_tnode_t *tnode;
    vfs_inode_t *inode;
    vfs_openmode_t mode;
    uint64_t seek_pos;
    vfs_tnode_t *curr_dir_ent;
    uint64_t curr_dir_idx;
} vfs_node_desc_t;

typedef vfs_tnode_t vfs_node;

namespace VFS{
    vfs_inode_t *AllocInode(vfs_node_type_t type, uint32_t perms,
                             uint32_t uid, vfs_fsinfo_t * fs,
                             vfs_tnode_t * mountpoint);
    
    void FreeNodes(vfs_tnode_t * tnode);
}