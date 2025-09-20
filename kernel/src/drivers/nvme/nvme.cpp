#include <drivers/nvme/nvme.h>

#include <arch/x86_64/vmm/vmm.h>
#include <arch/x86_64/pit/pit.h>
#include <arch/x86_64/smp/smp.h>

#if 0

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

    int32_t timeout = 500 * ((cap >> 24) & 0xff);
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
	this->WriteReg(NVME_CTRLREG_INITMC, (1u << host->intrNum) - 1);

	if (hw_nvme_registerQue(host) == res_FAIL) return;

	if (hw_nvme_initNsp(host) == res_FAIL) return;
	
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
	entry->Spec[1] = 0b1 | (((u32)subQue->trg->iden) << 16);
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

	hw_nvme_writeSubDb(host, subQue);

	task_Request_send(&req->req);

	if (req->res.Status != 0x01) {
		kerror("(NVME %p): request %p failed status:%x\n", (uint64_t)this, req, req->res.status);
	}
}

bool NVME::RegisterQue() {
	NVME::NVMERequest *req = this->MakeReq(1);
	for (uint32_t i = 1; i < this->INTRNUM; i++) {
		this->CreateCmplQue(req, this->cq[i]);
		this->Request(this->sq[0], req);
		if (req->res.status != 0x01) {
			kerror("(NVME %p): failed to register io completion queue #%d\n", (uint64_t)this, i);
			return false;
		}

		this->CreateSubQue(req, host->subQue[i]);
		this->Request(host->subQue[0], req);
		if (req->res.status != 0x01) {
			kerror("(NVME %p): failed to register io submission queue #%d\n", (uint64_t)this, i);
			return false;
		}
	}

	kfree(req);
	return true;
}

