/*
* SPDX-License-Identifier: GPL-2.0-only
* File: nvme.cpp
* Copyright (C) 2026 Yo-yo-ooo
*
* This file is part of SkylineSystem.
*
* SkylineSystem is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
*/
#ifdef __x86_64__
#include <drivers/nvme/nvme.h>

#include <arch/x86_64/vmm/vmm.h>
#include <arch/x86_64/pit/pit.h>
#include <arch/x86_64/smp/smp.h>
#include <mem/heap.h>


#define ReadReg64(off)      this->ReadReg(off)
#define WriteReg64(off,v)   this->WriteReg(off,v)



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


	WriteReg64(NVME_CTRLREG_ASQ,(this->sq[0]->Entries));
	WriteReg64(NVME_CTRLREG_ACQ,(this->cq[0]->Entries));

	{
		uint32_t ada = (this->sq[0]->Size - 1) | ((this->cq[0]->Size - 1) << 16);
		this->WriteReg(NVME_CTRLREG_AQA, ada);
	}
	
	return true;
}

NVME::NVME(PCI::PCIHeader0 *header){
    this->phdr = header;
    this->BaseAddr = (uint64_t)(((uint64_t)header->BAR1 << 32) | (header->BAR0 & 0xFFFFFFF0));
    //VMM::Map(this->BaseAddr);

    PCI::enable_interrupt(this->BaseAddr);
    PCI::enable_io_space(this->BaseAddr);
    PCI::enable_mem_space(this->BaseAddr);

    this->WriteReg(NVME_CTRLREG_CC,0);
    

    if((this->ReadReg(NVME_CTRLREG_CSTS) & 1) || (this->ReadReg(NVME_CTRLREG_CC) & 1)){
        kerror("NVME: Failed to reset");
        this->FialureNUM = 0;
        return;
    }

    if (this->ReadReg(NVME_CTRLREG_CC) & (1ul << 36)) {
		this->WriteReg(NVME_CTRLREG_NSSR, 0x4e564d65); // 'NVMe'
	}

    this->CAPREG = this->ReadReg(NVME_CTRLREG_CAP);

    uint32_t REG_CFG = ((sizeof(NVME::SubQueEntry) << 20)) |
    ((sizeof(NVME::CmplQueEntry) << 16)) | 
    (((this->CAPREG >> 17) & 0b11) << 11) | (0 << 7);

    this->DBStride = 1 << (2 + ((this->CAPREG >> 32) & 0xful));

    this->WriteReg(NVME_CTRLREG_CC,REG_CFG);

    this->INTRNUM = min(max(2,smp_last_cpu),NVME_MAX_INTRNUM);

    kinfoln("[NVME %p]: capAddr:%p cap:%#018lx version:%x dbStride:%d\n", 
		(uint64_t)this, this->CAPREG, ReadReg64(NVME_CTRLREG_CAP), this->ReadReg(NVME_CTRLREG_VS), this->DBStride);

    if(this->InitQue() == false) {this->FialureNUM = 1;}

    this->WriteReg(NVME_CTRLREG_CC,this->ReadReg(NVME_CTRLREG_CC) | 1);

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
	// mask all interrupts
	this->WriteReg(NVME_CTRLREG_INITMS, ~0u);
	// unmask interrupts
	this->WriteReg(NVME_CTRLREG_INITMC, (1u << this->INTRNUM) - 1);

	if (this->RegisterQue() == false) return;

	if (this->InitNsp() == false) return;
	
	kpok("SUCCESSFILLY INIT NVME ");

}

NVME::NVMERequest *MakeReq(int32_t InputSize){
    NVME::NVMERequest *req = kmalloc(sizeof(NVME::NVMERequest) + InputSize * sizeof(NVME::SubQueEntry));

    if(req == nullptr){
        kerror("(NVME %p) FAILED TO ALLOC REQUEST!,SIZE = %d",InputSize);
        return nullptr;
    }

    _memset(req->input, 0, sizeof(NVME::SubQueEntry) * InputSize);
	req->inputSz = InputSize;
	return req;
}

bool NVME::CreateSubQue(NVME::NVMERequest *req, NVME::SubQue *subQue) {
	NVME::SubQueEntry *entry = &req->input[0];
	entry->OPCode = 0x01;
	entry->PRP[0] = PHYSICAL(subQue->Entries);
	entry->Spec[0] = subQue->Ident | (((u32)subQue->Size - 1) << 16);
	// flag[0] : contiguous
	entry->Spec[1] = 0b1 | (((u32)subQue->Trg->Ident) << 16);
	return true;
}

bool NVME::CreateCmplQue(NVME::NVMERequest *req, NVME::CmplQue *cmplQue) {
	NVME::SubQueEntry *entry = &req->input[0];
	entry->OPCode = 0x05;
	entry->PRP[0] = PHYSICAL(cmplQue->Entries);
	entry->Spec[0] = cmplQue->Ident | ((u32)(cmplQue->Size - 1) << 16);
	// flag[0] : contiguous
	// flag[1] : enable interrupt
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
	// set identifier of each input to the idx in queue
	for (int32_t i = 0; i < req->inputSz; i++) {
		req->input[i].CommandIdent = subQue->Tail;
		subQue->Req[subQue->Tail] = req;
		__memcpy(&req->input[i], subQue->Entries + subQue->Tail, sizeof(NVME::SubQueEntry));
		
		if ((++subQue->Tail) == subQue->Size) subQue->Tail = 0;
	}
	spinlock_unlock(&subQue->Lock);
	return true;
}

