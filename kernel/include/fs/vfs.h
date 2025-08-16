#pragma once

#include <klib/klib.h>
#include <klib/time.h>

/*This is a simple version of VFS
Thanks to Linux,HanOS...*/
namespace VFS{ 
    typedef enum NodeType{
        VFS_NODE_FILE,
        VFS_NODE_SYMLINK,
        VFS_NODE_FOLDER,
        VFS_NODE_BLOCK_DEVICE,
        VFS_NODE_CHAR_DEVICE,
        VFS_NODE_MOUNTPOINT,
        VFS_NODE_INVALID
    } NodeType;

    typedef enum OpenMode{
        VFS_MODE_READ,
        VFS_MODE_WRITE,
        VFS_MODE_READWRITE
    } OpenMode;
#pragma GCC diagnostic ignore "-Wno-packed-bitfield-compat"
    PACK(typedef struct VFSTimeSpec{
        uint16_t _unused Align : 6;
        uint16_t Second        : 6;
        uint16_t Minute        : 6;
        uint16_t Hour          : 5;
        uint16_t Day           : 5;
        uint16_t Month         : 4;
        uint32_t Year;
    })VFSTimeSpec;
#pragma GCC diagnostic pop

    struct FileSystemType {
        char *name;
        uint32_t TypeID;
    };

    typedef struct VFSInodeOperations{
        /*IDK How to IMPL them*/
    }VFSInodeOperations;

    typedef struct VFSFileOperations{
        
    }VFSFileOperations;

    typedef struct DirEntry{
        uint8_t Name[VFS_MAX_NAME_LEN];
        //Inode *InodePointer;

    }DirEntry;

    typedef struct Inode{
        uint64_t Dev;               /* ID of device containing file */
        uint64_t InodeNumber;               /* Inode number */
        uint32_t Mode;                  /* File type and mode */
        uint32_t LinkNumber;           /* Number of hard links */
        uint32_t UserID;               /* User ID of owner */
        uint32_t GroupID;               /* Group ID of owner */
        uint64_t DevID;              /* Device ID (if special file) */
        uint64_t Size;              /* Total size, in bytes */
        uint64_t BlockSize;       /* Block size for filesystem I/O */
        uint64_t BlockNumber;         /* Number of 512B blocks allocated */
        uint32_t ProcessAccessCount;
        spinlock_t FileLock;

        VFSTimeSpec AccessTime;     /* Time of last access */
        VFSTimeSpec ModificationTime;     /* Time of last modification */
        VFSTimeSpec STATChangeTime;     /* Time of last status change */
        VFSInodeOperations *InodeOperation;
        VFSFileOperations *FileOperation;

    }Inode;
}