#pragma once

#include <stdint.h>

typedef struct VSTORAGE_DEVICE {
    uint16_t base;
    uint8_t type;
    char name[40];
} VSTORAGE_DEVICE;

typedef struct VSTORAGE_DEVICE_FUNC {
    uint8_t (*Init)();
    uint8_t (*Read)(uint32_t lba, uint8_t* buffer, uint32_t sector_count);
    uint8_t (*Write)(uint32_t lba, uint8_t* buffer, uint32_t sector_count);
} VSTORAGE_DEVICE_FUNC;

#define VSTORAGE_DEVICE_ERROR_INIT   0
#define VSTORAGE_DEVICE_ERROR_READ   1
#define VSTORAGE_DEVICE_ERROR_WRITE  2
