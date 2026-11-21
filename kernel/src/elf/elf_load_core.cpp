//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
// SPDX-License-Identifier: GPL-2.0-only
/* elf_load_core.cpp —— 用户 ELF 装载核心（Tier0）
 *
 * 原实现位于 modules/builtin/x86_64/syscall/exec.cpp。Schedule::NewThread
 * 直接调用 elf_load()，且 sched.cpp 使用 NOT_RUNQ_P，所以这两项不能随
 * sys_load 进入 .ko；本文件把它们留在内核，sys_load 通过 elf_load_bounded
 * 走同一份边界校验实现。
 */
#include <elf/elf.h>
#include <arch/x86_64/smp/smp.h>
#include <arch/x86_64/vmm/vmm.h>
#include <mem/pmm.h>
#include <klib/kio.h>
#include <klib/klib.h>
#include <klib/algorithm/art.h>
#include <arch/x86_64/cpu/smap.h>

extern int has_rdrand(void);
extern uint64_t rdrand64_retry(int retries);

#define USER_ADDR_LIMIT 0x0000800000000000ULL

static uint64_t elf_load_impl(uint8_t *data, pagemap_t *pagemap,
                              uint64_t *tls_offset, uint64_t *tls_memsz,
                              uint64_t *tls_filesz, uint64_t *tls_align,
                              uint64_t data_len, uint64_t *out_bias = nullptr) {
    Elf64::Elf64_Ehdr *hdr = (Elf64::Elf64_Ehdr*)data;

    if (hdr->e_ident.c[0] != 0x7f ||
        hdr->e_ident.c[1] != 'E' ||
        hdr->e_ident.c[2] != 'L' ||
        hdr->e_ident.c[3] != 'F') {
        kerrorln("LOAD FILE NOT ELF!");
        return 0;
    }
    /* ET_EXEC (2) loads at its linked (fixed) address; ET_DYN (3) is a PIE
       and receives a random page-aligned load bias (ASLR code randomisation). */
    uint64_t load_bias = 0;
    if (hdr->e_type == 3) {
        
        uint64_t rv = 0;
        if(has_rdrand())
            rv = rdrand64_retry(4);
        if (!rv) { uint32_t _lo,_hi; __asm__ __volatile__("rdtsc":"=a"(_lo),"=d"(_hi));
                   rv = ((uint64_t)_hi<<32)|_lo; rv ^= (uint64_t)(uintptr_t)hdr; }
        load_bias = 0x10000000ULL + ((rv >> 12) % 0x3F00000ULL) * PAGE_SIZE;
    } else if (hdr->e_type != 2) {
        kinfoln("ELF TYPE %d", hdr->e_type);
        kerrorln("ELF> LOAD ELF FILE TYPE NOT SUPPORT!");
        return 0;
    }
    if (hdr->e_ident.c[Elf64::EI_CLASS] != Elf64::ELFCLASS64) {
        kerrorln("ELF> FILE NOT 64 BIT!");
        return 0;
    }
    if (hdr->e_machine != 62) {               // 修复: 只认 EM_X86_64
        kerrorln("ELF> WRONG MACHINE TYPE %d!", hdr->e_machine);
        return 0;
    }
    /* 修复: 程序头表边界校验 (data_len==0 表示旧调用方, 跳过) */
    if (data_len != 0) {
        if (unlikely(data_len < sizeof(Elf64::Elf64_Ehdr))) {
            kerrorln("ELF> FILE TOO SMALL!");
            return 0;
        }
        if (unlikely(hdr->e_phnum == 0 || hdr->e_phnum == 0xFFFF)) {
            kerrorln("ELF> BAD PHNUM!");
            return 0;
        }
        uint64_t ph_end = hdr->e_phoff + (uint64_t)hdr->e_phnum * sizeof(Elf64::Elf64_Phdr);
        if (unlikely(hdr->e_phoff >= data_len || ph_end > data_len)) {
            kerrorln("ELF> PHDR TABLE OUT OF BOUNDS!");
            return 0;
        }
    }

    Elf64::Elf64_Phdr *phdrs = (Elf64::Elf64_Phdr*)(data + hdr->e_phoff);
    uint64_t max_vaddr = 0;

    // 保存进入时的地址空间,结尾恢复(否则 sysret 回调用者时 CR3 是错的 → 用户态取指 #PF)
    pagemap_t *restore_pm = VMM::SwitchPageMap(pagemap);

    for (int i = 0; i < hdr->e_phnum; i++) {
        Elf64::Elf64_Phdr *phdr = &phdrs[i];
        if (phdr->p_type == 1) {
            const uint64_t eff_vaddr = phdr->p_vaddr + load_bias;
            uint64_t filesz = phdr->p_filesz;
            /* 修复: 畸形段防御 —— filesz>memsz 会写穿映射区, 钳到 memsz */
            if (unlikely(filesz > phdr->p_memsz)) filesz = phdr->p_memsz;
            /* 修复: 段数据不得越过文件缓冲区。
               原写法 phdr->p_offset + filesz 在 p_offset 接近 UINT64_MAX 时
               回绕, 可绕过检查导致下方 memcpy 越界读内核内存。改为无回绕形式。 */
            if (unlikely(data_len != 0 &&
                         (phdr->p_offset > data_len || filesz > data_len - phdr->p_offset))) {
                kerrorln("ELF> SEGMENT DATA OUT OF BOUNDS!");
                VMM::SwitchPageMap(restore_pm ? restore_pm : (pagemap_t*)kernel_pagemap);
                return 0;
            }
            /* 修复: 目标地址必须在用户半区, 防止把内核地址映射进用户页表 */
            if (unlikely(eff_vaddr >= USER_ADDR_LIMIT ||
                         eff_vaddr + phdr->p_memsz >= USER_ADDR_LIMIT ||
                         eff_vaddr + phdr->p_memsz < eff_vaddr)) {
                kerrorln("ELF> SEGMENT VADDR ILLEGAL: 0x%lx", eff_vaddr);
                VMM::SwitchPageMap(restore_pm ? restore_pm : (pagemap_t*)kernel_pagemap);
                return 0;
            }

            uint64_t start = ALIGN_DOWN(eff_vaddr, PAGE_SIZE);
            uint64_t end = ALIGN_UP(eff_vaddr + phdr->p_memsz, PAGE_SIZE);
            uint64_t flags = MM_READ | MM_WRITE | MM_USER;
            for (uint64_t p = start; p < end; p += PAGE_SIZE) {
                // 两个 PT_LOAD 共享边界页时,第二段会重新分配物理页
                // 覆盖第一段尾部数据并泄漏旧页 → 已映射则跳过
                if (VMM::GetPhysics(pagemap, p))
                    continue;
                uint64_t page = (uint64_t)PMM::Request();
                if (unlikely(page == 0)) {
                    /* 低内存下 PMM 耗尽：不能把物理 0 映射进用户页表再往
                       (void*)eff_vaddr memcpy —— 那会在用户态未映射地址写
                       128-bit SSE 并内核 #PF（64MB 下 spawn DOOM 实测）。
                       返回加载失败，让 sys_load 走失败回收路径。 */
                    kerrorln("ELF> OUT OF MEMORY mapping 0x%lx", (unsigned long)p);
                    VMM::SwitchPageMap(restore_pm ? restore_pm
                                                   : (pagemap_t*)kernel_pagemap);
                    return 0;
                }
                VMM::Map(pagemap, p, page, flags);
                /* 每页 kinfoln 为运行期噪音, 已降级移除 */
            }
            VMM::NewMapping(pagemap, start, (end - start) / PAGE_SIZE, flags);
            // 登记 VMA:否则 Fork 克隆不到镜像、进程退出时这些页无人释放
            VMM::VMA::AddRegion(pagemap, start, (end - start) / PAGE_SIZE, flags);
            { SmapGuard seg_ug;   // write segment bytes into the live user mapping
              __memcpy((void*)eff_vaddr, (void*)(data + phdr->p_offset), filesz);
              if (phdr->p_memsz > filesz)
                  _memset((void*)(eff_vaddr + filesz), 0, phdr->p_memsz - filesz);
            }
            /* 权限收紧**不在这里做** —— 见第一个 PT_LOAD 循环之后的第二趟。
               原因(实测): 段可以**重叠**。hw2 动态版的布局是
                 seg2 va=0x401000 filesz=0x3060 flags=5 (RX)  跨 0x401000~0x404060
                 seg3 va=0x402000 filesz=0x3040 flags=4 (R)   起点落在 seg2 的页内
               如果每段拷完就立刻收紧, seg2 的收尾会把 0x402000 变成只读,
               紧接着 seg3 往同一页拷数据 -> 写只读页 #PF
               (ERROR_CODE=0x3, CR0.WP=1, 内核态同样受保护)。
               所以统一挪到"全部段都拷完"之后再按段收紧。 */
            (void)0;
            if (end > max_vaddr)
                max_vaddr = end;
            kinfoln("ELF LOADER PT_LOAD: vaddr=[0x%lx~0x%lx], filesz=0x%lx, memsz=0x%lx, flags=0x%x",
                    phdr->p_vaddr, phdr->p_vaddr + phdr->p_memsz,
                    filesz, phdr->p_memsz, phdr->p_flags);
        }
        else if (phdr->p_type == 7) {
            if (tls_offset) *tls_offset = phdr->p_offset;
            if (tls_memsz)  *tls_memsz  = phdr->p_memsz;
            if (tls_filesz) *tls_filesz = phdr->p_filesz;
            if (tls_align)  *tls_align  = phdr->p_align;
            kinfoln("ELF LOADER PT_TLS: offset=0x%lx, filesz=0x%lx, memsz=0x%lx, align=0x%lx",
                    phdr->p_offset, phdr->p_filesz, phdr->p_memsz, phdr->p_align);
        }
    }

    /* ---------------------------------------------------------------------
     * 第二趟: 所有段都拷完之后, 再按 ELF 的 p_flags 收紧权限。
     * 为什么必须推迟到这里: 段与段**可以重叠** (ld 在页粒度上让相邻段共享
     * 同一个物理页)。若在每段拷完就收紧, 前一段会把共享页设成只读,
     * 后一段再往同一页写数据就缺页 (实测 CR2=0x402000, ERROR_CODE=0x3,
     * CR0.WP=1 下内核态同样受写保护)。
     * 收紧规则保持不变 (B25/W^X): 可执行段去掉 W, 不可执行段加 NX。
     * ------------------------------------------------------------------- */
    for (int i = 0; i < hdr->e_phnum; i++) {
        Elf64::Elf64_Phdr *phdr = &phdrs[i];
        if (phdr->p_type != 1) continue;
        const uint64_t eff_vaddr = phdr->p_vaddr + load_bias;
        uint64_t start = ALIGN_DOWN(eff_vaddr, PAGE_SIZE);
        uint64_t end = ALIGN_UP(eff_vaddr + phdr->p_memsz, PAGE_SIZE);
        uint64_t prot_flags = MM_READ | MM_USER |
                              ((phdr->p_flags & 2) && !(phdr->p_flags & 1) ? MM_WRITE : 0) |
                              ((phdr->p_flags & 1) ? 0 : MM_NX);
        for (uint64_t p = start; p < end; p += PAGE_SIZE) {
            VMM::Internal::PageInfo pi = VMM::Internal::GetPageInfo(pagemap, p);
            if (pi.size == PAGE_2MB) {
                VMM::Map2M(pagemap, p, pi.phys, prot_flags);
                __asm__ volatile("invlpg (%0)" : : "r"(p) : "memory");
                p += PAGE_2MB - PAGE_SIZE;
            } else if (pi.size == PAGE_SIZE) {
                VMM::Map4K(pagemap, p, pi.phys, prot_flags);
                __asm__ volatile("invlpg (%0)" : : "r"(p) : "memory");
            }
        }
    }

    /* 这里**故意不**给 WM 协议页兜底: 那一页是 WM 与客户端之间的用户态
       ABI, 固定 VA 的约定只存在于 lib/graphic/winstyle.h 和 desktop/loader
       里, 加载器不认识它、也不该认识它 (内核无关性: 换个窗口系统, 加载器
       一行都不用动)。缺页的 PIE 客户端由 WM 在使用前补上, 见
       programs/desktop/loader.cpp 的 "share 2 前置"。 */

    // 恢复进入时的地址空间(!smp_started 时 SwitchPageMap 返回 nullptr,兜底内核)
    VMM::SwitchPageMap(restore_pm ? restore_pm : (pagemap_t*)kernel_pagemap);
    max_vaddr += PAGE_SIZE;
    VMM::VMA::SetStart(pagemap, max_vaddr, 1);
    kpokln("LOAD ELF!");

    /* 入口点必须可见: "进程刚起来就 exit 了"这类问题的第一现场就是
       "内核到底跳到哪条指令"。 */
    kinfoln("ELF entry: e_entry=0x%lx bias=0x%lx -> rip=0x%lx",
            (unsigned long)hdr->e_entry, (unsigned long)load_bias,
            (unsigned long)(hdr->e_entry + load_bias));

    if (out_bias) *out_bias = load_bias;
    return hdr->e_entry + load_bias;
}

