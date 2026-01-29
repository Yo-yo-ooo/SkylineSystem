#pragma once
#ifndef _FD_H_
#define _FD_H_

#include <stdint.h>
#include <stddef.h>

#define O_PATH 010000000

#define O_ACCMODE (03 | O_PATH)
#define O_RDONLY   00
#define O_WRONLY   01
#define O_RDWR     02

#define O_CREAT         0100
#define O_EXCL          0200
#define O_NOCTTY        0400
#define O_TRUNC        01000
#define O_APPEND       02000
#define O_NONBLOCK     04000
#define O_DSYNC       010000
#define O_ASYNC       020000
#define O_DIRECT      040000
#define O_DIRECTORY  0200000
#define O_NOFOLLOW   0400000
#define O_CLOEXEC   02000000
#define O_SYNC      04010000
#define O_RSYNC     04010000
#define O_LARGEFILE  0100000
#define O_NOATIME   01000000
#define O_TMPFILE  020000000

#define O_EXEC O_PATH
#define O_SEARCH O_PATH
#include <fs/lwext4/ext4.h>
#include <drivers/dev/dev.h>

enum FSType : uint8_t{
    FS_EXT4 = 1,
    FS_FATFS = 2,
};

/*
Device type list:
0 - Reserved
1 - Framebuffer

*/

typedef struct FileDesc{
    size_t off;
    int32_t flags;
    char* path;
    size_t size;
    ext4_file f;
    bool IsSpecial; //Is/NOT special device (eg.Framebuffer)
    uint32_t Type; //file type
    void *RSVD;
    FSType FsType;
    VsDevType DevType;
    uint32_t dev_idx;
} fd_t;

struct FSOps{
    int32_t (*write)(fd_t* fsd, const void *buf, size_t count);
    int32_t (*read)(fd_t* fsd, void *buf, size_t count);
    int32_t (*lseek)(fd_t* fsd, size_t offset, int32_t whence);
    int32_t (*close)(fd_t* fsd);
};

extern volatile struct FSOps FileSystemOps[256];

/*
fd_t->Type
0:Generic File
1:DIR
2:socket
3:block dev
4:char dev
5:pipe
*/

#endif