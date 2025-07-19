#include <drivers/nvme/nvme.h>

#include <arch/x86_64/vmm/vmm.h>
#include <arch/x86_64/pit/pit.h>
#include <arch/x86_64/smp/smp.h>

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
    VMM::Map(this->BaseAddr);

    PCI::enable_interrupt(this->BaseAddr);
    PCI::enable_io_space(this->BaseAddr);
    PCI::enable_mem_space(this->BaseAddr);

    this->WriteReg(NVME_CTRLREG_CC,0);

    

    if((this->ReadReg(NVME_CTRLREG_CSTS) & 1) || (this->ReadReg(NVME_CTRLREG_CC) & 1)){
        kerror("NVME: Failed to reset");
        this->FialureNUM = 0;
        //return *this;
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

    this->INTRNUM = min(max(2,smp_cpu_count),NVME_MAX_INTRNUM);

    kinfoln("[NVME %p]: capAddr:%p cap:%#018lx version:%x dbStride:%d\n", 
		(uint64_t)this, this->CAPREG, ReadReg64(NVME_CTRLREG_CAP), this->ReadReg(NVME_CTRLREG_VS), this->DBStride);

    if(this->InitQue() == false) {this->FialureNUM = 1;}

}

uint32_t NVME::ReadReg(uint32_t offset){
    volatile uint32_t *nvme_reg = (volatile uint32_t *)(this->BaseAddr + offset);
	VMM::Map((uint64_t)nvme_reg);
	return *nvme_reg;
}


void NVME::WriteReg(uint32_t offset, uint32_t value){
    volatile uint32_t *nvme_reg = (volatile uint32_t *)(this->BaseAddr + offset);
	VMM::Map((uint64_t)nvme_reg);
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
    kmalloc(sizeof(NVME::SubQue) + size * sizeof(NVME::SubQueEntry) + size * sizeof(NVME::Request *));
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

	que->Lock.locked = false;
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
    /*
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
			kerror("hw: nvme: host %p failed to set msi interrupt\n", (uint64_t)this);
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
    */
	return true;
}