void NVME::Request(NVME::SubQue *subQue, NVME::NVMERequest *req){
    while (this->TryInsertRequest(subQue, req) != true) ;

	this->WriteReg(0x1000 + (subQue->Ident << 1) * this->DBStride, subQue->Tail);

	if (req->res.Status != 0x01) {
		kerror("(NVME %p): request %p failed status:%x\n", (uint64_t)this, req, req->res.Status);
	}
}

bool NVME::RegisterQue() {
	NVME::NVMERequest *req = this->MakeReq(1);
	for (uint32_t i = 1; i < this->INTRNUM; i++) {
		this->CreateCmplQue(req, this->cq[i]);
		this->Request(this->sq[0], req);
		if (req->res.Status != 0x01) {
			kerror("(NVME %p): failed to register io completion queue #%d\n", (uint64_t)this, i);
			return false;
		}

		this->CreateSubQue(req, this->sq[i]);
		this->Request(this->sq[0], req);
		if (req->res.Status != 0x01) {
			kerror("(NVME %p): failed to register io submission queue #%d\n", (uint64_t)this, i);
			return false;
		}
	}
    
	kfree(req);
	return true;
}

bool NVME::InitNsp() {
	uint32_t *nspLst = kmalloc(sizeof(uint32_t) * 1024);
    NVME::NameSpace *nsp = kmalloc(sizeof(NVME::NameSpace));
	_memset(nspLst, 0, sizeof(uint32_t) * 1024);
	NVME::NVMERequest *req = this->MakeReq(1);
    NVME::InitREQIdent(req, REQ_IDENTIFY_TYPE_NSPLST, 0, nspLst);

	NVME::Request(this->sq[0], req);

	for (this->devNum = 0; nspLst[this->devNum]; this->devNum++) ;
	
	this->dev = kmalloc(sizeof(NVME::NVMEDev) * this->devNum);

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
	//VMM::Map((uint64_t)nvme_reg);
	return *nvme_reg;
}


void NVME::WriteReg(uint32_t offset, uint32_t value){
    volatile uint32_t *nvme_reg = (volatile uint32_t *)(this->BaseAddr + offset);
	//VMM::Map((uint64_t)nvme_reg);
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
	NVME::SubQue *que = (void *)(entry + size);
	que->Entries = entry;
	que->Ident = iden;

	que->Tail = que->Head = 0;
	que->Size = size;
	que->Load = 0;

	que->Trg = trg;

	//que->Lock.locked = false;
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
	NVME::CmplQue *que = (void *)(entry + size);
	que->Entries = entry;
	que->Ident = iden;
	que->Phase = 1;

	que->Pos = 0;
	que->Size = size;

	return que;
}

extern volatile uint64_t smp_cpu_count;

bool NVME::InitIntr() {

	this->MSI = nullptr;
	this->flags &= ~NVME_FLAG_MISIX;


    this->MSI = PCI::GetMSICap(this->phdr);
    this->MSIX = PCI::GetMSIXCap(this->phdr);
	if (this->MSI == nullptr || this->MSIX == nullptr) {
		kerror("[NVME: %p]: no MSI/MSI-X support\n", (uint64_t)this);
		return false;
	}else if(this->MSIX != nullptr){this->flags |= NVME_FLAG_MISIX;}


	
	this->INTRNUM = max(2, min(NVME_MAX_INTRNUM, smp_cpu_count));
	
	if (this->flags & NVME_FLAG_MISIX) {
		kinfo("[NVME: %p]: use msix\n", (uint64_t)this);
		
		int32_t vecNum = PCI_MSIX_CAP_VecNum(this->MSIX);
		vecNum = this->INTRNUM = min(vecNum, this->INTRNUM);

        PCI::PCI_MSIX_CAP *Cap = PCI::GetMSIXCap(this->phdr);
        PCI::PCI_MSIX_TABLE* Tbl = PCI::GetMSIXTblBaseAddr(this->phdr,Cap);
		for (int32_t i = 0; i < this->INTRNUM; i++){
            // 1. 决定这个队列由哪个 CPU 核心处理 (负载均衡)
            // 假设你有 n 个 CPU，这里可以用简单的轮询 (Round Robin)
            uint32_t targetCpu = i % smp_cpu_count; //ToDo: lw!!!

            // 2. 分配一个唯一的 IDT 向量号 (通常从 0x20 开始，避开异常区)
            // 在你的架构里，不同 CPU 的同一个向量号可以挂不同的 Handler
            uint16_t vector = 0x32 + i; 

            uint32_t MsgAddr = 0xfee00000u
                | ((targetCpu) << 12)
                | (0 << 3) | (0 << 2);                   
            Tbl[i].msgAddr = MsgAddr;
            Tbl[i].msgData = i;
            __asm__ volatile ("mfence" ::: "memory"); // 确保硬件看到地址和数据
            Tbl[i].vecCtrl &= ~1u; // 解除ENTRY[INTRNUM]'s Mask 
            idt_install_irq_cpu(targetCpu,vector,nullptr/*TODO:NVME HANDLER!!!*/);
        }
        PCI::enable_bus_mastering((uint64_t)this->phdr);
        Cap->MsgCtrl |= (1 << 15); // Enable
        Cap->MsgCtrl &= ~(1 << 14); // Unmask all
	} 
 
	PCI::disable_interrupt(this->phdr);
	kinfo("[NVME: %p]: msi/msix set\n", (uint64_t)this);
	kinfo("[NVME: %p]: enable msi/msix\n", (uint64_t)this);

	return true;
}


bool NVME::InitREQIdent(NVME::NVMERequest *req, u32 tp, u32 nspIden, void *buf){
    NVME::SubQueEntry *entry = &req->input[0];
    entry->OPCode = 0x6;
	entry->NspIdent = nspIden;
	entry->Spec[0] = tp;
	entry->PRP[0] = VMM::GetPhysics(kernel_pagemap,buf);
	return true;
}
#endif