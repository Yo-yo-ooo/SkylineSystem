#include <arch/x86_64/dev/pci/pci.h>
#include <arch/x86_64/smp/smp.h>
#include <arch/x86_64/lapic/lapic.h>
#include <arch/x86_64/interrupt/idt.h>

#if 0
u16 *hw_pci_MsiCap_msgData(PCI::PCI_MSI_CAP *cap) {
    return PCI_MSI_CAP_IS64(cap) ? &cap->Cap64.MsgData : &cap->Cap32.MsgData;
}
namespace PCI
{
    void SetMsgData16(uint16_t *msgData, uint32_t vec, uint32_t deliverMode, uint32_t level, uint32_t triggerMode) {
        *msgData = vec | (deliverMode << 8) | (level << 14) | (triggerMode << 15);
    }

    void SetMsgData32(uint32_t *msgData, uint32_t vec, uint32_t deliverMode, uint32_t level, uint32_t triggerMode) {
        *msgData = vec | (deliverMode << 8) | (level << 14) | (triggerMode << 15);
    }
    namespace MSIX
    {
        void SetMsgAddr(uint64_t *msgAddr, uint32_t cpuId, uint32_t redirect, uint32_t destMode) {
            *msgAddr = 0xfee00000u
                | ((smp_cpu_list[cpuId]->lapic_id) << 12)
                | (redirect << 3) | (destMode << 2);
        }
    } // namespace MSIX
    
    namespace MSI{


        void SetMsgAddr(PCI::PCI_MSI_CAP *cap, uint32_t cpuId, uint32_t redirect, uint32_t destMode) {
            register uint32_t val = 0xfee00000u
                | ((smp_cpu_list[cpuId]->lapic_id) << 12)
                | (redirect << 3) | (destMode << 2);
            if (PCI_MSI_CAP_IS64(cap)) cap->Cap64.MsgAddr = val;
            else cap->Cap32.MsgAddr = val;
        }    

    }
/*
    void GetIntrGate(IRQDesc *desc, u64 intrNum) {
        for (int32_t i = 0; i < intrNum; i++) {
            hal_intr_setIntrGate(smp_cpu_list[desc[i].CpuId]., desc[i].VecId, 0, hal_hw_pci_intrLst[desc[i].vecId - 0x40]);
        }
    }
*/
    bool SetMsi(PCI::PCI_MSI_CAP *cap, IRQDesc *desc, uint64_t intrNum) {
        PCI::MSI::SetMsgAddr(cap, desc->CpuId, 0, APIC_DestMode_Physical);
        PCI::SetMsgData16(hw_pci_MsiCap_msgData(cap), desc->VecId, APIC_DeliveryMode_Fixed, APIC_Level_Deassert, APIC_TriggerMode_Edge);

        //hal_hw_pci_getIntrGate(desc, intrNum);
        return true;
    }

    bool SetMsix(PCI::PCI_MSIX_CAP *cap, PCI::PCIHeader0 *cfg, IRQDesc *desc, uint64_t intrNum) {
        PCI::PCI_MSIX_TABLE *tbl = PCI::GetMSIXTbl(cap, cfg);
        VMM::Map((tbl));
        for (int32_t i = 0; i < intrNum; i++) {
            PCI::MSIX::SetMsgAddr(&tbl[i].msgAddr, desc[i].CpuId, 0, APIC_DestMode_Physical);
            PCI::SetMsgData32(&tbl[i].msgData, desc[i].VecId, APIC_DeliveryMode_Fixed, APIC_Level_Deassert, APIC_TriggerMode_Edge);
        }
        //hal_hw_pci_getIntrGate(desc, intrNum);
        return true;
        
    } // namespace pci
}
#endif