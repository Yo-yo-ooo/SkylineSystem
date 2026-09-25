//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
#ifdef __x86_64__
#include <drivers/nvme/nvme.h>
#include <arch/x86_64/vmm/vmm.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/smp/smp.h>
#include <mem/heap.h>

#define NVME_OP_READ            0x02
#define NVME_OP_WRITE           0x01
#define NVME_CHUNK_PAGES        8    // DMA bounce chunk = 8 pages (32 KiB)
#define NVME_MAX_PRP_PAGES      511  // PRP0 + one 510-entry PRP list

int32_t NVME::GetSpareReq(NVMEDev *dev){
    spinlock_lock(&dev->lck);
    for (int32_t i = 0; i < 64; i++) {
        if (!(dev->reqBitmap & (1ULL << i))) {
            dev->reqBitmap |= (1ULL << i);
            spinlock_unlock(&dev->lck);
            return i;
        }
    }
    spinlock_unlock(&dev->lck);
    return -1;
}

NVME::SubQue *NVME::FindIOSubQue(){
    // IO submission queues are numbered 1..INTRNUM-1; sq[0] is admin-only and
    // must never carry NVM Read/Write commands.
    uint32_t nIO = this->INTRNUM - 1;
    uint32_t idx = 1 + (this_cpu()->id % nIO);
    return this->sq[idx];
}

/* Fill PRP[0..1] for a page-aligned, physically contiguous kernel buffer.
   Returns the PRP-list kernel VA (caller frees it), nullptr if no list is
   needed, or (void*)-1 on failure. */
void* NVME::BuildPRP(NVME::SubQueEntry* e, void* buf, uint32_t nbytes){
    uint32_t npages = (nbytes + 0xFFFu) >> 12;
    uint64_t va = (uint64_t)buf;
    e->PRP[0] = VMM::GetPhysics(kernel_pagemap, va);
    if (npages == 1)
        return nullptr;
    if (npages == 2) {
        e->PRP[1] = VMM::GetPhysics(kernel_pagemap, va + 0x1000);
        return nullptr;
    }
    if (npages > NVME_MAX_PRP_PAGES)
        return (void*)-1;
    // Three or more pages: a single PRP-list page describes page1..pageN.
    uint64_t *list = (uint64_t*)VMM::Alloc((pagemap_t*)kernel_pagemap, 1, false);
    if (!list) return (void*)-1;
    for (uint32_t i = 1; i < npages; i++)
        list[i - 1] = VMM::GetPhysics(kernel_pagemap, va + (uint64_t)i * 0x1000);
    e->PRP[1] = VMM::GetPhysics(kernel_pagemap, (uint64_t)list);
    return list;
}

