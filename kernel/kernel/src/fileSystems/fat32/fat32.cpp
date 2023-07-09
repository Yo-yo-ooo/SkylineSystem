
#include "../../kernelStuff/IO/IO.h"
#include "../../OSDATA/osdata.h"
#include "../../kernelStuff/Disk_Stuff/Disk_Interfaces/sata/sataDiskInterface.h"
#include "../../kernelStuff/Disk_Stuff/Disk_Interfaces/generic/genericDiskInterface.h"
#include "fat32.h"
#include "slib.h"

void memset(void* start, uint8_t value, uint64_t num)
{
    for (uint64_t i = 0; i < num; i++)
        *(uint8_t*)((uint64_t)start + i) = value;
}

void read_sector(unsigned int lba, int num, int selector, void *dst) {
    DiskInterface::GenericDiskInterface gdi;
    // 计算起始扇区号
    
    // 调用SATARead函数来实现读取扇区的操作
    // 这里假设SATARead函数实现良好且返回一个bool值表示操作成功与否
    bool success = gdi.ReadBytes(lba,num,dst);
    
    // 如果读取操作成功，则不需要进行其他处理
    if (success == false) {
        LogError("Failed Fat32 Read Sector Command",osData.debugTerminalWindow);
    }
}

static dword get_fat32_entry(fs_context context, dword last) {
    FAT32_CONTEXT *_context = (FAT32_CONTEXT*)context;
    int num = (last / FAT32_ENTRIES_PER_SECTOR);
    int _sector = num + _context->fat1_start;
    int _offset = last % FAT32_ENTRIES_PER_SECTOR;

    if (num >= _context->buffer_start + FAT_BUFFER_SECTORS) { //不在缓冲区里
        read_sector(_sector, 4, _context->selector, _context->fat_buffer);
        _context->buffer_start = num; //设置开始编号
    }

    int num_off = num - _context->buffer_start;
    int offset = num_off * INFO.bytes_per_sector + //扇区间偏移量
                 _offset * sizeof(dword); //扇区内偏移量

    dword entry = *((dword *)(_context->fat_buffer + offset));
    return entry;
}

static void __load_file(fs_context context, dword clus, void *dst) {
    FAT32_CONTEXT *_context = (FAT32_CONTEXT*)context;
    //起始簇号为2
    int sectors_per_clus = INFO.sectors_per_clus;
    while (clus < 0x0FFFFFF0) {
        int start_sector = _context->data_start + (clus - 2) * sectors_per_clus;
        read_sector(start_sector, INFO.sectors_per_clus, _context->selector, dst);

        dst += INFO.sectors_per_clus * INFO.bytes_per_sector;

        clus = get_fat32_entry(context, clus);
    }
}

fs_context load_fat32_fs(int disk_selector, partition_info *info) {
    FAT32_CONTEXT *context = malloc(sizeof(FAT32_CONTEXT));

    void *superblock = malloc(512);
    memcpy(&context->partition, info, sizeof(partition_info));
    read_sector(context->partition.start_lba, 1, disk_selector, superblock);

    //MARK
    context->selector = disk_selector;
    context->magic = FAT32_CONTEXT_MAGIC;
    
    memcpy(&context->infomations, superblock, sizeof(context->infomations));

    //Extension
    context->fat1_start = context->infomations.reversed_sectors + context->partition.start_lba;
    context->data_start = context->fat1_start + context->infomations.num_fats * context->infomations.fat_sectors;
    
    context->root_directory = malloc(16 * context->infomations.bytes_per_sector); //16个扇区
    context->fat_buffer = malloc(FAT_BUFFER_SECTORS * context->infomations.bytes_per_sector); //4个扇区

    read_sector(context->fat1_start, 4, disk_selector, context->fat_buffer);
    context->buffer_start = 0;

    __load_file(context, context->infomations.root_directory_start_clus, context->root_directory);

    free(superblock);
    return context;
}

void terminate_fat32_fs(fs_context context) {
    free (((FAT32_CONTEXT*)context)->root_directory);
    free (((FAT32_CONTEXT*)context)->fat_buffer);
    free (context);
}

static dword get_file_start_clus(fs_context context, const char *name) {
    if (strlen(name) != 11) {
        return -1;
    }
    FAT32_CONTEXT *_context = (FAT32_CONTEXT*)context;
    char rdname[12] = "";
    rdname[11] = 0;
    for (int i = 0 ; i < 16 * FAT32_ENTRIES_PER_SECTOR ; i ++) {
        memcpy(rdname, _context->root_directory + i * FAT32_ENTRY_SIZE + 0, 11);
        if (! strcmp(name, rdname)) {
            dword low_word = *(word *)(_context->root_directory + i * FAT32_ENTRY_SIZE + 0x1A); //低字
            dword high_word = *(word *)(_context->root_directory + i * FAT32_ENTRY_SIZE + 0x14); //高字
            return (high_word << 16) + low_word;
        }
    }
    return -1;
}

static char *convert(const char *name, char *buffer) {
    char *backup = buffer;
    int cnt = 0;
    while (*name != '.') {
        *buffer = toupper(*name);
        buffer ++;
        name ++;
        cnt ++;
    }
    if (cnt > 8) {
        return NULL;
    }
    while (cnt < 8) {
        *buffer = ' ';
        buffer ++;
        cnt ++;
    }
    name ++;
    for (int i = 0 ; i < 3 ; i ++) {
        *buffer = toupper(*name);
        buffer ++;
        name ++;
    }
    *buffer = '\0';
    return backup;
}

bool load_fat32_file(fs_context context, const char *name, void *dst) {
    FAT32_CONTEXT *_context = (FAT32_CONTEXT*)context;
    char _name[12];
    if (convert(name, _name) == NULL) {
        return false;
    }
    dword clus = get_file_start_clus(context, _name);
    if (clus == -1) {
        return false;
    }
    __load_file(_context, clus, dst);
    return true;
}
