#include <elf/elf.h>

#ifdef __x86_64__
#include <klib/klib.h>
#include <mem/pmm.h>
#include <arch/x86_64/smp/smp.h>
uint64_t _x86_64_ELF_Load(uint8_t *img,pagemap *pm){
    Elf64::Elf64_Ehdr* hdr = (Elf64::Elf64_Ehdr*)img;

    if (hdr->e_ident[0] != 0x7f || hdr->e_ident[1] != 'E' ||
        hdr->e_ident[2] != 'L' || hdr->e_ident[3] != 'F')
        return -1;

    if (hdr->e_type != 2)
        return -1;
    
    Elf64::Elf64_Phdr* phdr = (Elf64::Elf64_Phdr*)(img + hdr->e_phoff);


    for (u16 i = 0; i < hdr->e_entry; i++, phdr++) {
        if (phdr->p_type == 1) {
            // Elf load
            uptr seg_start = ALIGN_DOWN(phdr->p_vaddr, PAGE_SIZE);
            uptr seg_end = ALIGN_UP(seg_start + phdr->p_memsz, PAGE_SIZE);
            size_t seg_size = seg_end - seg_start;
            void* seg = PMM::Alloc(seg_size / PAGE_SIZE);
            VMM::MapUserRange(pm, seg_start, (uptr)seg, seg_size / PAGE_SIZE, PTE_PRESENT | PTE_WRITABLE | PTE_USER);
            VMM::CreateRegion(pm, seg_start, (uptr)seg, seg_size / PAGE_SIZE, PTE_PRESENT | PTE_WRITABLE | PTE_USER);
            pagemap* old_pm = this_cpu()->pm;
            VMM::SwitchPM(pm);
            __memcpy((void*)phdr->p_vaddr, (void*)img + phdr->p_offset, phdr->p_filesz);
            VMM::SwitchPM(old_pm);
        }
    }
    return hdr->e_entry;
}
#endif