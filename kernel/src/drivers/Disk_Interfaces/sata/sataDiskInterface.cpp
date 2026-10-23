//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only

#ifdef __x86_64__
#include <drivers/Disk_Interfaces/sata/sataDiskInterface.h>
#include <mem/heap.h>
#include <mem/pmm.h>

#include <drivers/ahci/ahci.h>
#include <drivers/dev/dev.h>


uint64_t SataDiskInterface::GetMaxSectorCount()
{
    return Port->GetMaxSectorCount();
}

SataDiskInterface::SataDiskInterface(AHCI::Port* port)
{
    
    if(port == nullptr)
        return;
    this->Port = port;
    /* P0-12: 每请求缓冲方案后, 共享 buffer 不再用于 DMA —— 保留分配
       仅为兼容其他引用方, 不再有并发语义 */
    this->Port->buffer = (uint8_t*)(PMM::Request()); // 4096 Bytes
    
    this->SectorCount = GetMaxSectorCount();

    // 1. 彻底清理栈上的结构体，防止 ioctl 等未定义指针包含随机值
    DevOPS ops;
    _memset(&ops, 0, sizeof(DevOPS));

    // 2. 使用不捕获变量的 Lambda 进行适配 ([] 内严禁填 this)
    ops.Read = [](void* i, uint64_t lba, uint32_t count, void* buf) -> uint8_t {
        auto* self = static_cast<SataDiskInterface*>(i);
        return self->Read(lba, count, buf) ? Dev::RW_OK : Dev::RW_ERROR;
    };

    ops.Write = [](void* i, uint64_t lba, uint32_t count, void* buf) -> uint8_t {
        auto* self = static_cast<SataDiskInterface*>(i);
        return self->Write(lba, count, buf) ? Dev::RW_OK : Dev::RW_ERROR;
    };

    ops.ReadBytes = [](void* i, uint64_t addr, uint32_t count, void* buf) -> uint8_t {
        auto* self = static_cast<SataDiskInterface*>(i);
        return self->ReadBytes(addr, count, buf) ? Dev::RW_OK : Dev::RW_ERROR;
    };

    ops.WriteBytes = [](void* i, uint64_t addr, uint32_t count, void* buf) -> uint8_t {
        auto* self = static_cast<SataDiskInterface*>(i);
        return self->WriteBytes(addr, count, buf) ? Dev::RW_OK : Dev::RW_ERROR;
    };

    ops.GetMaxSectorCount = [](void* i) -> uint64_t {
        return static_cast<SataDiskInterface*>(i)->GetMaxSectorCount();
    };

    // 3. 注册设备，并将当前实例 (this) 作为私有数据传入
    Dev::AddStorageDevice(VsDevType::SATA, ops, SectorCount, this);
    

}

u8 SataDiskInterface::FRegVsDEV_R(uint64_t lba, uint32_t SectorCount, void* Buffer){
    if(Read(lba, SectorCount, Buffer) == true)
        return Dev::RW_OK;
    else 
        return Dev::RW_ERROR;
}

u8 SataDiskInterface::FRegVsDEV_W(uint64_t lba, uint32_t SectorCount, void* Buffer){
    if(Write(lba, SectorCount, Buffer) == true)
        return Dev::RW_OK;
    else 
        return Dev::RW_ERROR;
}

u8 SataDiskInterface::FRegVsDEV_Wb(uint64_t address, uint64_t count, void* buffer){
    if(WriteBytes(address, count, buffer) == true)
        return Dev::RW_OK;
    else 
        return Dev::RW_ERROR;
}

u8 SataDiskInterface::FRegVsDEV_Rb(uint64_t address, uint64_t count, void* buffer){
    if(ReadBytes(address, count, buffer) == true)
        return Dev::RW_OK;
    else 
        return Dev::RW_ERROR;
}

/* 预留的 DMA 页池。
   原来每个请求都现场 PMM::Request() 一页, 内存紧张时这一页拿不到, Read()
   就直接返回 false —— 上层 ext4 收到 EIO, 用户态看到的是"文件读到一半
   返回 0"。实测: 80 MiB 下 DOOM 在 WAD 的 3.4 MB 偏移处稳定报
   `W_ReadLump: only read 0 of 16297`, 而同一地址在 84 MiB 下完全正常。
   块设备读不该因为内存紧张而失败, 所以启动时先留一小把页, 请求时从池里
   取 (纯原子操作, 不会阻塞, 因此不受 AHCI WaitCommand 的 sti 窗口影响),
   只有池空时才退回 PMM。 */
