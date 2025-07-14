#include <arch/x86_64/dev/pci/pci.h>
#include <stddef.h>
#include <stdint.h>
#include <klib/klib.h>
#include <klib/kio.h>
#include <klib/kprintf.h>
#include <klib/cstr.h>
#include <acpi/acpi.h>
#include <arch/x86_64/vmm/vmm.h>
#include <conf.h>

PCI::PCIDeviceHeader *pciDevices[128];
u8 pciDeviceidx;
namespace PCI
{

    uint32_t read_pci0(uint32_t bus, uint32_t dev, uint32_t function,uint8_t registeroffset){
        uint32_t id = 1U << 31 | ((bus & 0xff) << 16) | ((dev & 0x1f) << 11) |
                      ((function & 0x07) << 8) | (registeroffset & 0xfc);
        io_out32(PCI_COMMAND_PORT, id);
        uint32_t result = io_in32(PCI_DATA_PORT);
        return result >> (8 * (registeroffset % 4));
    }

    void DoPCIWithoutMCFG(){
        uint32_t BUS, Equipment, F,PCI_NUM = 0;
        for (BUS = 0; BUS < 256; BUS++) {
            for (Equipment = 0; Equipment < 32; Equipment++) {
                for (F = 0; F < 8; F++) {
                    //pci_config0(BUS,F,Equipment,0);
                    unsigned int cmd = 0;
                    cmd = 0x80000000 + (uint32_t) 0 + ((uint32_t) F << 8) +
                        ((uint32_t) Equipment << 11) + ((uint32_t) BUS << 16);
                    // cmd = cmd | 0x01;
                    __asm__ volatile("outl %0, %1" : : "a"(PCI_COMMAND_PORT), "Nd"(cmd));
                    if(io_in32(PCI_DATA_PORT) != 0xFFFFFFFF){
                        PCI_NUM++;
                        //load_pci_device(BUS,Equipment,F);
                        uint32_t value_c = PCI::read_pci0(BUS, Equipment, F, PCI_CONF_REVISION);
                        uint32_t class_code = value_c >> 8;

                        uint16_t value_v = PCI::read_pci0(BUS, Equipment, F, PCI_CONF_VENDOR);
                        uint16_t value_d = PCI::read_pci0(BUS, Equipment, F, PCI_CONF_DEVICE);
                        uint16_t vendor_id = value_v & 0xffff;
                        uint16_t device_id = value_d & 0xffff;

                        /*
                        pci_device_t device = malloc(sizeof(struct pci_device));
                        device->name = pci_classname(class_code);
                        device->vendor_id = vendor_id;
                        device->device_id = device_id;
                        device->class_code = class_code;
                        device->bus = BUS;
                        device->slot = Equipment;
                        device->func = F;
                        */
                        PCIDeviceHeader* device = (PCIDeviceHeader*)kmalloc(sizeof(PCIDeviceHeader));
                        device->Vendor_ID = vendor_id;
                        device->Device_ID = device_id;
                        device->Class = class_code;
                        device->Prog_IF = io_in8(PCI_PROG_IF);
                        device->SubClass = io_in8(PCI_SUBCLASS);

                        if (pciDeviceidx > PCI_DEVICE_MAX) {
                            kwarn("PCI:add device full %d", pciDeviceidx);
                            return;
                        }
                        pciDevices[pciDeviceidx++] = device;

                        if(device->Class == 0x060400 || (device->Class & 0xFFFF00) == 0x060400){
                            return;
                        }
                    }
                }
            }
        }
        kinfo("PCI device loaded: %d", PCI_NUM);
    }

    void EnumeratePCI(ACPI::MCFGHeader* mcfg)
    {
        AddToStack();
        int entries = (mcfg->Header.Length - sizeof(ACPI::MCFGHeader)) / sizeof(ACPI::DeviceConfig);
        RemoveFromStack();
        
        AddToStack();

        _memset(pciDevices, 0, sizeof(PCI::PCIDeviceHeader) * 128);
        pciDeviceidx = 0;

        for (int t = 0; t < entries; t++)
        {
            ACPI::DeviceConfig* newDeviceConfig = (ACPI::DeviceConfig*)((uint64_t)mcfg + sizeof(ACPI::MCFGHeader) + sizeof(ACPI::DeviceConfig) * t);
            for (uint64_t bus = newDeviceConfig->StartBus; bus < newDeviceConfig->EndBus; bus++)
                EnumerateBus(newDeviceConfig->BaseAddress, bus);
        }
        RemoveFromStack();
    }

