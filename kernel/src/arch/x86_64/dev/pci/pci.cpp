#include "pci.h"
#include "../../../../klib/kio.h"
#include "../../../../acpi/acpi.h"
#include "../../vmm/vmm.h"
#include "../../../../mem/heap.h"

extern void *__memcpy(void *d, const void *s, size_t n);
namespace PCI{

    pci_device pci_list[128];
    i8 pci_list_idx = 0;
    uint64_t offset = 0;
    uint64_t busAddress = 0;
    uint64_t deviceAddress = 0;
    uint64_t functionAddress = 0;

    uint8_t ReadByte(uint8_t bus, uint8_t slot,uint8_t func, uint32_t offset)
    {
        uint32_t id = ((bus) << 16) | ((slot) << 11) | ((func) << 8);
        uint32_t addr = 0x80000000 | id | (offset & 0xfc);
        outl(0xCF8, addr);
        return inb(0xCFC + (offset & 0x03));
    }

    u16 ReadWord(u8 bus, u8 slot, u8 func, u8 offset) {
        u32 addr;
        u32 lbus = (u32)bus;
        u32 lslot = (u32)slot;
        u32 lfunc = (u32)func;
        
        addr = (u32)((lbus << 16) | (lslot << 11) | (lfunc << 8) | (offset & 0xFC) |
                ((u32)0x80000000));
        
        outl(0xCF8, addr);

        return (u16)((inl(0xCFC) >> ((offset & 2) * 8)) & 0xFFFF);
    }

    void WriteByte(u8 bus, u8 dev, u8 func, u8 offset, u8 value) {
        uint32_t shift = ((offset & 3) * 8);
        uint32_t val = 0x80000000 |
            (bus << 16) |
            (dev << 11) |
            (func << 8) |
            (offset & 0xFC);
        outd(0xCF8, val);
        outd(0xCFC, (ind(0xCFC) & ~(0xFF << shift)) | (value << shift));
    }

        // 'offset' is the byte offset from 0x00
    void WriteWord(u8 bus, u8 dev, u8 func, u8 offset, u16 value) {
        if ((offset & 3) <= 2) {
            uint32_t shift = ((offset & 3) * 8);
            const uint32_t val = 0x80000000 |
            (bus << 16) |
            (dev << 11) |
            (func << 8) |
            (offset & 0xFC);
            outd(0xCF8, val);
            outd(0xCFC, (ind(0xCFC) & ~(0xFFFF << shift)) | (value << shift));
        } else {
            PCI::WriteByte(bus, dev, func, offset + 0, (u8) (value & 0xFF));
            PCI::WriteByte(bus, dev, func, offset + 1, (u8) (value >> 8));
        }
    }


    void WriteDword(u8 bus, u8 slot, u8 func, u8 offset, u32 value) {
        u32 addr;
        u32 lbus = (u32)bus;
        u32 lslot = (u32)slot;
        u32 lfunc = (u32)func;
        
        addr = (u32)((lbus << 16) | (lslot << 11) | (lfunc << 8) | (offset & 0xFC) |
                ((u32)0x80000000));
        
        outl(0xCF8, addr);
        outl(0xCFC, value);
    }

    u32 ReadDword(u8 bus, u8 slot, u8 func, u8 offset) {
        return ((u32)PCI::ReadWord(bus, slot, func, offset + 2) << 16) | PCI::ReadWord(bus, slot, func, offset);
    }


    void AddDevice(u8 bus, u8 func, u8 Class, u8 subclass, 
    u16 device_id, u16 vendor_id, u32* bars,u8 slot,
    u64 busAddress,u64 deviceAddress,u64 functionAddress) {
        pci_list[pci_list_idx].bus = bus;
        pci_list[pci_list_idx].function = func;
        pci_list[pci_list_idx].Class = Class;
        pci_list[pci_list_idx].subclass = subclass;
        pci_list[pci_list_idx].device_id = device_id;
        pci_list[pci_list_idx].vendor_id = vendor_id;
        pci_list[pci_list_idx].slot = slot;
        pci_list[pci_list_idx].busAddress = busAddress;
        pci_list[pci_list_idx].deviceAddress = deviceAddress;
        pci_list[pci_list_idx].functionAddress = functionAddress;
        pci_list[pci_list_idx].Prog_IF = ReadWord(bus, device_id, func, 0x8) & 0xFF00;
        pci_list[pci_list_idx].Revision_ID = ReadWord(bus, device_id, func, 0x8) & 0x00FF;
        __memcpy(pci_list[pci_list_idx].bars, bars, sizeof(u32) * 6);
        pci_list_idx++;
    }

    i8 FindDevice(u8 Class, u8 subclass) {
        for (i8 i = 0; i < pci_list_idx; i++) {
            if (pci_list[i].Class == Class && pci_list[i].subclass == subclass)
            return i;
        }
        return -1;
    }