uint64_t NVME::Read(uint64_t offset, uint64_t size, void* buf){
    if (!this->dev || size == 0) return 0;
    NVMEDev *d = &this->dev[0];
    uint32_t lbaBytes = d->lbaBytes ? d->lbaBytes : 512;

    // Dev/PartitionManager addresses storage in 512-byte sectors; map that to
    // the controller's formatted LBA geometry.
    uint64_t startByte = offset * 512;
    uint64_t totalBytes = size * 512;
    if ((startByte % lbaBytes) != 0 || (totalBytes % lbaBytes) != 0) return 0;

    uint8_t *out = (uint8_t*)buf;
    uint64_t done = 0;

    // Page-aligned, physically contiguous bounce buffer for DMA.
    uint32_t chunkPg = NVME_CHUNK_PAGES;
    void *bounce = VMM::Alloc((pagemap_t*)kernel_pagemap, chunkPg, false);
    if (!bounce) { chunkPg = 1; bounce = VMM::Alloc((pagemap_t*)kernel_pagemap, 1, false); }
    if (!bounce) return 0;

    while (done < totalBytes) {
        uint64_t thisBytes = min((uint64_t)chunkPg * 0x1000, totalBytes - done);
        uint32_t nlb = (uint32_t)(thisBytes / lbaBytes);
        uint64_t slba = (startByte + done) / lbaBytes;

        int32_t ri = GetSpareReq(d);
        if (ri < 0) break;
        NVMERequest *req = d->reqs[ri];
        SubQueEntry *e = &req->input[0];
        _memset(e, 0, sizeof(*e));
        e->OPCode = NVME_OP_READ;
        e->NspIdent = (uint32_t)d->nspId;
        e->Spec[0] = (uint32_t)slba;         // CDW10: starting LBA low
        e->Spec[1] = (uint32_t)(slba >> 32); // CDW11: starting LBA high
        e->Spec[2] = (nlb - 1) & 0xFFFF;     // CDW12: NLB, zero-based
        void *list = BuildPRP(e, bounce, nlb * lbaBytes);
        bool fail = (list == (void*)-1);

        if (!fail) {
            SubQue *q = FindIOSubQue();
            Request(q, req);
            if (list) VMM::Free((pagemap_t*)kernel_pagemap, list);
            fail = !nvme_status_ok(req->res.Status);
        }
        d->reqBitmap &= ~(1ULL << ri);
        if (fail) break;

        __memcpy(out + done, bounce, thisBytes);
        done += thisBytes;
    }

    VMM::Free((pagemap_t*)kernel_pagemap, bounce);
    return done / 512; // completed 512-byte sectors
}

uint64_t NVME::Write(uint64_t offset, uint64_t size, void* buf){
    if (!this->dev || size == 0) return 0;
    NVMEDev *d = &this->dev[0];
    uint32_t lbaBytes = d->lbaBytes ? d->lbaBytes : 512;

    uint64_t startByte = offset * 512;
    uint64_t totalBytes = size * 512;
    if ((startByte % lbaBytes) != 0 || (totalBytes % lbaBytes) != 0) return 0;

    uint8_t *in = (uint8_t*)buf;
    uint64_t done = 0;

    uint32_t chunkPg = NVME_CHUNK_PAGES;
    void *bounce = VMM::Alloc((pagemap_t*)kernel_pagemap, chunkPg, false);
    if (!bounce) { chunkPg = 1; bounce = VMM::Alloc((pagemap_t*)kernel_pagemap, 1, false); }
    if (!bounce) return 0;

    while (done < totalBytes) {
        uint64_t thisBytes = min((uint64_t)chunkPg * 0x1000, totalBytes - done);
        uint32_t nlb = (uint32_t)(thisBytes / lbaBytes);
        uint64_t slba = (startByte + done) / lbaBytes;

        __memcpy(bounce, in + done, thisBytes);

        int32_t ri = GetSpareReq(d);
        if (ri < 0) break;
        NVMERequest *req = d->reqs[ri];
        SubQueEntry *e = &req->input[0];
        _memset(e, 0, sizeof(*e));
        e->OPCode = NVME_OP_WRITE;
        e->NspIdent = (uint32_t)d->nspId;
        e->Spec[0] = (uint32_t)slba;         // CDW10: starting LBA low
        e->Spec[1] = (uint32_t)(slba >> 32); // CDW11: starting LBA high
        e->Spec[2] = (nlb - 1) & 0xFFFF;     // CDW12: NLB, zero-based
        void *list = BuildPRP(e, bounce, nlb * lbaBytes);
        bool fail = (list == (void*)-1);

        if (!fail) {
            SubQue *q = FindIOSubQue();
            Request(q, req);
            if (list) VMM::Free((pagemap_t*)kernel_pagemap, list);
            fail = !nvme_status_ok(req->res.Status);
        }
        d->reqBitmap &= ~(1ULL << ri);
        if (fail) break;

        done += thisBytes;
    }

    VMM::Free((pagemap_t*)kernel_pagemap, bounce);
    return done / 512; // completed 512-byte sectors
}
#endif
