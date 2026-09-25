//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
#ifdef __x86_64__
#include <drivers/nvme/nvme.h>
#include <arch/x86_64/vmm/vmm.h>
#include <arch/x86_64/pit/pit.h>
#include <arch/x86_64/smp/smp.h>
#include <drivers/dev/dev.h>
#include <mem/heap.h>

extern volatile uint64_t smp_cpu_count;

/* dev.h's unscoped VsDevType enumerator "NVME" hides class NVME for ordinary
   (non-elaborated) name lookup once this TU includes dev.h; alias the class so
   the controller type can be named unambiguously below. */
using NVMECtrl = class ::NVME;

#define ReadReg64(off)      this->ReadReg(off)
#define WriteReg64(off,v)   this->WriteReg(off,v)

// 全局中断路由表：用于在中断上下文中通过向量号快速查找 NVMe 实例和队列
struct NVMEIrqRoute {
    NVMECtrl* instance;
    uint16_t queueId;
    bool valid;
} nvme_irq_routes[256];

static int32_t NVMEResponse(NVME::NVMERequest *req, NVME::SubQueEntry *entry){
    __memcpy(entry, &req->res, sizeof(NVME::CmplQueEntry));
    return 0;
}

inline void NVME::WriteCmplDB(NVME::CmplQue *cmplQue){
    this->WriteReg(0x1000 + (cmplQue->Ident << 1 | 1) * this->DBStride, cmplQue->Pos);
}

bool NVME::InitQue(){
    this->cq = (NVME::CmplQue**)kmalloc(sizeof(NVME::CmplQue*) * INTRNUM);
    this->sq = (NVME::SubQue**)kmalloc(sizeof(NVME::SubQue*) * INTRNUM);

    if (this->cq == NULL || this->sq == NULL) {
        kerror("[NVME %p]: failed to allocate completion queue / submission queue array\n", (uint64_t)this);
        return false;
    }

    uint32_t cmplQueSz, subQueSz;
    {
        uint64_t cap = ReadReg64(NVME_CTRLREG_CAP);
        cmplQueSz = min(NVME_CmplQueSz, (cap & 0xffff) + 1);
        subQueSz = min(NVME_SubQueSz, (cap & 0xffff) + 1);
    }
    for (uint32_t i = 0; i < this->INTRNUM; i++) {
        this->cq[i] = this->AllocCmplQue(i, cmplQueSz);
        if (this->cq[i] == nullptr) {
            kerror("[NVME %p]: failed to allocate completion queue %d\n", (uint64_t)this, i);
            return false;
        }
    }
    for (uint32_t i = 0; i < this->INTRNUM; i++) {
        this->sq[i] = this->AllocSubQue(i, subQueSz, this->cq[i]);
        if (this->sq[i] == nullptr) {
            kerror("[NVME %p]: failed to allocate submission queue %d\n", (uint64_t)this, i);
            return false;
        }
    }

    kinfo("[NVME %p]: subQueSz:%d cmplQueSz:%d\n", (uint64_t)this, subQueSz, cmplQueSz);

    // 必须写入物理地址，且必须保证 kmalloc 返回的是页对齐内存
    WriteReg64(NVME_CTRLREG_ASQ, VMM::GetPhysics(kernel_pagemap, (uint64_t)this->sq[0]->Entries));
    WriteReg64(NVME_CTRLREG_ACQ, VMM::GetPhysics(kernel_pagemap, (uint64_t)this->cq[0]->Entries));

    {
        uint32_t ada = (this->sq[0]->Size - 1) | ((this->cq[0]->Size - 1) << 16);
        this->WriteReg(NVME_CTRLREG_AQA, ada);
    }
    
    return true;
}

