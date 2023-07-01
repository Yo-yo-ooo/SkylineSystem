#pragma once

typedef void *fs_context;
typedef unsigned int dword;
typedef unsigned char byte;
typedef unsigned short word;

#define PRIVATE static
#define PUBLIC 

struct __partition_info {
    byte state;
    byte start_head;
    byte start_sector;
    byte start_cylinder;
    byte system_id;
    byte end_head;
    byte end_cylinder;
    byte end_sector;
    dword start_lba;
    dword sector_number;
} __attribute__((packed));

typedef struct __partition_info partition_info; 

//0号IDE通道
#define IDE0_BASE             (0x1F0)
#define IDE0_BASE2            (0x3F6)
#define IDE0_DATA             (IDE0_BASE + 0)
#define IDE0_ERROR            (IDE0_BASE + 1)
#define IDE0_FEATURES         (IDE0_BASE + 1)
#define IDE0_SECTOR_COUNTER   (IDE0_BASE + 2)
#define IDE0_LBA_LOW          (IDE0_BASE + 3)
#define IDE0_LBA_MID          (IDE0_BASE + 4)
#define IDE0_LBA_HIGH         (IDE0_BASE + 5)
#define IDE0_DEVICE           (IDE0_BASE + 6)
#define IDE0_STATUS           (IDE0_BASE + 7)
#define IDE0_COMMAND          (IDE0_BASE + 7)
#define IDE0_ALTERNATE_STATUS (IDE0_BASE2 + 0)
#define IDE0_DEVICE_CONTROL   (IDE0_BASE2 + 0)

//1号IDE通道
#define IDE1_BASE             (0x170)
#define IDE1_BASE2            (0x376)
#define IDE1_DATA             (IDE1_BASE + 0)
#define IDE1_ERROR            (IDE1_BASE + 1)
#define IDE1_FEATURES         (IDE1_BASE + 1)
#define IDE1_SECTOR_COUNTER   (IDE1_BASE + 2)
#define IDE1_LBA_LOW          (IDE1_BASE + 3)
#define IDE1_LBA_MID          (IDE1_BASE + 4)
#define IDE1_LBA_HIGH         (IDE1_BASE + 5)
#define IDE1_DEVICE           (IDE1_BASE + 6)
#define IDE1_STATUS           (IDE1_BASE + 7)
#define IDE1_COMMAND          (IDE1_BASE + 7)
#define IDE1_ALTERNATE_STATUS (IDE1_BASE2 + 0)
#define IDE1_DEVICE_CONTROL   (IDE1_BASE2 + 0)

struct fat32_info_struct {
    word _jmp_short; //跳转指令 +0x00
    byte _nop; //空指令 +0x02
    char oem_name[8]; //oem名 +0x03
    word bytes_per_sector; //每扇区字节数 +0x0B
    byte sectors_per_clus; //每簇扇区数 +0x0D
    word reversed_sectors; //保留扇区 +0x0E
    byte num_fats; //fat数 +0x10
    dword reserved0; // +0x11
    byte media; //介质 +0x15
    word reserved1; // +0x16
    word sectors_per_track; //每磁道扇区数 +0x18
    word heads; //磁头数 +0x1A
    dword hidden_sectors; //隐藏扇区数 +0x1C
    dword total_sectors; //总扇区数 +0x20
    dword fat_sectors; //fat扇区数 +0x24
    word reserved2; // +0x28
    word version; //版本号 +0x2A
    dword root_directory_start_clus; //根目录区起始簇号 +0x2C
    word fsinfo_sectors; //FS信息扇区数 +0x30
    word backup_boot_sectors; //引导备份扇区数 +0x32
    dword reserved3; // +0x34
    dword reserved4; // +0x38
    dword reserved5; // +0x3C
    byte drive_no; //驱动器号 +0x40
    byte reserved6; //驱动器号 +0x41
    byte boot_sig; //boot标志 +0x42
    dword volumn_id; //卷号 +0x43
    char volumn_label[11]; //卷标 +0x47
    char file_system[8]; //文件系统 +0x52
} __attribute__((packed));

typedef struct {
    // 
    dword magic; //0x93186A8E (MD5 of "File Allocation Table 32"的前32位)
    int selector; //Selector of Context
    partition_info partition;
    struct fat32_info_struct infomations;
    //Extension
    dword fat1_start; //fat1起始
    dword data_start; //数据区起始
    void *root_directory; //根目录数据
    void *fat_buffer; //FAT缓冲
    int buffer_start; //FAT缓冲开始扇区编号 (编号 + FAT1_START = 所在扇区)
} FAT32_CONTEXT;

#define FAT32_FILE_ENTRY_SIZE (0x20)
#define FAT32_FILE_ENTRIES_PER_SECTOR (0x200 / FAT32_FILE_ENTRY_SIZE)
#define FAT32_ENTRY_SIZE (4)
#define FAT32_ENTRIES_PER_SECTOR (0x200 / FAT32_ENTRY_SIZE)
#define FAT32_CONTEXT_MAGIC (0x93186A8E)

#define FAT_BUFFER_SECTORS (4)
#define INFO _context->infomations
/* endregion */