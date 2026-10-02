//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
#include <partition/mbrgpt.h>
#include <drivers/dev/dev.h>
#include <mem/heap.h>

// 辅助宏：判断是否为 GPT 保护分区
#define IS_GPT(dpt) ((dpt).PartitionTypeIndicator == 0xEE && (dpt).BootIndicator == 0x00)

/* B3 (round 6): GPT 头 CRC32 (bitwise, 表无关 —— 启动早期不依赖
   init 顺序; GPT 规范: 头 92 字节, CRC 字段 (16..19) 计算时置零) */
static uint32_t gpt_crc32(const uint8_t *data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static bool gpt_header_ok(VDL *d) {
    uint8_t hdr[92];
    if (Dev::ReadBytes(d, 512, 92, hdr) == Dev::RW_ERROR) return false;
    static const char kEfiPart[8] = {'E','F','I',' ','P','A','R','T'};
    for (int i = 0; i < 8; i++)
        if (hdr[i] != kEfiPart[i]) return false;
    uint32_t stored_crc;
    __memcpy(&stored_crc, hdr + 16, 4);
    __memcpy(hdr + 16, "\x00\x00\x00\x00", 4);
    return gpt_crc32(hdr, 92) == stored_crc;
}

/* B3 (round 6): 分区表项数对容量校验 —— entry_count × 128B 不得
   超出设备容量 (原无此检查, 损坏的 entry_count 可驱动越界读) */
static bool gpt_entry_count_sane(VDL *d, uint32_t entry_count) {
    uint64_t cap_bytes = (uint64_t)d->MaxSectorCount * 512;
    return (uint64_t)entry_count * 128 <= cap_bytes;
}

uint8_t IdentifyMBR(VsDevType DriverType, uint32_t DriverID) {
    MBR_DPT dpt;
    VDL* d = Dev::GetSDEV(DriverType, DriverID);
    if (!d) return 2;

    /* P1-42: MBR 引导签名 0x55AA 校验 (缺失时分区表不可信) */
    uint16_t signature = 0;
    if (Dev::ReadBytes(d, 510, 2, &signature) == Dev::RW_ERROR)
        return 2;
    if (signature != 0xAA55) return 2;

    if(Dev::ReadBytes(d, MBR_PARTITION_TABLE_OFFSET, 16, &dpt) == Dev::RW_ERROR)
        return 2;

    if(IS_GPT(dpt)) {
        /* B3 (round 6): GPT 头 CRC32 校验 (原仅 "EFI PART" magic) */
        if (!gpt_header_ok(d)) return 2;
        return 3; // GPT
    }
    return 0; // 传统 MBR
}

uint8_t GetPartitionSize(VsDevType DriverType, uint32_t DriverID, uint32_t PartitionID, uint64_t &PartitionSize) {
    VDL* d = Dev::GetSDEV(DriverType, DriverID);
    if (!d) return 2;
    MBR_DPT dpt;
    if (Dev::ReadBytes(d, MBR_PARTITION_TABLE_OFFSET, 16, &dpt) == Dev::RW_ERROR) return 2;

    if (IS_GPT(dpt)) {
        uint32_t entry_count = 0;
        // GPT Header 在 LBA 1 (512字节处)
        if (Dev::ReadBytes(d, 512 + GPT_HEADER_NUMBER_OF_PTE_OFFSET, 4, &entry_count) == Dev::RW_ERROR)
            return 4; /* 修复: 原忽略读失败, entry_count 为垃圾 */
        if (!gpt_entry_count_sane(d, entry_count)) return 4;   /* B3 */
        if (PartitionID >= entry_count) return 4;

        GPT_PTE gptpte;
        if (Dev::ReadBytes(d, GPT_PARTITION_TABLE_OFFSET + (PartitionID * 128), 128, &gptpte) == Dev::RW_ERROR)
            return 5;

        if (gptpte.PartitionStart == 0) { PartitionSize = 0; return 0; }
        /* 修复: End < Start 时无符号下溢到 ~2^64 */
        if (gptpte.PartitionEnd < gptpte.PartitionStart) { PartitionSize = 0; return 8; }
        PartitionSize = (gptpte.PartitionEnd - gptpte.PartitionStart) + 1;
        return 0;
    } else {
        if (PartitionID >= MBR_PARTITION_MAX) return 6;
        MBR_DPT entry;
        if (Dev::ReadBytes(d, MBR_PARTITION_TABLE_OFFSET + (PartitionID * 16), 16, &entry) == Dev::RW_ERROR)
            return 7;
        PartitionSize = entry.SectorsInPartition;
        return 0;
    }
}

// 注意这里加了 & 引用符号
uint8_t GetPartitionStart(VsDevType DriverType, uint32_t DriverID, uint32_t PartitionID, uint64_t &PartitionStart) {
    VDL* d = Dev::GetSDEV(DriverType, DriverID);
    if (!d) return 2;
    MBR_DPT dpt;
    if(Dev::ReadBytes(d, MBR_PARTITION_TABLE_OFFSET, 16, &dpt) == Dev::RW_ERROR) return 2;

    if(IS_GPT(dpt)) {
        uint32_t buffer = 0;
        if (Dev::ReadBytes(d, 512 + GPT_HEADER_NUMBER_OF_PTE_OFFSET, 4, &buffer) == Dev::RW_ERROR)
            return 4; /* 修复: 原忽略读失败 */
        if(PartitionID >= buffer) return 4;

        GPT_PTE gptpte;
        if(Dev::ReadBytes(d, GPT_PARTITION_TABLE_OFFSET + (PartitionID * 128), 128, &gptpte) == Dev::RW_ERROR)
            return 5;
        PartitionStart = gptpte.PartitionStart;
    } else {
        if(PartitionID >= MBR_PARTITION_MAX) return 6;
        MBR_DPT buffer2;
        if(Dev::ReadBytes(d, MBR_PARTITION_TABLE_OFFSET + (PartitionID * 16), 16, &buffer2) == Dev::RW_ERROR)
            return 7;
        PartitionStart = buffer2.StartLBA;
    }
    return 0;
}

// 注意这里加了 & 引用符号
uint8_t GetPartitionEnd(VsDevType DriverType, uint32_t DriverID, uint32_t PartitionID, uint64_t &PartitionEnd) {
    VDL* d = Dev::GetSDEV(DriverType, DriverID);
    if (!d) return 2;
    MBR_DPT dpt;
    if(Dev::ReadBytes(d, MBR_PARTITION_TABLE_OFFSET, 16, &dpt) == Dev::RW_ERROR) return 2;

    if(IS_GPT(dpt)) {
        uint32_t buffer = 0;
        if (Dev::ReadBytes(d, 512 + GPT_HEADER_NUMBER_OF_PTE_OFFSET, 4, &buffer) == Dev::RW_ERROR)
            return 4; /* 修复: 原忽略读失败 */
        if(PartitionID >= buffer) return 4;

        GPT_PTE gptpte;
        if(Dev::ReadBytes(d, GPT_PARTITION_TABLE_OFFSET + (PartitionID * 128), 128, &gptpte) == Dev::RW_ERROR)
            return 5;
        PartitionEnd = gptpte.PartitionEnd;
    } else {
        if(PartitionID >= MBR_PARTITION_MAX) return 6;
        MBR_DPT entry;
        if(Dev::ReadBytes(d, MBR_PARTITION_TABLE_OFFSET + (PartitionID * 16), 16, &entry) == Dev::RW_ERROR)
            return 7;

        // End = Start + Count - 1
        if (entry.SectorsInPartition == 0) PartitionEnd = 0;
        else PartitionEnd = entry.StartLBA + entry.SectorsInPartition - 1;
    }
    return 0;
}

uint8_t GetPartitionCount(VsDevType DriverType, uint32_t DriverID) {
    VDL* d = Dev::GetSDEV(DriverType, DriverID);
    if (!d) return 0;
    MBR_DPT dpt;
    if(Dev::ReadBytes(d, MBR_PARTITION_TABLE_OFFSET, 16, &dpt) == Dev::RW_ERROR) return 0;

    if(IS_GPT(dpt)) {
        uint32_t buffer = 0;
        // 简单返回分区表项总数（通常是128）
        if(Dev::ReadBytes(d, 512 + GPT_HEADER_NUMBER_OF_PTE_OFFSET, 4, &buffer) == Dev::RW_ERROR)
            return 0;
        /* 修复: 原 (uint8_t) 截断, >=256 时归零 */
        return buffer > 0xFF ? 0xFF : (uint8_t)buffer;
    } else {
        uint8_t count = 0;
        for(uint8_t i = 0; i < MBR_PARTITION_MAX; i++) {
            MBR_DPT buffer2;
            if(Dev::ReadBytes(d, MBR_PARTITION_TABLE_OFFSET + i * 16, 16, &buffer2) == Dev::RW_ERROR)
                break;
            if(buffer2.SectorsInPartition != 0) {
                count++;
            }
        }
        return count;
    }
}