    i8 FindDevice_(u16 vendor_id, u16 device_id) {
        for (i8 i = 0; i < pci_list_idx; i++) {
            if (pci_list[i].vendor_id == vendor_id && pci_list[i].device_id == device_id)
            return i;
        }
        return -1;
    }

    i8 FindDevice__(u8 Class, u8 subclass, u8 progif) {
        for (i8 i = 0; i < pci_list_idx; i++) {
            if (pci_list[i].Class == Class && 
                pci_list[i].subclass == subclass &&
                pci_list[i].Prog_IF == progif)
            return i;
        }
        return -1;
    }

    PCIDeviceHeader* _FindDevice_(uint16_t vendor_id, uint16_t device_id){
        return (PCIDeviceHeader*)pci_list[FindDevice_(vendor_id, device_id)].functionAddress;
    }

    PCIDeviceHeader* _FindDevice__(u8 Class, u8 subclass){
        return (PCIDeviceHeader*)pci_list[FindDevice(Class, subclass)].functionAddress;
    }
    
    PCIDeviceHeader* _FindDevice___(u8 Class, u8 subclass, u8 progif){
        return (PCIDeviceHeader*)pci_list[FindDevice__(Class, subclass,progif)].functionAddress;
    }

    
    

    void Init() {
        AddToStack();
        u16 vendor, device;
        u16 class_subclass;
        u8 Class, subclass;
        u32 bars[6];
        PCIDeviceHeader* pciDeviceHeader;
        //ACPI::MCFGHeader* mcfg = (ACPI::MCFGHeader*)ACPI::FindTable("MCFG");
        //int entries = (mcfg->Header.Length - sizeof(ACPI::MCFGHeader)) / sizeof(ACPI::DeviceConfig);
        int entries = (ACPI::mcfg->Header.Length - sizeof(ACPI::MCFGHeader)) / sizeof(ACPI::DeviceConfig);
        kinfo("PCI: Found %d entries\n", entries);
        for(int t = 0;t < entries; t++){
            ACPI::DeviceConfig* newDeviceConfig = (ACPI::DeviceConfig*)((uint64_t)ACPI::mcfg + 
            sizeof(ACPI::MCFGHeader) + sizeof(ACPI::DeviceConfig) * t);
            for (u64 bus = newDeviceConfig->StartBus; bus < newDeviceConfig->EndBus; bus++){
                offset = bus << 20;
                busAddress = newDeviceConfig->BaseAddress + offset;
                if(busAddress == 0) continue;
                VMM::Map((void*)busAddress, (void*)busAddress);
                pciDeviceHeader  = (PCIDeviceHeader*)busAddress;
                if (pciDeviceHeader ->Device_ID == 0x0000) {continue;}
                if (pciDeviceHeader ->Device_ID == 0xFFFF) {continue;}
                for (u8 slot = 0; slot < PCI_MAX_SLOT; slot++){
                    offset = (slot << 15);
                    deviceAddress = busAddress + offset;
                    if(deviceAddress == 0) continue;
                    VMM::Map((void*)deviceAddress, (void*)deviceAddress);
                    pciDeviceHeader  = (PCIDeviceHeader*)deviceAddress;
                    if (pciDeviceHeader ->Device_ID == 0x0000) {continue;}
                    if (pciDeviceHeader ->Device_ID == 0xFFFF) {continue;}
                    for (u8 func = 0; func < PCI_MAX_FUNC; func++) {
                        offset = (func << 12);
                        functionAddress = deviceAddress + offset;
                        if(functionAddress == 0) continue;
                        VMM::Map((void*)functionAddress, (void*)functionAddress);
                        pciDeviceHeader  = (PCIDeviceHeader*)functionAddress;
                        if (pciDeviceHeader ->Device_ID == 0x0000) {continue;}
                        if (pciDeviceHeader ->Device_ID == 0xFFFF) {continue;}
                        vendor = PCI::ReadWord(bus, slot, func, 0);
                        device = PCI::ReadWord(bus, slot, func, 2);
                        class_subclass = PCI::ReadWord(bus, slot, func, 0x8 + 2);
                        Class = (u8)class_subclass;
                        subclass = (u8)((class_subclass & 0xFF00) >> 8);
                        for (int i = 0; i < 6; i++) {
                            bars[i] = PCI::ReadDword(bus, slot, func, PCI_BAR0 + (sizeof(u32) * i));
                        }
                        PCI::PrintDevMessage(vendor,device,Class,subclass,(PCI::ReadDword(bus,slot,func,0x08) >> 8) & 0xFF);
                        PCI::AddDevice(bus, func, Class, subclass,
                         device, vendor, bars, slot, busAddress, deviceAddress, functionAddress);
                    }
                }
            }
        }
        RemoveFromStack();
    }
}