NVME::NVME(PCI::PCIHeader0 *header){
    this->phdr = header;
    this->dev = nullptr;
    this->devNum = 0;
    this->flags = 0;
    this->FialureNUM = 0;

    // PCI config: respond to memory/IO accesses and bus-mastered DMA.
    PCI::enable_interrupt((uint64_t)this->phdr);
    PCI::enable_io_space((uint64_t)this->phdr);
    PCI::enable_mem_space((uint64_t)this->phdr);
    PCI::enable_bus_mastering((uint64_t)this->phdr);

    // Map the 64-bit MMIO BAR: controller registers plus per-queue doorbells.
    uint64_t physBase = ((uint64_t)header->BAR1 << 32) | (header->BAR0 & 0xFFFFFFF0);
    for (uint64_t off = 0; off < 0x10000; off += 0x1000)
        VMM::Map((pagemap_t*)kernel_pagemap, physBase + off, physBase + off, VMM_FLAGS_MMIO);
    this->BaseAddr = physBase + hhdm_offset;

    // Bring the controller down: CC.EN = 0 and wait for CSTS.RDY to clear.
    this->WriteReg(NVME_CTRLREG_CC, 0);
    this->CAPREG = this->ReadReg(NVME_CTRLREG_CAP);
    {
        int32_t to = (int32_t)(500 * ((this->CAPREG >> 24) & 0xFF)) + 200;
        while ((this->ReadReg(NVME_CTRLREG_CSTS) & 1) && --to > 0)
            PIT::Sleep(1);
    }

    // Re-read CAP after quiesce. CAP.NSSRS (bit 36) gates the subsystem reset.
    this->CAPREG = this->ReadReg(NVME_CTRLREG_CAP);
    if (this->CAPREG & (1ULL << 36))
        this->WriteReg(NVME_CTRLREG_NSSR, 0x4E564D65u); // 'NVMe'

    this->DBStride = 1 << (2 + ((this->CAPREG >> 32) & 0xF));

    // IOSQES/IOCQES are log2 of the on-wire queue entry sizes; keep CC.EN clear.
    uint32_t iosqes = __builtin_ctz(sizeof(NVME::SubQueEntry));
    uint32_t iocqes = __builtin_ctz(sizeof(NVME::CmplQueEntry));
    // CC layout: IOSQES occupies bits 19:16 and IOCQES bits 23:20.
    uint32_t regCC = (iocqes << 20) | (iosqes << 16) |
                     (((this->CAPREG >> 17) & 0x3) << 11);
    this->WriteReg(NVME_CTRLREG_CC, regCC);

    kinfoln("[NVME %p]: cap:%#018lx version:%x dbStride:%d\n",
        (uint64_t)this, this->CAPREG, this->ReadReg(NVME_CTRLREG_VS), this->DBStride);

    // Configure MSI-X first so the final, vector-backed queue count is known.
    this->INTRNUM = (uint16_t)min(max(2UL, smp_cpu_count), (uint64_t)NVME_MAX_INTRNUM);
    if (!this->InitIntr()) {
        kerror("[NVME %p]: interrupt setup failed, skipping controller\n", (uint64_t)this);
        this->FialureNUM = 2;
        return;
    }

    // Allocate admin + IO queue memory and publish ASQ/ACQ/AQA.
    if (!this->InitQue()) { this->FialureNUM = 1; return; }

    // Enable the controller and wait for CSTS.RDY to assert.
    this->WriteReg(NVME_CTRLREG_CC, this->ReadReg(NVME_CTRLREG_CC) | 1);
    {
        int32_t to = (int32_t)(500 * ((this->CAPREG >> 24) & 0xFF)) + 200;
        while (--to > 0) {
            if (this->ReadReg(NVME_CTRLREG_CSTS) & 1) break;
            PIT::Sleep(1);
        }
    }
    if (!(this->ReadReg(NVME_CTRLREG_CSTS) & 1)) {
        kerror("[NVME %p]: failed to enable, csts=%08x cc=%08x\n", (uint64_t)this,
            this->ReadReg(NVME_CTRLREG_CSTS), this->ReadReg(NVME_CTRLREG_CC));
        this->FialureNUM = 3;
        return;
    }
    kpok("[NVME %p]: controller enabled, csts=%08x cc=%08x\n", (uint64_t)this,
        this->ReadReg(NVME_CTRLREG_CSTS), this->ReadReg(NVME_CTRLREG_CC));

    // Mask every vector first, then unmask the queues we actually created.
    this->WriteReg(NVME_CTRLREG_INITMS, 0xFFFFFFFFu);
    this->WriteReg(NVME_CTRLREG_INITMC, (1u << this->INTRNUM) - 1);

    if (!this->RegisterQue()) { this->FialureNUM = 4; return; }
    if (!this->InitNsp())    { this->FialureNUM = 5; return; }

    kpok("[NVME %p]: SUCCESSFULLY INITIALIZED\n", (uint64_t)this);
}

