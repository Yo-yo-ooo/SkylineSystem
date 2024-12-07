#pragma once

#include <stdint.h>

typedef struct VSTORAGE_DEVICE {
    uint8_t type;
    char name[40];
    uint8_t (*Init)();
    uint8_t (*Read)(uint32_t lba, uint8_t* buffer, uint32_t sector_count);
    uint8_t (*Write)(uint32_t lba, uint8_t* buffer, uint32_t sector_count);
} VSTORAGE_DEVICE;

namespace VStorage {
    void Init();
    void RegisterDevice(VSTORAGE_DEVICE device);
    uint8_t Read(uint8_t device, uint32_t lba, uint8_t* buffer, uint32_t sector_count);
    uint8_t Write(uint8_t device, uint32_t lba, uint8_t* buffer, uint32_t sector_count);

    uint8_t ReadBytes(uint64_t address, uint64_t count, void* buffer);
    uint8_t WriteBytes(uint64_t address, uint64_t count, void* buffer);
}

extern VSTORAGE_DEVICE* vsdev;

#define VSTORAGE_DEVICE_ERROR_INIT   0
#define VSTORAGE_DEVICE_ERROR_READ   1
#define VSTORAGE_DEVICE_ERROR_WRITE  2
