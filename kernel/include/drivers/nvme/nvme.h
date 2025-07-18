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


class NVME{

public:
    PACK(typedef struct SubQueEntry{
        uint8_t OPCode;
        uint8_t Attr;

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
        u16 Size, Pos;
        u16 Phase, Ident;

        CmplQueEntry *Entries;
    } CmplQue;

    typedef struct Request {
        //task_Request req;
        int inputSz;
        NVME::CmplQueEntry res;
        NVME::SubQueEntry input[0];
    } Request;

    typedef struct SubQue {
        u16 Size, Load;
        // since that controller can not guarantee the process order of command in ring,
        // we need to update the load by checking whether the head of queue has been handled.
        u16 Tail, Head;

        u16 Ident;

        atomic_lock Lock;
        
        NVME::SubQueEntry *Entries;
        NVME::CmplQue *Trg;

        NVME::Request *Req[0];
    } SubQue;


    NVME(PCI::PCIHeader0 *header);

    NVME::SubQue *AllocSubQue(u32 iden, u32 size, NVME::CmplQue *trg);
    NVME::CmplQue *AllocCmplQue(u32 iden, u32 size);

    uint32_t ReadReg(uint32_t offset);
    void WriteReg(uint32_t offset, uint32_t value);
    
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


	IRQDesc *intr;

    

    /*
    */
   uint8_t FialureNUM;
private:
    bool InitQue(); // Just for NVME::NVME(*p)
    bool InitIntr();
};
#endif

#endif