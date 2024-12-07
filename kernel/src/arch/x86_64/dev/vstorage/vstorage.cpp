#include "vstorage.h"
#include "../../../../mem/heap.h"

VSTORAGE_DEVICE* vsdev = { 0 };
u16 next = 0;

void VStorage::Init() {
    vsdev = (VSTORAGE_DEVICE*)kmalloc(sizeof(VSTORAGE_DEVICE) * 256);
    _memset(vsdev, 0, sizeof(VSTORAGE_DEVICE) * 256);
}

void VStorage::RegisterDevice(VSTORAGE_DEVICE device) {
    vsdev[next] = device;
    next++;
}

u8 VStorage::Read(u8 device, u32 lba, u8* buffer, u32 sector_count) {
    return vsdev[device].Read(lba, buffer, sector_count);
}

u8 VStorage::Write(u8 device, u32 lba, u8* buffer, u32 sector_count) {
    return vsdev[device].Write(lba, buffer, sector_count);
}

