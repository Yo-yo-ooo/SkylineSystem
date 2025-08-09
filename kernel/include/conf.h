#pragma once

//ABOUT ARCH
#ifdef __x86_64__

#define PCI_LIST_MAX 128
#define MAX_CPU 128

#endif

/*-------------------------------------------------*/

//COMMON CONFIG
#define PRINT_INFORMATION_ 1

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

#define USE_TEST_x2Apic 0


#define BACKGROUND_COLOR_AUTHOR_LIKE 0x292D3E
#define BACKGROUND_COLOR_DEFAULT 0x00000000