NVME::NVMERequest *NVME::MakeReq(int32_t InputSize){
    NVME::NVMERequest *req = (NVME::NVMERequest*)kmalloc(sizeof(NVME::NVMERequest) + InputSize * sizeof(NVME::SubQueEntry));
    if(req == nullptr){
        kerror("(NVME %p) FAILED TO ALLOC REQUEST!,SIZE = %d", InputSize);
        return nullptr;
    }
    _memset(req->input, 0, sizeof(NVME::SubQueEntry) * InputSize);
    req->inputSz = InputSize;
    return req;
}

bool NVME::CreateSubQue(NVME::NVMERequest *req, NVME::SubQue *subQue) {
    NVME::SubQueEntry *entry = &req->input[0];
    entry->OPCode = 0x01;
    entry->PRP[0] = VMM::GetPhysics(kernel_pagemap, (uint64_t)subQue->Entries);
    entry->Spec[0] = subQue->Ident | (((u32)subQue->Size - 1) << 16);
    entry->Spec[1] = 0b1 | (((u32)subQue->Trg->Ident) << 16);
    return true;
}

bool NVME::CreateCmplQue(NVME::NVMERequest *req, NVME::CmplQue *cmplQue) {
    NVME::SubQueEntry *entry = &req->input[0];
    entry->OPCode = 0x05;
    entry->PRP[0] = VMM::GetPhysics(kernel_pagemap, (uint64_t)cmplQue->Entries);
    entry->Spec[0] = cmplQue->Ident | ((u32)(cmplQue->Size - 1) << 16);
    entry->Spec[1] = 0b11 | ((u32)(cmplQue->Ident) << 16);
    return true;
}

bool NVME::TryInsertRequest(NVME::SubQue *subQue, NVME::NVMERequest *req) {
    spinlock_lock(&subQue->Lock);
    if (subQue->Load + req->inputSz > subQue->Size) {
        spinlock_unlock(&subQue->Lock);
        return false;
    }
    subQue->Load += req->inputSz;
    for (int32_t i = 0; i < req->inputSz; i++) {
        req->input[i].CommandIdent = subQue->Tail;
        subQue->Req[subQue->Tail] = req;
        // __memcpy 参数顺序是 dest, src
        __memcpy(subQue->Entries + subQue->Tail, &req->input[i], sizeof(NVME::SubQueEntry));
        if ((++subQue->Tail) == subQue->Size) subQue->Tail = 0;
    }
    spinlock_unlock(&subQue->Lock);
    return true;
}

void NVME::Request(NVME::SubQue *subQue, NVME::NVMERequest *req){
    req->done = false; // 重置完成标志
    while (!this->TryInsertRequest(subQue, req)) {
        asm volatile ("pause");
    }

    this->WriteReg(0x1000 + (subQue->Ident << 1) * this->DBStride, subQue->Tail);

    // Wait for completion. Polling the target CQ lets bring-up/admin commands
    // finish before interrupts are enabled; once MSI-X is live the IRQ handler
    // performs the same harvest ahead of us (PollCQ is phase-idempotent).
    while (!req->done) {
        this->PollCQ(subQue->Trg);
        if (!req->done)
            asm volatile ("pause");
    }

    // Inspect the completion: SC (bits 8:1) and SCT (bits 10:9) must be zero.
    if (!nvme_status_ok(req->res.Status)) {
        kerror("(NVME %p): request %p failed status:%04x (sc=%u sct=%u)\n",
            (uint64_t)this, req, req->res.Status,
            nvme_status_sc(req->res.Status), nvme_status_sct(req->res.Status));
    }
}