    void EnumerateBus(uint64_t baseAddress, uint64_t bus)
    {
        AddToStack();
        uint64_t offset = bus << 20;
        uint64_t busAddress = baseAddress + offset;
        RemoveFromStack();

        AddToStack();
        if (busAddress == 0)
        {
            RemoveFromStack();
            return;
        }

        VMM::Map((void*)busAddress, (void*)busAddress);
        
        PCIDeviceHeader* pciDeviceHeader  = (PCIDeviceHeader*)busAddress;

        if (pciDeviceHeader ->Device_ID == 0x0000) {RemoveFromStack(); return;}
        if (pciDeviceHeader ->Device_ID == 0xFFFF) {RemoveFromStack(); return;}

        for (uint64_t device = 0; device < 32; device++)
        {
            EnumerateDevice(busAddress, device);
        }
        RemoveFromStack();
    }

    void EnumerateDevice(uint64_t busAddress, uint64_t device) // Slot
    {
        AddToStack();
        uint64_t offset = device << 15;

        uint64_t deviceAddress = busAddress + offset;

        if (deviceAddress == 0)
        {
            RemoveFromStack();
            return;
        }

        VMM::Map((void*)deviceAddress, (void*)deviceAddress);
        
        PCIDeviceHeader* pciDeviceHeader  = (PCIDeviceHeader*)deviceAddress;

        if (pciDeviceHeader ->Device_ID == 0x0000) {RemoveFromStack(); return;}
        if (pciDeviceHeader ->Device_ID == 0xFFFF) {RemoveFromStack(); return;}

        for (uint64_t function = 0; function < 8; function++)
        {
            EnumerateFunction(deviceAddress, function);
        }
        RemoveFromStack();
    }

    void EnumerateFunction(uint64_t deviceAddress, uint64_t function)
    {
        AddToStack();
        uint64_t offset = function << 12;

        uint64_t functionAddress = deviceAddress + offset;

        if (functionAddress == 0)
        {
            RemoveFromStack();
            return;
        }

        VMM::Map((void*)functionAddress, (void*)functionAddress);
        
        PCIDeviceHeader* pciDeviceHeader  = (PCIDeviceHeader*)functionAddress;

        if (pciDeviceHeader ->Device_ID == 0x0000) {RemoveFromStack(); return;}
        if (pciDeviceHeader ->Device_ID == 0xFFFF) {RemoveFromStack(); return;}

        //BasicRenderer* renderer = osData.debugTerminalWindow->renderer;
        kprintf("%s"," > ");

        {
            const char* vendorName = GetVendorName(pciDeviceHeader->Vendor_ID);
            if (vendorName != unknownString)
                kprintf("%s",vendorName);
            else
            {
                kprintf("%s","<");
                kprintf("%s",ConvertHexToString(pciDeviceHeader->Vendor_ID));
                kprintf("%s",">");
            }
            kprintf("%s"," / ");
        }

        {
            const char* deviceName = GetDeviceName(pciDeviceHeader->Vendor_ID, pciDeviceHeader->Device_ID);
            if (deviceName != unknownString)
                kprintf("%s",deviceName);
            else
            {
                kprintf("%s","<");
                kprintf("%s",ConvertHexToString(pciDeviceHeader->Device_ID));
                kprintf("%s",">");
            }
            kprintf("%s"," / ");
        }

        {
            const char* className = GetClassName(pciDeviceHeader->Class);
            if (className != unknownString)
                kprintf("%s",className);
            else
            {
                kprintf("%s","<");
                kprintf("%s",ConvertHexToString(pciDeviceHeader->Class));
                kprintf("%s",">");
            }
            kprintf("%s"," / ");
        }

        {
            const char* subclassName = GetSubclassName(pciDeviceHeader->Class, pciDeviceHeader->SubClass);
            if (subclassName != unknownString)
                kprintf("%s",subclassName);
            else
            {
                kprintf("%s","<");
                kprintf("%s",ConvertHexToString(pciDeviceHeader->SubClass));
                kprintf("%s",">");
            }
            kprintf("%s"," / ");
        }

        {
            const char* progIFName = GetProgIFName(pciDeviceHeader->Class, pciDeviceHeader->SubClass, pciDeviceHeader->Prog_IF);
            if (progIFName != unknownString)
                kprintf("%s",progIFName);
            else
            {
                kprintf("%s","<");
                kprintf("%s",ConvertHexToString(pciDeviceHeader->Prog_IF));
                kprintf("%s",">");
            }
            //renderer->Print(" / ");
        }
        kprintf("\n");

        pciDevices[pciDeviceidx] = pciDeviceHeader;
        pciDeviceidx++;

        RemoveFromStack();
    }

