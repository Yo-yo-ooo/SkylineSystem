#pragma once
#include <stdint.h>
#include <stddef.h>

typedef uint8_t u8_t;
typedef uint16_t u16_t;
typedef uint32_t u32_t;

typedef struct {
    uint16_t bytes_per_sector;
    uint16_t sectors_per_cluster;
    uint16_t reversed_sector;
    uint8_t fats;
    uint16_t root_entries;
    uint16_t total_sectors_16;
    uint8_t media;
    uint16_t sectors_per_fat_16;
    uint16_t sectors_per_track;
    uint16_t heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
    uint32_t sectors_per_fat_32;
    uint16_t extended_flags;
    uint16_t fs_version;
    uint32_t root_dir_first_cluster;
    uint16_t fs_info_sector;
    uint16_t backup_boot_sector;
    uint8_t reserved_0[12];
    uint8_t drive_num;
    uint8_t ext_sig;
    uint32_t volume_id;
    uint8_t volume_label[11];
    uint8_t fs_type_label[8];
} BiosParameterBlock;