//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
#ifdef __x86_64__
#include <drivers/nvme/nvme.h>
#include <arch/x86_64/vmm/vmm.h>
#include <arch/x86_64/pit/pit.h>
#include <arch/x86_64/smp/smp.h>
#include <mem/heap.h>

#define ReadReg64(off)      this->ReadReg(off)
#define WriteReg64(off,v)   this->WriteReg(off,v)

// 全局中断路由表：用于在中断上下文中通过向量号快速查找 NVMe 实例和队列
struct NVMEIrqRoute {
    NVME* instance;
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
    WriteReg64(NVME_CTRLREG_ASQ, VMM::GetPhysics(kernel_pagemap, this->sq[0]->Entries));
    WriteReg64(NVME_CTRLREG_ACQ, VMM::GetPhysics(kernel_pagemap, this->cq[0]->Entries));

    {
        uint32_t ada = (this->sq[0]->Size - 1) | ((this->cq[0]->Size - 1) << 16);
        this->WriteReg(NVME_CTRLREG_AQA, ada);
    }
    
    return true;
}

NVME::NVME(PCI::PCIHeader0 *header){
    this->phdr = header;
    this->BaseAddr = (uint64_t)(((uint64_t)header->BAR1 << 32) | (header->BAR0 & 0xFFFFFFF0));

    // 配置空间操作应该传入 PCI 头指针
    PCI::enable_interrupt((uint64_t)this->phdr);
    PCI::enable_io_space((uint64_t)this->phdr);
    PCI::enable_mem_space((uint64_t)this->phdr);

    this->WriteReg(NVME_CTRLREG_CC, 0);
    if((this->ReadReg(NVME_CTRLREG_CSTS) & 1) || (this->ReadReg(NVME_CTRLREG_CC) & 1)){
        kerror("NVME: Failed to reset");
        this->FialureNUM = 0;
        return;
    }

    if (this->ReadReg(NVME_CTRLREG_CC) & (1ul << 36)) {
        this->WriteReg(NVME_CTRLREG_NSSR, 0x4e564d65); // 'NVMe'
    }

    this->CAPREG = this->ReadReg(NVME_CTRLREG_CAP);

    // IOCQES 和 IOSQES 必须是以 2 为底的幂次方值，不能直接写 sizeof
    uint32_t iosqes = __builtin_ctz(sizeof(NVME::SubQueEntry)); 
    uint32_t iocqes = __builtin_ctz(sizeof(NVME::CmplQueEntry));
    uint32_t REG_CFG = (iosqes << 20) | (iocqes << 16) | 
                       (((this->CAPREG >> 17) & 0b11) << 11) | (0 << 7);

    this->DBStride = 1 << (2 + ((this->CAPREG >> 32) & 0xful));
    this->WriteReg(NVME_CTRLREG_CC, REG_CFG);

    this->INTRNUM = min(max(2, smp_last_cpu), NVME_MAX_INTRNUM);

    kinfoln("[NVME %p]: capAddr:%p cap:%#018lx version:%x dbStride:%d\n", 
        (uint64_t)this, this->CAPREG, ReadReg64(NVME_CTRLREG_CAP), this->ReadReg(NVME_CTRLREG_VS), this->DBStride);

    if(this->InitQue() == false) {this->FialureNUM = 1;}

    this->WriteReg(NVME_CTRLREG_CC, this->ReadReg(NVME_CTRLREG_CC) | 1);

    int32_t timeout = 500 * ((this->CAPREG >> 24) & 0xff);
    while (timeout > 30) {
        if ((~this->ReadReg(NVME_CTRLREG_CSTS) & 1) || (~this->ReadReg(NVME_CTRLREG_CC) & 1)) {
            timeout--;
            PIT::Sleep(1);
        } else break;
    }
    if ((this->ReadReg(NVME_CTRLREG_CSTS) & 1) && (this->ReadReg(NVME_CTRLREG_CC) & 1)) {
        kpok("(NVME) %p: succ to enable, status=%08x cfg=%08x\n", 
            (uint64_t)this, this->ReadReg(NVME_CTRLREG_CSTS), this->ReadReg(NVME_CTRLREG_CC));
    } else {
        kerror("(NVME %p): failed to enable, status=%08x cfg=%08x\n", 
            (uint64_t)this, this->ReadReg(NVME_CTRLREG_CSTS), this->ReadReg(NVME_CTRLREG_CC));
        return;
    }
    
    this->WriteReg(NVME_CTRLREG_INITMS, ~0u);
    this->WriteReg(NVME_CTRLREG_INITMC, (1u << this->INTRNUM) - 1);

    if (this->RegisterQue() == false) return;
    if (this->InitNsp() == false) return;
    
    kpok("SUCCESSFILLY INIT NVME ");
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
    entry->PRP[0] = VMM::GetPhysics(kernel_pagemap, subQue->Entries);
    entry->Spec[0] = subQue->Ident | (((u32)subQue->Size - 1) << 16);
    entry->Spec[1] = 0b1 | (((u32)subQue->Trg->Ident) << 16);
    return true;
}