bool NVME::RegisterQue() {
    // Negotiate the number of IO queues (feature 07h) before creating them;
    // this also activates the per-queue interrupt vectors on the controller.
    // The number of IO queue pairs we actually want (one per CPU beyond BSP).
    uint16_t nIO = this->INTRNUM - 1;
    {
        NVME::NVMERequest *freq = this->MakeReq(1);
        NVME::SubQueEntry *fe = &freq->input[0];
        fe->OPCode = 0x09;                       // Set Features
        fe->Spec[0] = 0x07;                      // FID: Number of Queues
        // Number of Queues fields are 0-based: value N means N + 1 queues.
        uint32_t want0 = (uint32_t)(nIO - 1);
        fe->Spec[1] = (want0 << 16) | want0;
        this->Request(this->sq[0], freq);
        uint16_t gotCq = (uint16_t)((freq->res.Spec[0] & 0xFFFF) + 1);
        uint16_t gotSq = (uint16_t)((freq->res.Spec[0] >> 16) + 1);
        uint16_t got = min(gotCq, gotSq);
        kfree(freq);
        if (got < nIO) {
            kerror("[NVME %p]: only %u io queues available, wanted %u\n",
                (uint64_t)this, got, nIO);
            return false;
        }
    }

    NVME::NVMERequest *req = this->MakeReq(1);
    for (uint16_t i = 1; i <= nIO; i++) {
        this->CreateCmplQue(req, this->cq[i]);
        this->Request(this->sq[0], req);
        if (!nvme_status_ok(req->res.Status)) {
            kerror("(NVME %p): failed to register io completion queue #%d\n", (uint64_t)this, i);
            return false;
        }

        this->CreateSubQue(req, this->sq[i]);
        this->Request(this->sq[0], req);
        if (!nvme_status_ok(req->res.Status)) {
            kerror("(NVME %p): failed to register io submission queue #%d\n", (uint64_t)this, i);
            return false;
        }
    }
    kfree(req);
    return true;
}