    PCI::PCIDeviceHeader* FindPCIDev(u8 Class,u8 SubClass,u8 ProgIF){
        for (u8 i = 0; i < PCI_LIST_MAX; i++)
            if(pciDevices[i]->Class = Class 
            && pciDevices[i]->SubClass == SubClass
            && pciDevices[i]->Prog_IF == ProgIF)
                return pciDevices[i];
        
    }

    PCI::PCIDeviceHeader* FindPCIDev(u8 Class,u8 SubClass){
        for (u8 i = 0; i < PCI_LIST_MAX; i++)
            if(pciDevices[i]->Class = Class 
            && pciDevices[i]->SubClass == SubClass)
                return pciDevices[i];
    }


    IOAddress get_address(PCIDeviceHeader* hdr, uint8_t field)
    {
        uint64_t addr = (uint64_t)hdr;
        return get_address(addr, field);
    }

    IOAddress get_address(uint64_t addr, uint8_t field)
    {
        IOAddress address;
        address.attrs.field = field;
        address.attrs.function = (addr >> 12) & 0b111; // 3 bit
        address.attrs.slot = (addr >> 15) & 0b11111; // 5 bit
        address.attrs.bus = (addr >> 20) & 0b11111111; // 8 bit
        address.attrs.enable = true;
        //0x80000000 
        return address;
    }

	uint8_t io_read_byte(uint64_t address, uint8_t field) {
		outl(PCI_ADDRESS_PORT, get_address(address, field).value);
		return inb(PCI_DATA_PORT + (field & 3));
	}

	uint16_t io_read_word(uint64_t address, uint8_t field){
		outl(PCI_ADDRESS_PORT, get_address(address, field).value);
		return inw(PCI_DATA_PORT + (field & 2));
	}

	uint32_t io_read_dword(uint64_t address, uint8_t field) {
	    outl(PCI_ADDRESS_PORT, get_address(address, field).value);
		return inl(PCI_DATA_PORT);
	}

	void io_write_byte(uint64_t address, uint8_t field, uint8_t value) {
		outl(PCI_ADDRESS_PORT, get_address(address, field).value);
		outb(PCI_DATA_PORT + (field & 3), value);
	}

	void io_write_word(uint64_t address, uint8_t field, uint16_t value) {
		outl(PCI_ADDRESS_PORT, get_address(address, field).value);
		outw(PCI_DATA_PORT + (field & 2), value);
	}

	void io_write_dword(uint64_t address, uint8_t field, uint32_t value) {
		outl(PCI_ADDRESS_PORT, get_address(address, field).value);
		outl(PCI_DATA_PORT, value);
	}

	void enable_interrupt(uint64_t address) {
		Command comm = {.value = io_read_word(address, PCI_COMMAND)};
		comm.attrs.interrupt_disable = false;
		io_write_word(address, PCI_COMMAND, comm.value);
	}

	void disable_interrupt(uint64_t address) {
		Command comm = {.value = io_read_word(address, PCI_COMMAND)};
		comm.attrs.interrupt_disable = true;
		io_write_word(address, PCI_COMMAND, comm.value);
	}

	void enable_bus_mastering(uint64_t address) {
		Command comm = {.value = io_read_word(address, PCI_COMMAND)};
		comm.attrs.bus_master = true;
		io_write_word(address, PCI_COMMAND, comm.value);
	}

	void disable_bus_mastering(uint64_t address) {
		Command comm = {.value = io_read_word(address, PCI_COMMAND)};
		comm.attrs.bus_master = false;
		io_write_word(address, PCI_COMMAND, comm.value);
	}

    void enable_io_space(uint64_t address)
    {
        Command comm = {.value = io_read_word(address, PCI_COMMAND)};
        comm.attrs.io_space = true;
        io_write_word(address, PCI_COMMAND, comm.value);
    }
	void disable_io_space(uint64_t address)
    {
        Command comm = {.value = io_read_word(address, PCI_COMMAND)};
        comm.attrs.io_space = false;
        io_write_word(address, PCI_COMMAND, comm.value);
    }
    void enable_mem_space(uint64_t address)
    {
        Command comm = {.value = io_read_word(address, PCI_COMMAND)};
        comm.attrs.mem_space = true;
        io_write_word(address, PCI_COMMAND, comm.value);
    }
	void disable_mem_space(uint64_t address)
    {
        Command comm = {.value = io_read_word(address, PCI_COMMAND)};
        comm.attrs.mem_space = false;
        io_write_word(address, PCI_COMMAND, comm.value);
    }

    void pci_read_bar(uint32_t* mask, uint64_t addr, uint32_t offset)
    {
        uint32_t data = io_read_dword(addr, offset);
        io_write_dword(addr, offset, 0xffffffff);
        *mask = io_read_dword(addr, offset);
        io_write_dword(addr, offset, data);
    }

