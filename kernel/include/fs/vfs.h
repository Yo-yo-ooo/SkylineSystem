#pragma once

#include <klib/klib.h>
#include <klib/time.h>

#include <klib/algorithm/hash_table.h>
#include <drivers/dev/dev.h>

typedef struct VFSDirEntry VFSDirEntry;
typedef struct VFSInode VFSInode;
typedef struct VFSFile VFSFile;

typedef struct VFSTimeSpec{
    int64_t tv_sec;
    int64_t tv_nsec;
} VFSTimeSpec;

PACK(typedef struct VFSTimeDesc{
    uint16_t _unused Align : 6;
    uint16_t Second        : 6;
    uint16_t Minute        : 6;
    uint16_t Hour          : 5;
    uint16_t Day           : 5;
    uint16_t Month         : 4;
    uint32_t Year;
})VFSTimeDesc;

struct VFSFileSystemType {
    char *Name;
    uint32_t FSID;
    VFSFileSystemType *Next;
};

typedef struct VFSSuperBlock{
    VFSFileSystemType *FSType;
    spinlock_t SBLock;
    uint32_t DevIDX;

    list *DelInode;
    list *DelDirEntry;
}VFSSuperBlock;



typedef struct VFSMount{
    VFSDirEntry *MountPoint;
    char *DevName;
    char *Where;

    VFSMount *Parent;
    VFSMount *Child;
}VFSMount;


typedef struct VFSFileOperations{
    ssize_t (*Open) (VFSFile *file, int32_t flags  , uint32_t mode);
    ssize_t (*Read) (VFSFile *file, void* buf      , size_t   count);
    ssize_t (*Write)(VFSFile *file, const void* buf, size_t   count);
    ssize_t (*Close)(VFSFile *file);
};

typedef struct VFSFile{
    spinlock_t FileLock;
    VFSInode *FileInode;
    VFSFileOperations *FileOP;

    uint32_t FileMode;
    loff_t   FilePosition;
    uint32_t FileFlags;
    
};

typedef struct VFSInode{
    VFSTimeDesc Time;
    uint32_t UserID;
    uint32_t ReferenceCount;          
    uint32_t ReadCount;
    uint32_t WriteCount;

    char *Link;
    uint32_t LengthOfLink;
    VFSMount *Mount;

    hash_table HashTbl;
}VFSInode;

typedef struct VFSDirEntry{
    char *Name; 

    hash_table HashTbl;
}VFSDirEntry;

/*This is a simple version of VFS
Thanks to Linux,HanOS...*/
namespace VFS{ 
    

}