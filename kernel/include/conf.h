#pragma once

#define PRINT_INFORMATION_ 1

#define PCI_LIST_MAX 128

#define MAX_VSDEV_COUNT 256

#define VFS_MAX_PATH_LEN    4096
#define VFS_MAX_NAME_LEN    256
#define VFS_BLOCK_SIZE      512

//#define _SYS_DEBUG_OUT

//That means Just Have One File System Partition
//and Partition start address is 0x0(Not have MBR or GPT)
#define USE_VIRT_IMAGE

#ifdef USE_VIRT_IMAGE
#define ENABLE_VIRT_IMAGE true
#else
#define ENABLE_VIRT_IMAGE false
#endif