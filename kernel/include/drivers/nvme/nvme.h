#pragma once

#ifndef _NVME_H_
#define _NVME_H_

#include <stdint.h>
#include <stddef.h>
#include <pdef.h>
/**/
#ifdef __x86_64__


#include <arch/x86_64/dev/pci/pci.h>
#include <klib/klib.h>
#include <arch/x86_64/interrupt/idt.h>

#define REQ_IDENTIFY_TYPE_NSP		0x0
#define REQ_IDENTIFY_TYPE_CTRL		0x1
#define REQ_IDENTIFY_TYPE_NSPLST	0x2
//nvme controller register offset
#define NVME_CTRLREG_CAP            0x0
#define NVME_CTRLREG_VS             0x8
#define NVME_CTRLREG_INITMS         0xC
#define NVME_CTRLREG_INITMC         0x10
#define NVME_CTRLREG_CC             0x14
#define NVME_CTRLREG_CSTS           0x1C
#define NVME_CTRLREG_NSSR           0x20

#define NVME_CTRLREG_AQA            0x24
#define NVME_CTRLREG_ASQ            0x28
#define NVME_CTRLREG_ACQ            0x30

#define NVME_MAX_INTRNUM            4

#define NVME_FLAG_MISIX             0x1

#define NVME_SubQueSz	(4096 / sizeof(NVME::SubQueEntry))
#define NVME_CmplQueSz	(4096 / sizeof(NVME::CmplQueEntry))

namespace INFile_NVME{
PACK(typedef struct _lbaformat_t_{
            // metadata size 
            uint16_t metaSz;
            // lba data size
            uint8_t lbaDtSz;
            // relative performance
            uint8_t rp : 2;
            uint32_t reserved : 6;
}) _lbaformat_t_;
}
class NVME{

public:
    PACK(typedef struct hw_nvme_Nsp {
        uint64_t nspSz;
        uint64_t nspCap;
        uint64_t nspUtil;
        uint8_t nspFeature;
        uint8_t numOfLbaFormat;
        // formatted lba size
        uint8_t formattedLbaSz;
        // metadata capabilities
        uint8_t metaCap;
        // end-to-end data protection capabilites
        uint8_t dtProtectCap;
        // end-to-end data protection tp settings
        uint8_t dtProtectTpSet;

        // unecessary properties
        // namespace multi-path i/o and namespace sharing capabilities
        uint8_t nmic;
        // reservation capability
        uint8_t rescap;
        // format progress indicator
        uint8_t fpi;
        // dealocate logical block features
        uint8_t dlfeat;
        // namespace atomic write unit normal
        uint16_t nawun;
        // namespace atomic write unit power fail
        uint16_t nawupf;
        // namespace atomic compare & write unit
        uint16_t nacwu;
        // namespace atomic boundary size normal
        uint16_t nabsn;
        // namespace atomic boundary offset
        uint16_t nabo;
        // namespace atomic boundary size power fail
        uint16_t nabspf;
        // namespace optimal i/o boundary
        uint16_t noiob;
        
        // NVM capacity
        uint64_t nvmcap[2];

        // namespace preferred write granularity
        uint16_t npwg;
        // namespace preferred write alignment
        uint16_t npwa;
        // namespace preferred dealocate granularity
        uint16_t npdg;
        // namespace preferred deallocate alignment
        uint16_t npda;
        // namespace optimal write size
        uint16_t nows;
        // maximum single source range length (used for Copy command)
        uint16_t mssrl;
        // maximum copy length
        uint32_t mcl;
        // maximum source range count
        uint8_t msrc;
        // key per i/o status
        uint8_t kpios;
        // number of unique attribute lba formats
        uint8_t nulbaf;
        
        uint8_t reserved;
        // key pre i/o data access alignment and granularity
        uint32_t kpiodaag;
        
        uint32_t reserved2;
        // anagroup identifier
        uint32_t anagrpid;
        
        uint8_t reserved3[3];
        
        // namespace attributes
        uint8_t nsattr;
        // NVM set identifier
        uint16_t nvmsetid;
        // endurance group identifier
        uint16_t endgid;
        // namespace globally unique identifier
        uint64_t nguid[2];
        // ieee extended unique identifier
        uint64_t eui64;
        // support lba format, this structure use bit field to reduce function (doge)
        INFile_NVME::_lbaformat_t_ lbaFormat[64];
        uint8_t vendorSpec[PAGE_SIZE - 384];
    }) NameSpace;

