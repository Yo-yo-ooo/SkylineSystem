#pragma once
/*
COMMON: 0:FALSE
        1:TRUE
*/
//ABOUT ARCH
#ifdef __x86_64__

#define PCI_LIST_MAX 128


#if NOT_COMPILE_X86MEM == 1

#define CONFIG_FAST_MEMCPY 0
#define CONFIG_FAST_MEMSET 0
#define CONFIG_FAST_MEMCMP 0
#define CONFIG_FAST_MEMMOVE 0

#else

#define CONFIG_FAST_MEMCPY 1
#define CONFIG_FAST_MEMSET 1
#define CONFIG_FAST_MEMCMP 1
#define CONFIG_FAST_MEMMOVE 1

#endif

#endif

#ifdef __aarch64__
#define CONFIG_DOSENT_SUPPORT_SYS_REG_ID_AA64SMFR0_EL1 0
#define CONFIG_DOSENT_SUPPORT_SYS_REG_ID_AA64ZFR0_EL1 0

#define CONFIG_PAGESIZE_4K  0
#define CONFIG_PAGESIZE_16K 1
#define CONFIG_PAGESIZE_64K 2

#define CONFIG_PAGESIZE CONFIG_PAGESIZE_4K

#endif

/*-------------------------------------------------*/

//That means Just Have One File System Partition
//and Partition start address is 0x0(Not have MBR or GPT)
#define USE_VIRT_IMAGE

#ifdef USE_VIRT_IMAGE
#define ENABLE_VIRT_IMAGE true
#else
#define ENABLE_VIRT_IMAGE false
#endif

//COMMON CONFIG
#define PRINT_INFORMATION_ 1

#define MAX_VSDEV_COUNT 256
#define DEV_LIST_NODE_OF_VDL_LIST_COUNT 32 //Dev::DeviceListNL_t->VDL Alloc Count
#define DEV_LIST_NODE_COUNT 256 //Dev::DeviceList_t->Node Alloc Count

#define VFS_MAX_PATH_LEN    4096
#define VFS_MAX_NAME_LEN    36
#define VFS_BLOCK_SIZE      512

//#define _SYS_DEBUG_OUT

#define CONFIG_USE_DESKTOP_SUBSYS
#define MAX_CPU 128


#define BACKGROUND_COLOR_AUTHOR_LIKE 0x292D3E
#define BACKGROUND_COLOR_DEFAULT 0x00000000