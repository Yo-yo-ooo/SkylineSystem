#include <arch/x86_64/dev/pci/pci.h>
#include <arch/x86_64/smp/smp.h>
#include <arch/x86_64/lapic/lapic.h>
#include <arch/x86_64/interrupt/idt.h>
#include <klib/kio.h>

static u16 *MSICAP_MSGDATA(PCI::PCI_MSI_CAP *cap) {
    return PCI_MSI_CAP_IS64(cap) ? &cap->Cap64.MsgData : &cap->Cap32.MsgData;
}
namespace PCI
{
    void SetMsgData16(uint16_t *msgData, uint32_t vec, 
        uint32_t deliverMode, uint32_t level, uint32_t triggerMode) {
        *msgData = vec | (deliverMode << 8) | (level << 14) | (triggerMode << 15);
    }

    void SetMsgData32(uint32_t *msgData, uint32_t vec, 
        uint32_t deliverMode, uint32_t level, uint32_t triggerMode) {
        *msgData = vec | (deliverMode << 8) | (level << 14) | (triggerMode << 15);
    }
    namespace MSIX
    {
        void SetMsgAddr(uint64_t *msgAddr, uint32_t cpuId, uint32_t redirect, uint32_t destMode) {
            *msgAddr = 0xfee00000u
                | ((smp_cpu_list[cpuId]->id) << 12)
                | (redirect << 3) | (destMode << 2);
        }

        bool Enable(PCI::PCI_MSIX_CAP *cap, PCI::PCIHeader0 *hdr, uint64_t INTRIDX){
            /* if (desc->ctrl != NULL && desc->ctrl->enable != NULL) {
                if (desc->ctrl->enable(desc) == false) 
                    return false;
            } */
            PCI::PCI_MSIX_TABLE *tbl =  PCI::GetMSIXTbl(cap, hdr);
            bit_set0_32(&tbl[INTRIDX].vecCtrl, 0);
            return true;
        }

        bool EnableAll(PCI::PCI_MSIX_CAP *cap, PCI::PCIHeader0 *hdr, uint64_t INTRNUM) {
            PCI::PCI_MSIX_TABLE *tbl = PCI::GetMSIXTbl(cap, hdr);
            for (int i = 0; i < INTRNUM; i++) 
                if ((tbl[i].vecCtrl & 1) && 
                    PCI::MSIX::Enable(cap, hdr, i) == false) 
                        return false;
            bit_set1_16(&cap->MsgCtrl, 15);
            bit_set0_16(&cap->MsgCtrl, 14);
            return true;
        }
    } // namespace MSIX
    
    namespace MSI{

        void SetMsgAddr(PCI::PCI_MSI_CAP *cap, uint32_t cpuId, uint32_t redirect, uint32_t destMode) {
            uint32_t val = 0xfee00000u
                | ((smp_cpu_list[cpuId]->id) << 12)
                | (redirect << 3) | (destMode << 2);
            if (PCI_MSI_CAP_IS64(cap)) cap->Cap64.MsgAddr = val;
            else cap->Cap32.MsgAddr = val;
        }    


    }

    void GetIntrGate(uint8_t vecID, uint32_t cpuID, uint64_t intrNum) {
        for (int32_t i = 0; i < intrNum; i++) {
            idt_install_irq(vecID + i, nullptr);
        }
    } 

    bool SetMsi(PCI::PCI_MSI_CAP *cap, uint8_t vecID, uint32_t cpuID,uint64_t intrNum) {
        PCI::MSI::SetMsgAddr(cap, cpuID, 0, APIC_DestMode_Physical);
        PCI::SetMsgData16(MSICAP_MSGDATA(cap), vecID, APIC_DeliveryMode_Fixed, APIC_Level_Deassert, APIC_TriggerMode_Edge);

        //hal_hw_pci_getIntrGate(desc, intrNum);
        return true;
    }

    bool SetMsix(PCI::PCI_MSIX_CAP *cap, PCI::PCIHeader0 *cfg, uint8_t vecID, uint32_t cpuID,uint64_t intrNum) {
        PCI::PCI_MSIX_TABLE *tbl = PCI::GetMSIXTbl(cap, cfg);
        //VMM::Map((tbl));
        for (int32_t i = 0; i < intrNum; i++) {
            PCI::MSIX::SetMsgAddr(&tbl[i].msgAddr, cpuID, 0, APIC_DestMode_Physical);
            PCI::SetMsgData32(&tbl[i].msgData, vecID, APIC_DeliveryMode_Fixed, APIC_Level_Deassert, APIC_TriggerMode_Edge);
        }
        //hal_hw_pci_getIntrGate(desc, intrNum);
        return true;
        
    } // namespace pci
}