    PCI_BAR_TYPE pci_get_bar(uint32_t* bar0, int bar_num, uint64_t addr)
    {
        PCI_BAR_TYPE bar;
        bar.io_address = 0;
        bar.size = 0;
        bar.mem_address = 0;
        bar.type = NONE;

        uint32_t* bar_ptr = bar0 + bar_num;

        if (*bar_ptr == 0) 
        {
            bar.type = NONE;
            return bar;
        }

        uint32_t mask;
        pci_read_bar(&mask, addr, bar_num * sizeof(uint32_t));

        if (*bar_ptr & 0x04) 
        { //64-bit mmio
            bar.type = MMIO64;

            uint32_t* bar_ptr_high = bar0 + bar_num + 4;
            uint32_t mask_high;
            pci_read_bar(&mask_high, addr, (bar_num * sizeof(uint32_t)) + 0x4);

            bar.mem_address = ((uint64_t) (*bar_ptr_high & ~0xf) << 32) | (*bar_ptr & ~0xf);
            bar.size = (((uint64_t) mask_high << 32) | (mask & ~0xf)) + 1;
        } 
        else if (*bar_ptr & 0x01) 
        { //IO
            bar.type = IO;

            bar.io_address = (uint16_t)(*bar_ptr & ~0x3);
            bar.size = (uint16_t)(~(mask & ~0x3) + 1);
        } 
        else 
        { //32-bit mmio
            bar.type = MMIO32;

            bar.mem_address = (uint64_t) *bar_ptr & ~0xf;
            bar.size = ~(mask & ~0xf) + 1;
        }
        

        return bar;
    }

    PCI_BAR_TYPE pci_get_bar(PCIHeader0* addr, int bar_num)
    {
        return pci_get_bar(&(addr->BAR0), bar_num, (uint64_t)addr);
    }

    uint8_t read_byte(uint64_t address, PCI_BAR_TYPE type, uint8_t field)
    {
        if (type.type == PCI_BAR_TYPE_ENUM::MMIO32 || type.type == PCI_BAR_TYPE_ENUM::MMIO64)
            return *(uint8_t*)(type.mem_address + field);
        else if (type.type == PCI_BAR_TYPE_ENUM::IO)
            return inb(type.io_address + field);
        return 0;
    }

	uint16_t read_word(uint64_t address, PCI_BAR_TYPE type, uint8_t field)
    {
        if (type.type == PCI_BAR_TYPE_ENUM::MMIO32 || type.type == PCI_BAR_TYPE_ENUM::MMIO64)
            return *(uint16_t*)(type.mem_address + field);
        else if (type.type == PCI_BAR_TYPE_ENUM::IO)
            return inw(type.io_address + field);
        return 0;
    }

	uint32_t read_dword(uint64_t address, PCI_BAR_TYPE type, uint8_t field)
    {
        if (type.type == PCI_BAR_TYPE_ENUM::MMIO32 || type.type == PCI_BAR_TYPE_ENUM::MMIO64)
            return *(uint32_t*)(type.mem_address + field);
        else if (type.type == PCI_BAR_TYPE_ENUM::IO)
            return inl(type.io_address + field);
        return 0;
    }

	void write_byte(uint64_t address, PCI_BAR_TYPE type, uint16_t field, uint8_t value)
    {
        if (type.type == PCI_BAR_TYPE_ENUM::MMIO32 || type.type == PCI_BAR_TYPE_ENUM::MMIO64)
            *(uint8_t*)(type.mem_address + field) = value;
        else if (type.type == PCI_BAR_TYPE_ENUM::IO)
            outb(type.io_address + field, value);
    }

	void write_word(uint64_t address, PCI_BAR_TYPE type, uint16_t field, uint16_t value)
    {
        if (type.type == PCI_BAR_TYPE_ENUM::MMIO32 || type.type == PCI_BAR_TYPE_ENUM::MMIO64)
            *(uint16_t*)(type.mem_address + field) = value;
        else if (type.type == PCI_BAR_TYPE_ENUM::IO)
            outw(type.io_address + field, value);
    }

	void write_dword(uint64_t address, PCI_BAR_TYPE type, uint16_t field, uint32_t value)
    {
        if (type.type == PCI_BAR_TYPE_ENUM::MMIO32 || type.type == PCI_BAR_TYPE_ENUM::MMIO64)
            *(uint32_t*)(type.mem_address + field) = value;
        else if (type.type == PCI_BAR_TYPE_ENUM::IO)
            outl(type.io_address + field, value);
    }
}