bool NVME::InitNsp() {
    // Identify data is a full 4 KiB page; back it with page-aligned memory so a
    // single PRP0 describes the whole transfer (kmalloc does not align to 4 KiB).
    uint32_t *nspLst = (uint32_t*)VMM::Alloc((pagemap_t*)kernel_pagemap, 1, false);
    NVME::NameSpace *nsp = (NVME::NameSpace*)VMM::Alloc((pagemap_t*)kernel_pagemap, 1, false);
    if (!nspLst || !nsp) {
        if (nspLst) VMM::Free((pagemap_t*)kernel_pagemap, nspLst);
        if (nsp) VMM::Free((pagemap_t*)kernel_pagemap, nsp);
        return false;
    }
    _memset(nspLst, 0, PAGE_SIZE);
    NVME::NVMERequest *req = this->MakeReq(1);
    if (!req) { kfree(nspLst); kfree(nsp); return false; }

    NVME::InitREQIdent(req, REQ_IDENTIFY_TYPE_NSPLST, 0, nspLst);
    NVME::Request(this->sq[0], req);
    if (!nvme_status_ok(req->res.Status)) {
        kerror("[NVME %p]: identify active ns list failed status:%04x\n",
            (uint64_t)this, req->res.Status);
        kfree(req);
        VMM::Free((pagemap_t*)kernel_pagemap, nspLst);
        VMM::Free((pagemap_t*)kernel_pagemap, nsp);
        return false;
    }

    for (this->devNum = 0; nspLst[this->devNum]; this->devNum++) ;

    if (this->devNum == 0) {
        kinfo("[NVME %p]: no active namespace\n", (uint64_t)this);
        kfree(req);
        VMM::Free((pagemap_t*)kernel_pagemap, nspLst);
        VMM::Free((pagemap_t*)kernel_pagemap, nsp);
        return true; // controller usable, just no namespaces
    }

    this->dev = (NVME::NVMEDev*)kmalloc(sizeof(NVME::NVMEDev) * this->devNum);
    if (!this->dev) {
        kfree(req);
        VMM::Free((pagemap_t*)kernel_pagemap, nspLst);
        VMM::Free((pagemap_t*)kernel_pagemap, nsp);
        return false;
    }
    _memset(this->dev, 0, sizeof(NVME::NVMEDev) * this->devNum);
    kinfo("[NVME %p]: namespace count:%d\n", (uint64_t)this, this->devNum);

    for (int32_t i = 0; i < this->devNum; i++) {
        NVME::InitREQIdent(req, REQ_IDENTIFY_TYPE_NSP, nspLst[i], nsp);
        NVME::Request(this->sq[0], req);

        // Resolve the formatted LBA size through FLBAS and the LBAF table.
        uint32_t lbaIdx = nsp->formattedLbaSz & 0xF;
        uint32_t lbaBytes = 1u << nsp->lbaFormat[lbaIdx].lbaDtSz;

        NVMEDev *d = &this->dev[i];
        d->nspId = (int32_t)nspLst[i];
        d->size = nsp->nspSz;
        d->lbaBytes = lbaBytes;
        d->lck = 0;
        d->reqBitmap = 0;
        // Pre-allocate one reusable request per slot (64 in-flight slots).
        for (int32_t j = 0; j < 64; j++)
            d->reqs[j] = this->MakeReq(1);

        kinfo("[NVME %p]: ns #%d nsid:%u lbaBytes:%u lbaCount:%ld\n",
            (uint64_t)this, i, nspLst[i], lbaBytes, nsp->nspSz);
    }

    kfree(req);
    VMM::Free((pagemap_t*)kernel_pagemap, nspLst);
    VMM::Free((pagemap_t*)kernel_pagemap, nsp);

    // Register the first namespace. Dev/PartitionManager addresses storage in
    // 512-byte sectors regardless of the controller's formatted LBA size.
    NVMEDev *d0 = &this->dev[0];
    uint64_t sectors512 = d0->size * ((uint64_t)d0->lbaBytes / 512);

    DevOPS ops;
    _memset(&ops, 0, sizeof(ops));
    ops.Read = [](void* i, uint64_t lba, uint32_t cnt, void* buf) -> uint8_t {
        auto* self = static_cast<NVMECtrl*>(i);
        return self->Read(lba, cnt, buf) == cnt ? Dev::RW_OK : Dev::RW_ERROR;
    };
    ops.Write = [](void* i, uint64_t lba, uint32_t cnt, void* buf) -> uint8_t {
        auto* self = static_cast<NVMECtrl*>(i);
        return self->Write(lba, cnt, buf) == cnt ? Dev::RW_OK : Dev::RW_ERROR;
    };
    ops.GetMaxSectorCount = [](void* i) -> uint64_t {
        auto* self = static_cast<NVMECtrl*>(i);
        return self->dev[0].size * ((uint64_t)self->dev[0].lbaBytes / 512);
    };
    Dev::AddStorageDevice(VsDevType::NVME, ops, (uint32_t)sectors512, this);
    return true;
}

uint32_t NVME::ReadReg(uint32_t offset){
    volatile uint32_t *nvme_reg = (volatile uint32_t *)(this->BaseAddr + offset);
    return *nvme_reg;
}

void NVME::WriteReg(uint32_t offset, uint32_t value){
    volatile uint32_t *nvme_reg = (volatile uint32_t *)(this->BaseAddr + offset);
    *nvme_reg = value;
}

