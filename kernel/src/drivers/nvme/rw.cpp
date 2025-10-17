#include <drivers/nvme/nvme.h>

#include <arch/x86_64/vmm/vmm.h>
#include <arch/x86_64/pit/pit.h>
#include <arch/x86_64/smp/smp.h>
#include <mem/heap.h>
#include <arch/x86_64/schedule/sched.h>



int32_t NVME::GetSpareReq(NVME::NVMEDev *dev){
    int32_t idx = -1;
	while (idx == -1) {
		spinlock_lock(&dev->lck);
		// find a spare request
		if (dev->reqBitmap != ~0x0ul) {
			for (int i = 0; i < 64; i++) if (~dev->reqBitmap & (1ul << i)) {
				bit_set1(&dev->reqBitmap, idx = i);
				break;
			}
		}
		spinlock_unlock(&dev->lck);
		Schedule::Yield();
	}
	return idx;
}
NVME::SubQue *NVME::FindIOSubQue(){
    NVME::SubQue *que = this->sq[1];
    for(int32_t i = 2;i < this->INTRNUM;i++)
        if(this->sq[i]->Load < que->Load)
            que = this->sq[i];
    return que;
}

uint64_t NVME::Read(uint64_t offset, uint64_t size, void *buf) {
    int32_t reqIdx = this->GetSpareReq(this->dev);
    NVME::NVMERequest *req = this->dev->reqs[reqIdx];
    NVME::SubQueEntry *entry = &req->input[0];
	NVME::SubQue *subQue;

	// make request and send
	uint64_t res = 0;
	_memset(entry, 0, sizeof(NVME::SubQueEntry));
	entry->OPCode = 0x02;
	entry->NspIdent = this->dev->nspId;
	while (size != res) {
		entry->PRP[0] = VMM::GetPhysics(kernel_pagemap,buf) + res * 512;
		*(uint64_t *)&entry->Spec[0] = offset + res;
		uint32_t blkSz = min(size - res, (1ul << 16));
		entry->Spec[2] = blkSz - 1;

		// find a io submission queue & submit this request
		subQue = this->FindIOSubQue();
		this->Request(subQue, req);
		if (req->res.Status != 0x01) {
			kerrorln("hw: nvme: %p: device %p: failed to read blk: %ld~%ld", 
				(uint64_t)this, (uint64_t)this->dev, 
				offset + res, offset + res + blkSz - 1);
			break;
		}
		res += blkSz;
	}

	// release request
	//_releaseReq(nvmeDev, reqIdx);
    bit_set0(&this->dev->reqBitmap, reqIdx);
	return res;
}

uint64_t NVME::Write(uint64_t offset, uint64_t size, void *buf) {
	int32_t reqIdx = this->GetSpareReq(this->dev);
    NVME::NVMERequest *req = this->dev->reqs[reqIdx];
    NVME::SubQueEntry *entry = &req->input[0];
	NVME::SubQue *subQue;

	// make request and send
	uint64_t res = 0;
	_memset(entry, 0, sizeof(NVME::SubQueEntry));
	
    entry->OPCode = 0x01;
	entry->NspIdent = this->dev->nspId;
	while (size != res) {
		entry->PRP[0] = VMM::GetPhysics(kernel_pagemap,buf) + res * 512;
		*(uint64_t *)&entry->Spec[0] = offset + res;
		uint32_t blkSz = min(size - res, (1ul << 16));
		entry->Spec[2] = blkSz - 1;

		// find a io submission queue & submit this request
		subQue = this->FindIOSubQue();
		this->Request(subQue, req);
		if (req->res.Status != 0x01) {
			kerrorln("hw: nvme: %p: device %p: failed to read blk: %ld~%ld\n", 
				(uint64_t)this, (uint64_t)this->dev, 
				offset + res, offset + res + blkSz - 1);
			break;
		}
		res += blkSz;
	}

	// release request
    bit_set0(&this->dev->reqBitmap, reqIdx);
	return res;
}