#define SATA_DMA_POOL 8
static void*         g_dma_pages[SATA_DMA_POOL];
static volatile int  g_dma_busy[SATA_DMA_POOL];
static volatile int  g_dma_pool_state;      /* 0=未初始化 1=初始化中 2=就绪 */

static void dma_pool_init(void) {
    int expected = 0;
    if (!__atomic_compare_exchange_n(&g_dma_pool_state, &expected, 1,
                                     0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
        return;                              /* 别人正在初始化或已就绪 */
    for (int i = 0; i < SATA_DMA_POOL; i++) {
        g_dma_pages[i] = PMM::Request();
        g_dma_busy[i] = 0;
    }
    __atomic_store_n(&g_dma_pool_state, 2, __ATOMIC_RELEASE);
}

static void* dma_get(void) {
    /* 首次使用时建池; 若别的 CPU 正在建, 本次直接走下面的动态分配退回。 */
    if (__atomic_load_n(&g_dma_pool_state, __ATOMIC_ACQUIRE) != 2)
        dma_pool_init();
    for (int i = 0; i < SATA_DMA_POOL; i++) {
        if (!g_dma_pages[i]) continue;
        if (!__atomic_test_and_set(&g_dma_busy[i], __ATOMIC_ACQUIRE))
            return g_dma_pages[i];
    }
    return PMM::Request();                   /* 池满: 退回动态分配 */
}

static void dma_put(void* p) {
    if (!p) return;
    for (int i = 0; i < SATA_DMA_POOL; i++) {
        if (g_dma_pages[i] == p) {
            __atomic_clear(&g_dma_busy[i], __ATOMIC_RELEASE);
            return;
        }
    }
    PMM::Free(p);
}

bool SataDiskInterface::Read(uint64_t sector, uint32_t sectorCount, void* buffer)
{
    /* P0-12 重做 (round 80): 每请求独立 DMA 页 —— round 52 的 iolock
       方案因 AHCI WaitCommand 的 sti 窗口回退; 每请求缓冲无共享状态,
       天然并发安全。现在改为优先从预留池取 (见 dma_get), 池空才现场分配。 */
    uint8_t* buf = (uint8_t*)buffer;
    int32_t sectorCountDiv8 = ((sectorCount) / 8);

    void* dma_phys = dma_get();
    if (!dma_phys) return false;
    void* hhdm_buffer = HIGHER_HALF(dma_phys);

    debugpln("(SataDiskInterface::Read)HIT 2");
    for (int32_t sect = 0; sect < sectorCountDiv8; sect++)
    {
        _memset(hhdm_buffer, 0, 0x1000);
        debugpln("(SataDiskInterface::Read)HIT 3");
        if (!Port->Read(sector, 8, dma_phys))
        {
            debugpln("(SataDiskInterface::Read)HIT ERROR 1");
            dma_put(dma_phys);
            return false;
        }
        __memcpy(buf, hhdm_buffer, 0x1000);
        
        buf += 0x1000;
        sector += 8;
    }

    // 处理剩余不足 8 个扇区的部分
    uint32_t remaining = sectorCount % 8;
    if (remaining > 0) {
        _memset(hhdm_buffer, 0, remaining << 9);
        if (!Port->Read(sector, remaining, dma_phys))
        {
            debugpln("(SataDiskInterface::Read)HIT ERROR 2");
            dma_put(dma_phys);
            return false;
        }
        __memcpy(buf, hhdm_buffer, remaining << 9);
    }

    debugpln("(SataDiskInterface::Read)HIT OK!");
    dma_put(dma_phys);
    return true;
}

bool SataDiskInterface::Write(uint64_t sector, uint32_t sectorCount, void* buffer)
{
    uint8_t* buf = (uint8_t*)buffer;
    int32_t sectorCountDiv8 = ((sectorCount) / 8);
    
    /* P0-12: 每请求独立 DMA 页 (同 Read) */
    void* dma_phys = PMM::Request();
    if (!dma_phys) return false;
    void* hhdm_buffer = HIGHER_HALF(dma_phys);

    for (int32_t sect = 0; sect < sectorCountDiv8; sect++)
    {
        _memset(hhdm_buffer, 0, 0x1000);
        __memcpy(hhdm_buffer, buf, 0x1000);
        
        if (!Port->Write(sector, 8, dma_phys))
        {
            PMM::Free(dma_phys);
            return false;
        }
        buf += 0x1000;
        sector += 8;
    }

    uint32_t remaining = sectorCount % 8;
    if (remaining > 0) {
        _memset(hhdm_buffer, 0, remaining << 9);
        
        __memcpy(hhdm_buffer, buf, remaining << 9);
        
        if (!Port->Write(sector, remaining, dma_phys))
        {
            PMM::Free(dma_phys);
            return false;
        }
    }
    
    PMM::Free(dma_phys);
    return true;
}


bool SataDiskInterface::ReadBytes(uint64_t address, uint64_t count, void* buffer)
{
    if (count == 0)
        return true;
    /* A4 (round 1 修复): 溢出安全边界 —— 原 address+count-1 >= C*512
       在 address+count 回绕时失效。改为减法形式 (无回绕) + 扇区级
       双保险: 起始扇区 + 需读扇区数不得超过容量 */
    if (address > SectorCount * 512 || count > SectorCount * 512 - address)
        return false;
    
    uint32_t tempSectorCount = ((((address + count) + 511) / 512) - (address / 512));
    if ((address / 512) + tempSectorCount > SectorCount)
        return false;   /* 尾扇区越界 (A4: 扇区级判据) */
    uint8_t* buffer2 = (uint8_t*)kmalloc(tempSectorCount * 512);//"Malloc for Read Buffer"
    if (!buffer2) return false;   /* OOM 显式失败 */
    _memset(buffer2, 0, tempSectorCount * 512);

    /* P1-38: 读失败直接返回, 不得把未填充的 buffer2 拷给调用者
       (原实现失败路径仍拷贝零页 → 调用者拿到被污染的缓冲) */
    if (!Read((address / 512), tempSectorCount, buffer2))
    {
        kfree(buffer2);
        return false;
    }

    uint16_t offset = address % 512;
    for (uint64_t i = 0; i < count; i++)
        ((uint8_t*)buffer)[i] = buffer2[i + offset];
            
    kfree(buffer2);
    
    return true;
}

bool SataDiskInterface::WriteBytes(uint64_t address, uint64_t count, void* buffer)
{
    //Window* window = osData.mainTerminalWindow;
    
    if (count == 0)
        return true;
    /* A4 (round 1 修复): 溢出安全边界 + 扇区级双保险 (同 ReadBytes) */
    if (address > SectorCount * 512 || count > SectorCount * 512 - address)
        return false;
    
    uint32_t tempSectorCount = ((((address + count) + 511) / 512) - (address / 512));
    if ((address / 512) + tempSectorCount > SectorCount)
        return false;   /* 尾扇区越界 */
    uint8_t* buffer2 = (uint8_t*)kmalloc(512); //Malloc for Write Buffer
    //window->Log("Writing Bytes...");
    
    if (tempSectorCount == 1)
    {
        //window->Log("Writing Bytes (1/1)");
        //uint8_t* buffer2 = (uint8_t*)malloc(512, "Malloc for Read Buffer (1/1)");
        _memset(buffer2, 0, 512);
        if (!Read((address / 512), 1, buffer2))
        {
            kfree(buffer2);
            
            return false;
        }

        uint16_t offset = address % 512;
        for (uint64_t i = 0; i < count; i++)
            buffer2[i + offset] = ((uint8_t*)buffer)[i];

        if (!Write((address / 512), 1, buffer2))
        {
            kfree(buffer2);
            
            return false;
        }
        
        //free(buffer2);
    }
    else
    {
        //window->Log("Writing Bytes (1/3)");
        uint64_t newAddr = address;
        uint64_t newCount = count;
        uint64_t addrOffset = 0;
        {
            // GlobalRenderer->Clear(Colors.dgray);
            // while (true);


            //uint8_t* buffer2 = (uint8_t*)malloc(512, "Malloc for Read Buffer (1/2)");
            _memset(buffer2, 0, 512);
            //window->Log("Writing to Sector: {}", to_string((address / 512)), Colors.yellow);
            if (!Read((address / 512), 1, buffer2))
            {
                kfree(buffer2);
                
                return false;
            }

            uint16_t offset = address % 512;
            uint16_t specialCount = 512 - offset;
            addrOffset = specialCount;
            newAddr = address + specialCount;
            newCount = count - specialCount;

            // window->Log("Address:        {}", to_string(address), Colors.yellow);
            // window->Log("Count:          {}", to_string(count), Colors.yellow);
            // window->Log("New Address:    {}", to_string(newAddr), Colors.yellow);
            // window->Log("New Count:      {}", to_string(newCount), Colors.yellow);
            // window->Log("Address Offset: {}", to_string(addrOffset), Colors.yellow);

            for (uint64_t i = 0; i < specialCount; i++)
                buffer2[i + offset] = ((uint8_t*)buffer)[i];

            if (!Write((address / 512), 1, buffer2))
            {
                kfree(buffer2);
                
                return false;
            }
            
            //free(buffer2);
        }

        //window->Log("Writing Bytes (2/3)");
        {
            //uint8_t* buffer2 = (uint8_t*)malloc(512, "Malloc for Read Buffer (1/2)");
            _memset(buffer2, 0, 512);
            //window->Log("Writing to Sector: {}", to_string((address + count - 1) / 512), Colors.yellow);
            if (!Read(((address + count) / 512), 1, buffer2))
            {
                kfree(buffer2);
                
                return false;
            }

            uint16_t specialCount = ((address + count) % 512);
            //uint64_t offset = (address + count - specialCount);
            newCount -= specialCount;
            //newAddr = address + specialCount;

            // GlobalRenderer->Clear(Colors.black);

            // GlobalRenderer->Println("Address:        {}", to_string(address), Colors.yellow);
            // GlobalRenderer->Println("Count:          {}", to_string(count), Colors.yellow);
            // GlobalRenderer->Println("New Address:    {}", to_string(newAddr), Colors.yellow);
            // GlobalRenderer->Println("New Count:      {}", to_string(newCount), Colors.yellow);
            // //window->Log("Offset:         {}", to_string(offset), Colors.yellow);
            // GlobalRenderer->Println("Address Offset: {}", to_string(addrOffset), Colors.yellow);
            // GlobalRenderer->Println("Special Count:  {}", to_string(specialCount), Colors.yellow);

            uint64_t blehus = (count - specialCount);
            //GlobalRenderer->Println("blehus:  {}", to_string(blehus), Colors.yellow);

            // GlobalRenderer->Println("BUFF (1):");
            // for (int32_t i = 0; i < 512; i++)
            //     GlobalRenderer->Print("{} ", to_string((int32_t)buffer2[i]), Colors.yellow);
            // GlobalRenderer->Println("");

            for (int64_t i = 0; i < specialCount; i++)
                buffer2[i] = ((uint8_t*)buffer)[i + blehus];

            // GlobalRenderer->Println("BUFF (2):");
            // for (int32_t i = 0; i < 512; i++)
            //     GlobalRenderer->Print("{} ", to_string((int32_t)buffer2[i]), Colors.yellow);
            // GlobalRenderer->Println("");

            //while (true);

            if (!Write(((address + count) / 512), 1, buffer2))
            {
                kfree(buffer2);
                
                return false;
            }
            
            //free(buffer2);
        }
        
        //window->Log("Writing Bytes (3/3)");
        {
            uint64_t newSectorCount = newCount / 512;
            if (newSectorCount != 0)
            {
                uint64_t newSectorStartId = newAddr / 512;

                // window->Log("newSectorCount:    {}", to_string(newSectorCount), Colors.yellow);
                // window->Log("newSectorStartID:  {}", to_string(newSectorStartId), Colors.yellow);
                // window->Log("Address Offset:    {}", to_string(addrOffset), Colors.yellow);

                if (!Write(newSectorStartId, newSectorCount, (void*)((uint64_t)buffer + addrOffset)))
                {
                    kfree(buffer2);
                    
                    return false;
                }
            }
        }


        
    }
    kfree(buffer2);


    // free(buffer2);
    
    return true;
}

#endif