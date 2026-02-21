#pragma once

#include <stdint.h>
#include <stddef.h>

#include <klib/klib.h>


namespace ACPI{
    
    struct RSDP1
    {
        uint8_t Signature[8];
        uint8_t Checksum;
        uint8_t OEM_ID[6];
        uint8_t Revision;
        uint32_t RSDTAddress;
    } __attribute__((packed));

    struct RSDP2
    {
        RSDP1 firstPart;

        uint32_t Length;
        uint64_t XSDTAddress;
        uint8_t ExtendedChecksum;
        uint8_t Reserved[3];

    } __attribute__((packed));



    struct SDTHeader
    {
        uint8_t Signature[4];
        uint32_t Length;
        uint8_t Revision;
        uint8_t Checksum;
        uint8_t OEM_ID[6];
        uint8_t OEM_TABLE_ID[8];
        uint32_t OEM_Revision;
        uint32_t Creator_ID; 
        uint32_t CreatorRevision;
    } __attribute__((packed));

    

    struct DeviceConfig
    {
        uint64_t BaseAddress;
        uint16_t PCISegGroup;
        uint8_t StartBus;
        uint8_t EndBus;
        uint32_t Reserved;
    } __attribute__((packed));

    struct MCFGHeader
    {
        SDTHeader Header;
        uint64_t Reserved;
        DeviceConfig ENTRIES[];
    } __attribute__((packed));

    typedef struct SDT{
        ACPI::SDTHeader header;
        char table[];
    } __attribute__((packed)) SDT;

    typedef struct[[gnu::packed]] {
        uint8_t addr_space_id; //os_acpi_gas_addrspace_kind
        uint8_t reg_bit_width;
        uint8_t reg_bit_offset; 
        uint8_t reserved; //os_acpi_gas_access_size_kind
        uint64_t address;
    } gas_t;

    enum os_acpi_gas_addrspace_kind : uint8_t {
        OS_ACPI_GAS_ADDRSPACE_KIND_SYSMEM,
        OS_ACPI_GAS_ADDRSPACE_KIND_SYS_IO,
        OS_ACPI_GAS_ADDRSPACE_KIND_PCI_CONFIG,
        OS_ACPI_GAS_ADDRSPACE_KIND_EMBED_CONTROLLER,
        OS_ACPI_GAS_ADDRSPACE_KIND_SYS_MANAGEMENT_BUS,
        OS_ACPI_GAS_ADDRSPACE_KIND_SYS_CMOS,
        OS_ACPI_GAS_ADDRSPACE_KIND_SYS_PCI_DEV_BAR_TARGET,
        OS_ACPI_GAS_ADDRSPACE_KIND_SYS_INTELLIGENT_PLATFORM_MANAGEMENT_INFRA,
        OS_ACPI_GAS_ADDRSPACE_KIND_SYS_GEN_PURPOSE_IO,
        OS_ACPI_GAS_ADDRSPACE_KIND_SYS_GEN_SERIAL_BUS,
        OS_ACPI_GAS_ADDRSPACE_KIND_SYS_PLATFORM_COMM_CHANNEL,
    };

    enum os_acpi_gas_access_size_kind : uint8_t {
        OS_ACPI_GAS_ACCESS_SIZE_UNDEFINED,

        OS_ACPI_GAS_ACCESS_SIZE_1_BYTE,
        OS_ACPI_GAS_ACCESS_SIZE_2_BYTE,
        OS_ACPI_GAS_ACCESS_SIZE_4_BYTE,
        OS_ACPI_GAS_ACCESS_SIZE_8_BYTE,
    };

    PACK(typedef struct FADTHeader{
        SDTHeader sdt;

        uint32_t firmware_ctrl;
        uint32_t dsdt;

        // Field used in ACPI 1.0; no longer in use, for compatibility only
        uint8_t reserved;
        uint8_t preferred_power_management_profile;

        uint16_t sci_interrupt;
        uint32_t smi_command_port;

        uint8_t acpi_enable;
        uint8_t acpi_disable;

        uint8_t s4bios_req;

        uint8_t pstate_control;
        uint32_t pm1a_event_block;
        uint32_t pm1b_event_block;
        uint32_t pm1a_control_block;
        uint32_t pm1b_control_block;
        uint32_t pm2_control_block;
        uint32_t pm_timer_block;

        uint32_t gpe0_block;
        uint32_t gpe1_block;

        uint8_t pm1_event_length;
        uint8_t pm1_control_length;
        uint8_t pm2_control_length;
        uint8_t pm_timer_length;

        uint8_t gpe0_length;
        uint8_t gpe1_length;
        uint8_t gpe1_base;

        uint8_t cstate_control;

        uint16_t worst_c2_latency;
        uint16_t worst_c3_latency;

        uint16_t flush_size;
        uint16_t flush_stride;

        uint8_t duty_offset;
        uint8_t duty_width;

        /*
        * Eight bit value that can represent 0x01-0x31 days in BCD or 0x01-0x1F
        * days in binary. Bits 6 and 7 of this field are treated as Ignored by
        * software. The RTC is initialized such that this field contains a "don't
        * care" value when the platform firmware switches from legacy to ACPI mode.
        * A don’t care value can be any unused value (not 0x1-0x31 BCD or 0x01-0x1F
        * hex) that the RTC reverts back to a 24 hour alarm.
        */
        uint8_t day_alarm;

        /*
        * Eight bit value that can represent 01-12 months in BCD or 0x01-0xC months
        * in binary. The RTC is initialized such that this field contains a don't
        * care value when the platform firmware switches from legacy to ACPI mode.
        * A "don't care" value can be any unused value (not 1-12 BCD or x01-xC hex)
        * that the RTC reverts back to a 24 hour alarm and/or 31 day alarm).
        */
        uint8_t month_alarm;

        /*
        * 8-bit BCD or binary value. This value indicates the thousand year and
        * hundred year (Centenary) variables of the date in BCD (19 for this
        * century, 20 for the next) or binary (x13 for this century, x14 for the
        * next).
        */
        uint8_t century;
        uint16_t iapc_boot_arch_flags;

        uint8_t reserved2;
        uint32_t flags;

        gas_t reset_reg;

        uint8_t reset_value;
        uint16_t arm_boot_arch_flags;
        uint8_t minor_version;

        // Available on ACPI 2.0+
        uint64_t x_firmware_ctrl;
        uint64_t x_dsdt;

        gas_t x_pm1a_event_block;
        gas_t x_pm1b_event_block;
        gas_t x_pm1a_ctrl_block;
        gas_t x_pm1b_ctrl_block;
        gas_t x_pm2_ctrl_block;
        gas_t x_pm_timer_block;
        gas_t x_gpe0_block;
        gas_t x_gpe1_block;
        gas_t sleep_control_reg;
        gas_t sleep_status_reg;
        uint64_t hypervisor_vendor_identity;
    })FADTHeader;


    extern ACPI::MCFGHeader* mcfg;
    extern ACPI::FADTHeader* FADT;
    extern ACPI::SDTHeader* rootThing;
    extern int32_t ACPI_DIV;
    extern volatile bool UseXSDT;

    u64 Init(void *addr);
    void* FindTable(SDTHeader* sdtHeader, const char* signature, int32_t div);
    void* FindTable(const char* signature);
}