/* 兼容包装: 保持原 6 参数签名 —— task.cpp 的 extern 声明按此签名 mangle,
 * 改参数表会链接失败。新调用方(sys_load)走 elf_load_impl 带长度校验。
 * out_bias: 新增的可选出参, 动态链接需要它把 DT_* 里的链接期虚拟地址
 *           换算成运行期地址 (PIE 主程序必需, 见 elf_dyn.cpp)。 */
uint64_t elf_load(uint8_t *data, pagemap_t *pagemap,
                  uint64_t *tls_offset = nullptr,
                  uint64_t *tls_memsz = nullptr,
                  uint64_t *tls_filesz = nullptr,
                  uint64_t *tls_align = nullptr,
                  uint64_t *out_bias = nullptr) {
    return elf_load_impl(data, pagemap, tls_offset, tls_memsz,
                         tls_filesz, tls_align, 0, out_bias);
}

/* sys_load 使用带 data_len 的入口；与 elf_load 共享同一 impl。 */
uint64_t elf_load_bounded(uint8_t *data, pagemap_t *pagemap,
                          uint64_t *tls_offset, uint64_t *tls_memsz,
                          uint64_t *tls_filesz, uint64_t *tls_align,
                          uint64_t data_len, uint64_t *out_bias) {
    return elf_load_impl(data, pagemap, tls_offset, tls_memsz,
                         tls_filesz, tls_align, data_len, out_bias);
}

art_tree *NOT_RUNQ_P;