NVME::SubQue *NVME::AllocSubQue(uint32_t iden, uint32_t size, NVME::CmplQue *trg) {
    // The controller requires the SQ base address to be 4 KiB aligned and the
    // whole ring physically contiguous, so back it with whole pages (kmalloc
    // does not guarantee page alignment).
    uint64_t entriesBytes = (uint64_t)size * sizeof(NVME::SubQueEntry);
    uint64_t totalBytes = entriesBytes + sizeof(NVME::SubQue) +
                          (uint64_t)size * sizeof(NVMERequest*);
    uint64_t pages = (totalBytes + 0xFFFu) >> 12;
    uint8_t *block = (uint8_t*)VMM::Alloc((pagemap_t*)kernel_pagemap, pages, false);
    if (!block) {
        kerror("[NVME %p]: failed to allocate submission queue size=%d\n", (uint64_t)this, size);
        return nullptr;
    }
    _memset(block, 0, pages * 0x1000);

    NVME::SubQueEntry *entry = (NVME::SubQueEntry*)block; // page aligned
    NVME::SubQue *que = (NVME::SubQue*)(block + entriesBytes);
    que->Entries = entry;
    que->Ident = iden;
    que->Tail = que->Head = 0;
    que->Size = size;
    que->Load = 0;
    que->Trg = trg;
    return que;
}

NVME::CmplQue *NVME::AllocCmplQue(uint32_t iden, uint32_t size) {
    // Completion queues have the same 4 KiB aligned, physically-contiguous rule.
    uint64_t entriesBytes = (uint64_t)size * sizeof(NVME::CmplQueEntry);
    uint64_t totalBytes = entriesBytes + sizeof(NVME::CmplQue);
    uint64_t pages = (totalBytes + 0xFFFu) >> 12;
    uint8_t *block = (uint8_t*)VMM::Alloc((pagemap_t*)kernel_pagemap, pages, false);
    if (!block) {
        kerror("[NVME %p]: failed to allocate completion queue size=%d\n", (uint64_t)this, size);
        return nullptr;
    }
    _memset(block, 0, pages * 0x1000);

    NVME::CmplQueEntry *entry = (NVME::CmplQueEntry*)block; // page aligned
    NVME::CmplQue *que = (NVME::CmplQue*)(block + entriesBytes);
    que->Entries = entry;
    que->Ident = iden;
    que->Phase = 1;
    que->Pos = 0;
    que->Size = size;
    return que;
}

extern volatile uint64_t smp_cpu_count;
void MSIXHandler(context_t *ctx);

