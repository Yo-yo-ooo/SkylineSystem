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
#include <klib/algorithm/hmap.h>

enum FSType : uint32_t{
    FS_UNKOWN = 0,
    FS_EXT4 = 1,
    FS_FATFS = 2,
};



extern "C" char* GetMountPointName(const char* path);


typedef struct fsops{
    int32_t (*write)(void* filedesc, const void *buf, size_t count);
    int32_t (*read)(void* filedesc, void *buf, size_t count);
    int32_t (*lseek)(void* filedesc, uint64_t offset, uint32_t whence);
    int32_t (*close)(void* filedesc);
    int32_t (*open)(void* filedesc, const char* path, int32_t flags);
    uint64_t (*fsize)(void *filedesc);

    uint64_t SIZEOF_FILE_DESC;
}FS_PDESC; //File System public mamage desc


struct alignas(16) __hmap_s_mp{
    char *MPName;
    void* MP;
    FSType FST;
    FS_PDESC *FSOPS;
};


typedef struct fd{
    void *filedesc;
}fd_t;


extern "C" int32_t __hmap_s_mp_compare(const void* a,const void* b,void *udata);
extern "C" uint64_t __hmap_s_mp_hash(const void* item, uint64_t seed0, uint64_t seed1);
extern "C" struct hashmap* HMapS_MP;
__hmap_s_mp *GetMount(const char *path);

void InitFFMAN();


#endif