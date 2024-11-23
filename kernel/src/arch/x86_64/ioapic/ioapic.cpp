#include "ioapic.h"

namespace IOAPIC{
    u64 Init(){
        madt_ioapic* ioapic = madt_ioapic_list[0];
  
        u32 val = IOAPIC::Read(ioapic, IOAPIC_VER);
        u32 count = ((val >> 16) & 0xFF);

        if ((IOAPIC::Read(ioapic, 0) >> 24) != ioapic->apic_id) {
            kinfo("   IOAPIC::Init(): Not setting it up from the MP.\n");
            return 0;
        }

        for (u8 i = 0; i <= count; ++i) {
            IOAPIC::Write(ioapic, IOAPIC_REDTBL+2*i, 0x00010000 | (32 + i));
            IOAPIC::Write(ioapic, IOAPIC_REDTBL+2*i+1, 0); // redir cpu
        }
        
        kinfo("   IOAPIC::Init(): IO/APIC Initialised.\n");

        return 1;
        // We disable all entries by default
    }
    void Write(madt_ioapic* ioapic, u8 reg, u32 val) {
        *((volatile u32*)(HIGHER_HALF(ioapic->apic_addr) + IOAPIC_REGSEL)) = reg;
        *((volatile u32*)(HIGHER_HALF(ioapic->apic_addr) + IOAPIC_IOWIN)) = val;
    }

    u32 Read(madt_ioapic* ioapic, u8 reg) {
        *((volatile u32*)(HIGHER_HALF(ioapic->apic_addr) + IOAPIC_REGSEL)) = reg;
        return *((volatile u32*)(HIGHER_HALF(ioapic->apic_addr) + IOAPIC_IOWIN));
    }

    void SetEntry(madt_ioapic* ioapic, u8 idx, u64 data) {
        IOAPIC::Write(ioapic, (u8)(IOAPIC_REDTBL + idx * 2), (u32)data);
        IOAPIC::Write(ioapic, (u8)(IOAPIC_REDTBL + idx * 2 + 1), (u32)(data >> 32));
    }

    u64 GSICount(madt_ioapic* ioapic) {
        return (IOAPIC::Read(ioapic, 1) & 0xff0000) >> 16;
    }

    madt_ioapic* GetGSI(u32 gsi) {
        for (u64 i = 0; i < madt_ioapic_len; i++) {
            madt_ioapic* ioapic = madt_ioapic_list[i];
            if (ioapic->gsi_base <= gsi && ioapic->gsi_base + GSICount(ioapic) > gsi)
                return ioapic;
        }
        return NULL;
    }

    void RedirectGSI(u32 lapic_id, u8 vec, u32 gsi, u16 flags, bool mask) {
        madt_ioapic* ioapic = IOAPIC::GetGSI(gsi);

        u64 redirect = vec;

        if ((flags & (1 << 1)) != 0) {
            redirect |= (1 << 13);
        }

        if ((flags & (1 << 3)) != 0) {
            redirect |= (1 << 15);
        }

        if (mask) redirect |= (1 << 16);
        else redirect &= ~(1 << 16);

        redirect |= (uint64_t)lapic_id << 56;

        u32 redir_table = (gsi - ioapic->gsi_base) * 2 + 16;
        IOAPIC::Write(ioapic, redir_table, (u32)redirect);
        IOAPIC::Write(ioapic, redir_table + 1, (u32)(redirect >> 32));
    }

    void RedirectIRQ(u32 lapic_id, u8 vec, u8 irq, bool mask) {
        u8 idx = 0;
        madt_iso* iso = NULL;

        while (idx < madt_iso_len) {
            iso = madt_iso_list[idx];
            if (iso->irq_src == irq) {
                IOAPIC::RedirectGSI(lapic_id, vec, iso->gsi, iso->flags, mask);
                return;
            }
            idx++;
        }

        IOAPIC::RedirectGSI(lapic_id, vec, irq, 0, mask);
    }

    u32 GetRedirectIRQ(u8 irq) {
        u8 idx = 0;
        madt_iso* iso;

        while (idx < madt_iso_len) {
            iso = madt_iso_list[idx];
            if (iso->irq_src == irq) {
                return iso->gsi;
            }
            idx++;
        }

        return irq;
    }
}