bool NVME::CreateCmplQue(NVME::NVMERequest *req, NVME::CmplQue *cmplQue) {
    NVME::SubQueEntry *entry = &req->input[0];
    entry->OPCode = 0x05;
    entry->PRP[0] = VMM::GetPhysics(kernel_pagemap, cmplQue->Entries);
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

    // 必须同步等待硬件完成。中断处理函数会将其置为 true
    while (!req->done) {
        asm volatile ("pause"); // 实际内核中应调用 schedule() 让出 CPU
    }

    // 检查硬件返回的状态码 (0 表示成功)
    if ((req->res.Status & 0xFF) != 0x00) {
        kerror("(NVME %p): request %p failed status:%x\n", (uint64_t)this, req, req->res.Status);
    }
}

bool NVME::RegisterQue() {
    NVME::NVMERequest *req = this->MakeReq(1);
    for (uint32_t i = 1; i < this->INTRNUM; i++) {
        this->CreateCmplQue(req, this->cq[i]);
        this->Request(this->sq[0], req);
        if ((req->res.Status & 0xFF) != 0x00) {
            kerror("(NVME %p): failed to register io completion queue #%d\n", (uint64_t)this, i);
            return false;
        }

        this->CreateSubQue(req, this->sq[i]);
        this->Request(this->sq[0], req);
        if ((req->res.Status & 0xFF) != 0x00) {
            kerror("(NVME %p): failed to register io submission queue #%d\n", (uint64_t)this, i);
            return false;
        }
    }
    kfree(req);
    return true;
}

bool NVME::InitNsp() {
    uint32_t *nspLst = (uint32_t*)kmalloc(sizeof(uint32_t) * 1024);
    NVME::NameSpace *nsp = (NVME::NameSpace*)kmalloc(sizeof(NVME::NameSpace));
    _memset(nspLst, 0, sizeof(uint32_t) * 1024);
    NVME::NVMERequest *req = this->MakeReq(1);
    NVME::InitREQIdent(req, REQ_IDENTIFY_TYPE_NSPLST, 0, nspLst);

    NVME::Request(this->sq[0], req);

    for (this->devNum = 0; nspLst[this->devNum]; this->devNum++) ;
    
    this->dev = (NVME::NVMEDev*)kmalloc(sizeof(NVME::NVMEDev) * this->devNum);
    kinfo("(NVME %p): nsp num:%d\n", this, this->devNum);

    for (int32_t i = 0; i < this->devNum; i++) {
        NVME::InitREQIdent(req, REQ_IDENTIFY_TYPE_NSPLST, nspLst[i], nsp);
        NVME::Request(this->sq[0], req);

        kinfo("(NVME %p): nsp #%d: nspSz:%ld nspCap:%ld nspUtil:%ld\n", this, 
            nspLst[i], nsp->nspSz, nsp->nspCap, nsp->nspUtil);
        NVMEDev *dev = &this->dev[i];
    }

    kfree(req);
    kfree(nspLst);
    kfree(nsp);
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
    NVME::SubQueEntry *entry = (NVME::SubQueEntry *)
    kmalloc(sizeof(NVME::SubQue) + size * sizeof(NVME::SubQueEntry) + size * sizeof(NVME::NVMERequest *));
    if (entry == nullptr) {
        kerror("[NVME %p]: failed to allocate submission queue size=%d\n", (uint64_t)this, size);
        return nullptr;
    }
    _memset(entry, 0, sizeof(NVME::SubQueEntry) * size);
    NVME::SubQue *que = (NVME::SubQue *)(entry + size);
    que->Entries = entry;
    que->Ident = iden;
    que->Tail = que->Head = 0;
    que->Size = size;
    que->Load = 0;
    que->Trg = trg;
    return que;
}