bool NVME::InitIntr() {
    this->flags &= ~NVME_FLAG_MISIX;
    this->MSIX = PCI::GetMSIXCap(this->phdr);
    if (this->MSIX == nullptr) {
        kerror("[NVME: %p]: no MSI-X support\n", (uint64_t)this);
        return false;
    }else{this->flags |= NVME_FLAG_MISIX;}

    this->INTRNUM = max(2, min(NVME_MAX_INTRNUM, smp_cpu_count));
    
    if (this->flags & NVME_FLAG_MISIX) {
        kinfo("[NVME: %p]: use msix\n", (uint64_t)this);
        int32_t vecNum = PCI_MSIX_CAP_VecNum(this->MSIX);
        vecNum = this->INTRNUM = min(vecNum, this->INTRNUM);

        PCI::PCI_MSIX_CAP *Cap = PCI::GetMSIXCap(this->phdr);
        PCI::PCI_MSIX_TABLE* Tbl = (PCI::PCI_MSIX_TABLE*)PCI::GetMSIXTblBaseAddr(this->phdr, Cap);
        
        for (int32_t i = 0; i < this->INTRNUM; i++){
            uint32_t targetCpu = GetLWIntrCpu()->id; // 获取绑定的 CPU
            uint16_t vector = RequestFreeIRQPerCPU(); // 获取空闲向量号

            // 注册到全局路由表
            nvme_irq_routes[vector].instance = this;
            nvme_irq_routes[vector].queueId = i;
            nvme_irq_routes[vector].valid = true;

            this->IRQMap[i].CPUID = targetCpu;
            this->IRQMap[i].VecID = vector;
            this->IRQMap[i].Instance = this;
            this->IRQMap[i].QueueID = i;

            uint32_t MsgAddr = 0xfee00000u | (targetCpu << 12);
            Tbl[i].msgAddr = MsgAddr;
            Tbl[i].msgData = vector; // MsgData 必须是 IDT 向量号
            __asm__ volatile ("mfence" ::: "memory");
            Tbl[i].vecCtrl &= ~1u; // 解除 Mask 
            
            idt_install_irq_cpu(targetCpu, vector, (void*)MSIXHandler);
        }
        PCI::enable_bus_mastering((uint64_t)this->phdr);
        Cap->MsgCtrl |= (1 << 15); // Enable
        Cap->MsgCtrl &= ~(1 << 14); // Unmask all
    } 
 
    PCI::disable_interrupt((uint64_t)this->phdr);
    kinfo("[NVME: %p]: msi/msix set\n", (uint64_t)this);
    kinfo("[NVME: %p]: enable msi/msix\n", (uint64_t)this);
    return true;
}

/* Drain every phase-matching completion entry from one completion queue. Safe to
   call from the MSI-X handler or by polling during bring-up; when no new entry
   has been posted the phase check ends the loop immediately. */
void NVME::PollCQ(NVME::CmplQue *cmpq){
#define NVME_PHASE(cq) ((*(uint16_t*)(&(cq)->Entries[(cq)->Pos].Status)) & 1)
    while (NVME_PHASE(cmpq) == cmpq->Phase) {
        NVME::CmplQueEntry *entry = &cmpq->Entries[cmpq->Pos++];

        if (cmpq->Pos == cmpq->Size) {
            cmpq->Pos = 0;
            cmpq->Phase ^= 1;
        }

        NVME::SubQue *SQ = this->sq[entry->SubQueIden];
        spinlock_lock(&SQ->Lock);

        // Account for the commands the controller consumed and advance Head.
        uint16_t consumed = (entry->SubQueHdrPtr - SQ->Head + SQ->Size) % SQ->Size;
        SQ->Head = entry->SubQueHdrPtr;
        SQ->Load -= consumed;

        NVME::NVMERequest *req = SQ->Req[entry->CmdIden];
        spinlock_unlock(&SQ->Lock);

        // Copy the completion back and release the waiting thread.
        if (req) {
            __memcpy(&req->res, entry, sizeof(NVME::CmplQueEntry));
            req->done = true;
        }
    }
#undef NVME_PHASE
}

void MSIXHandler(context_t *ctx){
    uint32_t cpuid = this_cpu()->id;
    uint8_t vector = ctx->int_no & 0xFF;

    // Resolve the instance and queue through the global routing table.
    if (vector >= 256 || !nvme_irq_routes[vector].valid) {
        kerror("Spurious NVME interrupt on CPU %d, Vector %d\n", cpuid, vector);
        return;
    }

    NVMECtrl *instance = nvme_irq_routes[vector].instance;
    uint16_t queueId = nvme_irq_routes[vector].queueId;

    NVME::CmplQue *cmpq = instance->cq[queueId];
    instance->PollCQ(cmpq);
    instance->WriteCmplDB(cmpq);
}

bool NVME::InitREQIdent(NVME::NVMERequest *req, u32 tp, u32 nspIden, void *buf){
    NVME::SubQueEntry *entry = &req->input[0];
    entry->OPCode = 0x6;
    entry->NspIdent = nspIden;
    entry->Spec[0] = tp;
    entry->PRP[0] = VMM::GetPhysics(kernel_pagemap, (uint64_t)buf);
    return true;
}
#endif