    PACK(typedef struct SubQueEntry{
        uint8_t OPCode;
        uint8_t Attr;
        uint16_t CommandIdent;

        uint32_t NspIdent;

        uint32_t DW2;
        uint32_t DW3;

        union {
            uint32_t MetaPtr32[2];
            uint64_t MetaPtr;
	    };

        // dword 6 ~ 7
        // data pointer(DPTR)
        union {
            uint32_t DT_Ptr32[4];
            uint64_t PRP[2];
            struct {

            } sgl;
        };
	
        // dwords 10 ~ dwords 15
        uint32_t Spec[6];
    })SubQueEntry;

    PACK(typedef struct CmplQueEntry{
        uint32_t Spec[2];
        uint16_t SubQueHdrPtr;
        uint16_t SubQueIden;
        uint16_t CmdIden;
        uint16_t Status;
    })CmplQueEntry;

    typedef struct CmplQue {
        uint16_t Size, Pos;
        uint16_t Phase, Ident;

        CmplQueEntry *Entries;
    } CmplQue;

    typedef struct NVMERequest {
        //task_Request req;
        int32_t inputSz;
        NVME::CmplQueEntry res;
        NVME::SubQueEntry input[0];
    } NVMERequest;

    typedef struct SubQue {
        uint16_t Size, Load;
        // since that controller can not guarantee the process order of command in ring,
        // we need to update the load by checking whether the head of queue has been handled.
        uint16_t Tail, Head;

        uint16_t Ident;

        spinlock_t Lock;
        
        NVME::SubQueEntry *Entries;
        NVME::CmplQue *Trg;

        NVME::NVMERequest *Req[0];
    } SubQue;

    typedef struct NVMEDev {
        int32_t nspId;
        uint64_t size;

        uint64_t *host;


        spinlock_t lck;
        uint64_t reqBitmap;
        NVME::NVMERequest *reqs[64]; 
    } NVMEDev;


    NVME(PCI::PCIHeader0 *header);

    void Request(NVME::SubQue *subQue, NVME::NVMERequest *req);
    
    NVME::NVMERequest *MakeReq(int32_t InputSize);
    bool TryInsertRequest(NVME::SubQue *subQue, NVME::NVMERequest *req);

    bool RegisterQue();
    NVME::SubQue *AllocSubQue(uint32_t iden, uint32_t size, NVME::CmplQue *trg);
    bool CreateSubQue(NVME::NVMERequest *req, NVME::SubQue *subQue);
    NVME::CmplQue *AllocCmplQue(uint32_t iden, uint32_t size);
    bool CreateCmplQue(NVME::NVMERequest *req, NVME::CmplQue *cmplQue);

    uint32_t ReadReg(uint32_t offset);
    void WriteReg(uint32_t offset, uint32_t value);

    bool InitREQIdent(NVME::NVMERequest *req, u32 tp, u32 nspIden, void *buf);
    
    uint64_t Read( uint64_t offset, uint64_t size, void *buf);
    uint64_t Write(uint64_t offset, uint64_t size, void *buf);

protected:
    uint64_t BaseAddr;

    uint64_t CAPREG;
    
    uint32_t DBStride;
    uint16_t INTRNUM;

    NVME::CmplQue **cq;
    NVME::SubQue **sq;

    PCI::PCIHeader0 *phdr;

    PCI::PCI_MSI_CAP *MSI;
    PCI::PCI_MSIX_CAP *MSIX;


	//IRQDesc *intr;
    int32_t devNum;

    NVMEDev *dev;
    

    /*
    */
   uint8_t FialureNUM;
private:

    int32_t GetSpareReq(NVME::NVMEDev *dev);
    NVME::SubQue *FindIOSubQue();
    bool InitQue(); // Just for NVME::NVME(*p)
    bool InitIntr();
    bool InitNsp();
};
#endif

#endif