NVME::CmplQue *NVME::AllocCmplQue(uint32_t iden, uint32_t size) {
    NVME::CmplQueEntry *entry = (NVME::CmplQueEntry *)
    kmalloc(sizeof(NVME::CmplQue) + size * sizeof(NVME::CmplQueEntry));
    if (entry == NULL) {
        kerror("[NVME %p]: failed to allocate completion queue size=%d\n", (uint64_t)this, size);
        return NULL;
    }
    _memset(entry, 0, sizeof(NVME::CmplQueEntry) * size);
    NVME::CmplQue *que = (NVME::CmplQue *)(entry + size);
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
        PCI::PCI_MSIX_TABLE* Tbl = PCI::GetMSIXTblBaseAddr(this->phdr, Cap);
        
        for (int32_t i = 0; i < this->INTRNUM; i++){
            uint32_t targetCpu = GetLWIntrCpu(); // 获取绑定的 CPU
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
            
            idt_install_irq_cpu(targetCpu, vector, MSIXHandler);
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

void MSIXHandler(context_t *ctx){
    uint32_t cpuid = this_cpu()->id; 
    uint8_t vector = ctx->int_no & 0xFF;

    // 通过全局表安全查找实例
    if (vector >= 256 || !nvme_irq_routes[vector].valid) {
        kerror("Spurious NVME interrupt on CPU %d, Vector %d\n", cpuid, vector);
        return; 
    }

    NVME *instance = nvme_irq_routes[vector].instance;
    uint16_t queueId = nvme_irq_routes[vector].queueId;

    NVME::CmplQue *cmpq = instance->cq[queueId];
    NVME::SubQue *sq = instance->sq[queueId];

#define PHASETag ((*(uint16_t*)(&cmpq->Entries[cmpq->Pos].Status)) & 1)
    while(PHASETag == cmpq->Phase){
        NVME::CmplQueEntry *entry = &cmpq->Entries[cmpq->Pos++];

        if(cmpq->Pos == cmpq->Size){
            cmpq->Pos = 0;
            cmpq->Phase ^= 1;
        }

        NVME::SubQue *SQ = instance->sq[entry->SubQueIden];
        spinlock_lock(&SQ->Lock);
        
        // 正确计算硬件消费的命令数，并更新 Head 指针
        uint16_t consumed = (entry->SubQueHdrPtr - SQ->Head + SQ->Size) % SQ->Size;
        SQ->Head = entry->SubQueHdrPtr;
        SQ->Load -= consumed;
        
        NVME::NVMERequest *req = SQ->Req[entry->CmdIden];
        spinlock_unlock(&SQ->Lock);

        // 将结果拷贝回请求体，并唤醒等待的线程
        if (req) {
            __memcpy(&req->res, entry, sizeof(NVME::CmplQueEntry));
            req->done = true; 
        }
    }
    instance->WriteCmplDB(cmpq);
}

bool NVME::InitREQIdent(NVME::NVMERequest *req, u32 tp, u32 nspIden, void *buf){
    NVME::SubQueEntry *entry = &req->input[0];
    entry->OPCode = 0x6;
    entry->NspIdent = nspIden;
    entry->Spec[0] = tp;
    entry->PRP[0] = VMM::GetPhysics(kernel_pagemap, buf);
    return true;
}
#endif