bool NVME::InitNsp(hw_nvme_Host *host) {
	uint32_t *nspLst = kmalloc(sizeof(uint32_t) * 1024);
	hw_nvme_Nsp *nsp = kmalloc(sizeof(hw_nvme_Nsp), mm_Attr_Shared, NULL);
	_memset(nspLst, 0, sizeof(uint32_t) * 1024);
	hw_nvme_Request *req = hw_nvme_makeReq(1);
	hw_nvme_initReq_identify(req, hw_nvme_Request_Identify_type_NspLst, 0, nspLst);

	hw_nvme_request(host, host->subQue[0], req);

	for (host->devNum = 0; nspLst[host->devNum]; host->devNum++) ;
	
	host->dev = mm_kmalloc(sizeof(hw_nvme_Dev) * host->devNum, mm_Attr_Shared, NULL);

	printk(screen_log, "(NVME %p): nsp num:%d\n", host, host->devNum);

	for (int32_t i = 0; i < host->devNum; i++) {
		hw_nvme_initReq_identify(req, hw_nvme_Request_Identify_type_Nsp, nspLst[i], nsp);

		hw_nvme_request(host, host->subQue[0], req);

		printk(screen_log, "(NVME %p): nsp #%d: nspSz:%ld nspCap:%ld nspUtil:%ld\n", host, nspLst[i], nsp->nspSz, nsp->nspCap, nsp->nspUtil);

		hw_nvme_Dev *dev = &host->dev[i];

		dev->size = nsp->nspUtil;
		dev->host = host;

		dev->nspId = nspLst[i];

		dev->diskDev.device.parent = &host->pci.device;
		dev->diskDev.device.drv = &hw_nvme_devDrv;

		dev->diskDev.device.drv->cfg(&dev->diskDev.device);

		// install device
		hw_DiskDev_install(&dev->diskDev);
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

/*
__always_inline__ uint32_t hw_nvme_read32(hw_nvme_Host *host, uint32_t off) { return hal_read32(this->capRegAddr + off); }

__always_inline__ uint64_t hw_nvme_read64(hw_nvme_Host *host, uint32_t off) { return hal_read64(this->capRegAddr + off); }

__always_inline__ void hw_nvme_write32(hw_nvme_Host *host, uint32_t off, uint32_t val) { hal_write32(this->capRegAddr + off, val); }

__always_inline__ void hw_nvme_write64(hw_nvme_Host *host, uint32_t off, uint64_t val) { hal_write64(this->capRegAddr + off, val); }
*/

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

bool NVME::InitIntr() {

	this->MSI = NULL;
	this->flags &= ~NVME_FLAG_MISIX;

	for (PCI::PCICapHdr *hdr = PCI::GetNxtCap(this->phdr, NULL); hdr != NULL; hdr = PCI::GetNxtCap(this->phdr, hdr)) {
		switch (hdr->CapID) {
			case CAPHDR_CAPID_MSI :
				this->MSI = //container(container(hdr, hw_pci_MsiCap32, hdr), PCI::PCI_MSI_CAP, cap32);
                ((PCI::PCI_MSI_CAP *)(((u64)(((PCI::MSI_CAP32 *)
                (((u64)(hdr))-((u64)&(((PCI::MSI_CAP32 *)0)->Header))))))
                -
                ((u64)&(((PCI::PCI_MSI_CAP *)0)->Cap32))));
				break;
			case CAPHDR_CAPID_MSIX :
				this->MSIX = //container(hdr, hw_pci_MsixCap, hdr);
                ((PCI::PCI_MSIX_CAP *)(((u64)(hdr))-((u64)&(((PCI::PCI_MSIX_CAP *)0)->Header))));
				this->flags |= NVME_FLAG_MISIX;
				break;
		}
		if (this->flags & NVME_FLAG_MISIX) break;
	}
	if (this->MSI == NULL) {
		kerror("[NVME: %p]: no MSI/MSI-X support\n", (uint64_t)this);
		return false;
	}
	
	this->INTRNUM = max(2, min(NVME_MAX_INTRNUM, smp_cpu_count));
	// allocate interrupt descriptor
	this->intr = kmalloc(sizeof(intr_Desc) * this->INTRNUM);
	if (this->intr == NULL) {
		kerror("[NVME: %p]: failed to allocate interrupt descriptor\n", (uint64_t)this);
		return false;
	}

	if (this->flags & NVME_FLAG_MISIX) {
		kinfo("[NVME: %p]: use msix\n", (uint64_t)this);
		
		int32_t vecNum = PCI_MSIX_CAP_VecNum(this->MSIX);
		vecNum = this->INTRNUM = min(vecNum, this->INTRNUM);

		PCI::PCI_MSIX_TABLE *tbl = PCI::GetMSIXTbl(this->MSIX, this->phdr);

		kinfo("[NVME: %p]: msixtbl:%p vecNum:%d msgCtrl:%#010x\n", host, tbl, vecNum, this->MSIX->MsgCtrl);

		for (int32_t i = 0; i < this->INTRNUM; i++)
			hw_pci_initIntr(this->intr + i, hw_nvme_msiHandler, (u64)host | i, "nvme msix");
		if (hw_pci_allocMsix(this->MSIX, this->phdr, this->intr, this->INTRNUM) == false) {
			kerror("[NVME: %p]: failed to allocate msix interrupt\n", (uint64_t)this);
			return false;
		}
	} else {
		kinfo("[NVME: %p]: use msi\n", (uint64_t)this);

		int32_t vecNum = PCI_MSIX_CAP_VecNum(this->MSI);
		this->INTRNUM = vecNum = min(vecNum, this->INTRNUM);

		kinfo("[NVME: %p]: msi: %s vecNum:%d mgsCtrl:%#010x\n", 
				host, hw_pci_MsiCap_is64(this->MSI) ? "64B" : "32B", vecNum, *hw_pci_MsiCap_msgCtrl(this->MSI));

		for (int32_t i = 0; i < vecNum; i++)
			hw_pci_initIntr(this->intr + i, hw_nvme_msiHandler, (u64)host | i, "nvme msi");
		if (hw_pci_allocMsi(this->MSI, this->intr, this->INTRNUM) == false) {
			kerror("(NVME): host %p failed to set msi interrupt\n", (uint64_t)this);
			return false;
		}
	}

	hw_pci_disableIntx(this->phdr);
	kinfo("[NVME: %p]: msi/msix set\n", (uint64_t)this);

	// enable msi/msix
	int32_t res;
	if (this->flags & NVME_FLAG_MISIX) 
		res = hw_pci_enableMsixAll(this->MSIX, this->phdr, this->intr, this->INTRNUM);
	else
		res = hw_pci_enableMsiAll(this->MSI, this->intr, this->INTRNUM);
	if (res == false) {
		kerror("[NVME: %p]: failed to enable msi/msix\n", (uint64_t)this);
		return false;
	}
	kinfo("[NVME: %p]: enable msi/msix\n", (uint64_t)this);
    
	return true;
}

#endif