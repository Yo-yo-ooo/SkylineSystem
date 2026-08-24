// SPDX-FileCopyrightText: 2026 Yo-yo-ooo
// SPDX-License-Identifier: GPL-2.0-only
#include <limine.h>
#include <arch/x86_64/smp/smp.h>
#include <conf.h>
#include <arch/x86_64/vmm/vmm.h>
#include <klib/klib.h>
#include <arch/x86_64/lapic/lapic.h>
#include <mem/pmm.h>
#include <klib/kio.h>
#include <arch/x86_64/schedule/sched.h>

#ifndef container_of
#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))
#endif


#define PTE_KEEP        0xFFF0000000000FFFULL

__attribute__((used, section(".limine_requests")))
static volatile struct limine_executable_address_request limine_executable_address = {
    .id = LIMINE_EXECUTABLE_ADDRESS_REQUEST_ID, .revision = 0
};

#if CONFIG_VMM_5LVL_MAP == 1
#define REQ_TOP_LVL LIMINE_PAGING_MODE_X86_64_5LVL
#else
#define REQ_TOP_LVL LIMINE_PAGING_MODE_X86_64_4LVL
#endif

__attribute__((used, section(".limine_requests")))
static volatile struct limine_paging_mode_request paging_mode_request = {
    .id = LIMINE_PAGING_MODE_REQUEST_ID, .revision = 0,
    .mode = REQ_TOP_LVL, .max_mode = REQ_TOP_LVL,
    .min_mode = LIMINE_PAGING_MODE_X86_64_MIN
};

volatile bool IsPM5LVL = (paging_mode_request.response->mode == REQ_TOP_LVL);

extern spinlock_t pmm_lock;
#define PHYS_BASE(x) (x - executable_vaddr + executable_paddr)
volatile pagemap_t* kernel_pagemap = nullptr;
extern "C" void mmu_invlpg(uint64_t vaddr);
extern struct limine_memmap_response* pmm_memmap;
extern char ld_limine_start[], ld_limine_end[], ld_text_start[], ld_text_end[];
extern char ld_rodata_start[], ld_rodata_end[], ld_data_start[], ld_data_end[];
extern bool smp_started;

#ifndef MAX_CPU
#define MAX_CPU 256
#endif
#define TLB_MASK_WORDS ((MAX_CPU + 63) / 64)

/* ============================================================
 *  红黑树引用计数 — 仅跟踪被共享的物理页
 *  非共享页不在树中, RefDecPhys 直接返回 true (单主人, 直接释放)
 *  树节点 rc_node 通过 kmalloc 分配 (~72 bytes), 远小于 flat array
 * ============================================================ */

struct rc_node {
    rb_node_t rb;       /* 56 bytes (含 parent/left/right/color/flags/time/count) */
    uint64_t  phys;     /* 物理页基址 (4K 对齐) */
    uint32_t  count;    /* 引用计数 */
    uint8_t   _pad[4];
};

static int rc_cmp(const rb_node_t *a, const rb_node_t *b) {
    const rc_node *ra = container_of(a, rc_node, rb);
    const rc_node *rb_ = container_of(b, rc_node, rb);
    if (ra->phys < rb_->phys) return -1;
    if (ra->phys > rb_->phys) return 1;
    return 0;
}

static rb_root_t   rc_tree       = {};
static spinlock_t  rc_tree_lock  = 0;

/* 裸搜索 (跳过 rb_touch 的温度维护, 引用计数不需要) */
static inline rc_node *rc_search(uint64_t phys) {
    rb_node_t *cur = rc_tree.node;
    while (likely(cur)) {
        rc_node *rn = container_of(cur, rc_node, rb);
        if (phys < rn->phys)      cur = cur->left;
        else if (phys > rn->phys) cur = cur->right;
        else                      return rn;
    }
    return nullptr;
}

/* 公共 API: 首次共享 0→2 (src+dst), 后续 ++
   在 sys_pmmapSHARE / Fork 的 pt_lock 临界区内调用,
   rc_tree_lock 是 pt_lock 之后获取的全局锁, 无反向持有者, 无死锁 */
void RefSharedPhys(uint64_t phys) {
    /* 预分配在锁外: kmalloc 可能触发页面回收, 持锁 kmalloc 有死锁风险 */
    rc_node *rn = (rc_node*)kmalloc(sizeof(rc_node));
    if (unlikely(!rn)) {
        kwarnln("VMM: rc_node OOM, sharing untracked (phys=%#lx)", phys);
        return;
    }

    spinlock_lock(&rc_tree_lock);

    rc_node *found = rc_search(phys);
    if (likely(found)) {
        found->count++;
        spinlock_unlock(&rc_tree_lock);
        kfree(rn);              /* 预分配未用上, 回收 */
        return;
    }

    rn->phys  = phys;
    rn->count = 2;             /* src 主人 + dst 共享者 */
    rb_init_node(&rn->rb);
    rb_insert_raw(&rc_tree, &rn->rb, rc_cmp);
    rc_tree.cnt++;

    spinlock_unlock(&rc_tree_lock);
}

/* 内部: 递减引用, 返回 true 表示归零应 PMM::Free
   在 CleanPM / Free / HandlePF 的 pt_lock 临界区内调用 */
static inline bool RefDecPhys(uint64_t phys) {
    spinlock_lock(&rc_tree_lock);

    rc_node *found = rc_search(phys);
    if (unlikely(!found)) {
        /* 树中无此页 → 非共享, 单主人 → 直接释放 */
        spinlock_unlock(&rc_tree_lock);
        return true;
    }

    if (--found->count == 0) {
        /* 最后一个引用: 从树摘除并释放 rc_node 结构 */
        rb_node_t hint_key = {};
        /* rb_erase 会清 hint (若匹配), 做法是 CAS root->hint */
        rb_erase_raw(&rc_tree, &found->rb);
        rc_tree.cnt--;
        spinlock_unlock(&rc_tree_lock);
        kfree(found);
        return true;            /* 物理页也释放 */
    }

    spinlock_unlock(&rc_tree_lock);
    return false;
}

static void RefcountTreeInit() {
    rb_root_init(&rc_tree, nullptr, nullptr, nullptr, nullptr, nullptr);
    rc_tree.node = nullptr;
    rc_tree.cnt  = 0;
}

namespace VMM {
    namespace CPUFeatures { bool has_pcid = false; bool has_invpcid = false; }
    static spinlock_t pcid_alloc_lock = 0;
    static uint64_t pcid_bitmap[64] = {0};

    static inline uint32_t AllocPCID() {
        if (unlikely(!CPUFeatures::has_pcid || !CPUFeatures::has_invpcid)) return 0;
        spinlock_lock(&pcid_alloc_lock);
        pcid_bitmap[0] |= 1;
        for (int i = 0; i < 64; i++) {
            if (pcid_bitmap[i] != ~0ULL) {
                int bit = __builtin_ffsll(~pcid_bitmap[i]) - 1;
                uint32_t pcid = i * 64 + bit;
                pcid_bitmap[i] |= (1ULL << bit);
                spinlock_unlock(&pcid_alloc_lock);
                return pcid;
            }
        }
        spinlock_unlock(&pcid_alloc_lock);
        return 0;
    }

    static inline void FreePCID(uint32_t pcid) {
        if (pcid == 0) return;
        spinlock_lock(&pcid_alloc_lock);
        pcid_bitmap[pcid / 64] &= ~(1ULL << (pcid % 64));
        spinlock_unlock(&pcid_alloc_lock);
    }

    static inline void BitmapSet(uint64_t *map, uint32_t cpu) {
        __atomic_fetch_or(&map[cpu / 64], 1ULL << (cpu % 64), __ATOMIC_RELAXED);
    }
    static inline void BitmapClear(uint64_t *map, uint32_t cpu) {
        __atomic_fetch_and(&map[cpu / 64], ~(1ULL << (cpu % 64)), __ATOMIC_RELAXED);
    }
    static inline void BitmapClearAll(uint64_t *map) {
        for (int i = 0; i < TLB_MASK_WORDS; i++)
            __atomic_store_n(&map[i], 0, __ATOMIC_RELAXED);
    }

    #define TLB_FLUSH_VEC (SCHED_VEC + 1)

    namespace LazyTLB {
        static inline void cpuid(uint32_t leaf, uint32_t &a, uint32_t &b, uint32_t &c, uint32_t &d) {
            __asm__ volatile ("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(leaf));
        }
        void IPIHandler(context_t *ctx);

        void Init() {
            uint32_t a, b, c, d;
            cpuid(1, a, b, c, d); CPUFeatures::has_pcid = (c >> 17) & 1;
            cpuid(7, a, b, c, d); CPUFeatures::has_invpcid = (b >> 10) & 1;
            if (likely(CPUFeatures::has_pcid && CPUFeatures::has_invpcid)) {
                uint64_t cr4; __asm__ volatile ("movq %%cr4, %0" : "=r"(cr4));
                cr4 |= (1ULL << 17); __asm__ volatile ("movq %0, %%cr4" :: "r"(cr4));
            } else { CPUFeatures::has_pcid = false; }
            idt_install_irq(TLB_FLUSH_VEC, (void*)IPIHandler);
            idt_set_ist(TLB_FLUSH_VEC, 0);
        }

        static inline void LocalInvlpg(pagemap_t *pm, uint64_t vaddr) {
            if (likely(CPUFeatures::has_invpcid)) {
                struct { uint64_t vaddr; uint64_t pcid; } __attribute__((aligned(16))) desc = { vaddr, pm ? pm->pcid : 0 };
                __asm__ volatile ("invpcid %1, %0" : : "r"((uint64_t)0), "m"(desc) : "memory");
            } else {
                __asm__ volatile ("invlpg (%0)" :: "r"(vaddr) : "memory");
            }
        }

        static inline void LocalFullFlush(pagemap_t *pm) {
            if (likely(CPUFeatures::has_invpcid)) {
                struct { uint64_t vaddr; uint64_t pcid; } __attribute__((aligned(16))) desc = { 0, pm ? pm->pcid : 0 };
                __asm__ volatile ("invpcid %1, %0" : : "r"((uint64_t)1), "m"(desc) : "memory");
            } else {
                uint64_t cr3; __asm__ volatile ("movq %%cr3, %0\n\tmovq %0, %%cr3" : "=&r"(cr3) :: "memory");
            }
        }

        static inline void LocalGlobalFlush() {
            if (likely(CPUFeatures::has_invpcid)) {
                struct { uint64_t vaddr; uint64_t pcid; } __attribute__((aligned(16))) desc = { 0, 0 };
                __asm__ volatile ("invpcid %1, %0" : : "r"((uint64_t)3), "m"(desc) : "memory");
            } else {
                uint64_t cr4; __asm__ volatile ("movq %%cr4, %0" : "=r"(cr4));
                cr4 &= ~(1ULL << 7); __asm__ volatile ("movq %0, %%cr4" :: "r"(cr4) : "memory");
                uint64_t cr3; __asm__ volatile ("movq %%cr3, %0\n\tmovq %0, %%cr3" : "=&r"(cr3) :: "memory");
                cr4 |= (1ULL << 7); __asm__ volatile ("movq %0, %%cr4" :: "r"(cr4) : "memory");
            }
        }

        void OnAttach(pagemap_t *pm) {
            if (unlikely(!smp_started || !pm)) return;
            BitmapSet(pm->cpus_with_tlb, this_cpu()->id);
        }
        void OnDetach(pagemap_t *pm) {
            if (unlikely(!smp_started || !pm || CPUFeatures::has_pcid)) return;
            BitmapClear(pm->cpus_with_tlb, this_cpu()->id);
        }

        void ShootdownPage(pagemap_t *pm, uint64_t vaddr) {
            if (unlikely(!pm)) return;
            LocalInvlpg(pm, vaddr);
            if (unlikely(!smp_started)) return;
            uint32_t me = this_cpu()->id;
            uint64_t targets[TLB_MASK_WORDS];
            for (int i = 0; i < TLB_MASK_WORDS; i++)
                targets[i] = __atomic_load_n(&pm->cpus_with_tlb[i], __ATOMIC_RELAXED);
            targets[me / 64] &= ~(1ULL << (me % 64));

            for (int w = 0; w < TLB_MASK_WORDS; w++) {
                while (targets[w]) {
                    int b = __builtin_ffsll(targets[w]) - 1;
                    targets[w] &= ~(1ULL << b);
                    uint32_t i = w * 64 + b;
                    if (unlikely(i >= MAX_CPU)) continue;
                    cpu_t *target = smp_cpu_list[i];
                    if (unlikely(!target)) continue;

                    uint64_t rf;
                    asm volatile("pushfq\n\tcli\n\tpop %0" : "=r"(rf) :: "memory");
                    spinlock_lock(&target->shootdown_lock);
                    if (likely(target->shootdown_count < 32)) {
                        target->shootdown_queue[target->shootdown_count++] = {pm, vaddr, 1};
                    } else {
                        target->shootdown_queue[0] = {nullptr, 0, 3};
                        target->shootdown_count = 1;
                    }
                    spinlock_unlock(&target->shootdown_lock);
                    asm volatile("push %0\n\tpopfq" :: "r"(rf) : "memory");
                    LAPIC::IPI(target->lapic_id, TLB_FLUSH_VEC);
                }
            }
        }

        void ShootdownFull(pagemap_t *pm) {
            if (unlikely(!pm)) return;
            LocalFullFlush(pm);
            if (unlikely(!smp_started)) return;
            uint32_t me = this_cpu()->id;
            uint64_t targets[TLB_MASK_WORDS];
            for (int i = 0; i < TLB_MASK_WORDS; i++)
                targets[i] = __atomic_load_n(&pm->cpus_with_tlb[i], __ATOMIC_RELAXED);
            targets[me / 64] &= ~(1ULL << (me % 64));

            for (int w = 0; w < TLB_MASK_WORDS; w++) {
                while (targets[w]) {
                    int b = __builtin_ffsll(targets[w]) - 1;
                    targets[w] &= ~(1ULL << b);
                    uint32_t i = w * 64 + b;
                    if (unlikely(i >= MAX_CPU)) continue;
                    cpu_t *target = smp_cpu_list[i];
                    if (unlikely(!target)) continue;

                    uint64_t rf;
                    asm volatile("pushfq\n\tcli\n\tpop %0" : "=r"(rf) :: "memory");
                    spinlock_lock(&target->shootdown_lock);
                    if (likely(target->shootdown_count < 32)) {
                        target->shootdown_queue[target->shootdown_count++] = {pm, 0, 2};
                    } else {
                        target->shootdown_queue[0] = {nullptr, 0, 3};
                        target->shootdown_count = 1;
                    }
                    spinlock_unlock(&target->shootdown_lock);
                    asm volatile("push %0\n\tpopfq" :: "r"(rf) : "memory");
                    LAPIC::IPI(target->lapic_id, TLB_FLUSH_VEC);
                }
            }
            if (unlikely(!CPUFeatures::has_pcid)) {
                BitmapClearAll(pm->cpus_with_tlb);
                BitmapSet(pm->cpus_with_tlb, me);
            }
        }

        void IPIHandler(context_t * /*ctx*/) {
            cpu_t *c = this_cpu();
            if (unlikely(!c)) { LAPIC::EOI(); return; }
            while (true) {
                spinlock_lock(&c->shootdown_lock);
                if (c->shootdown_count == 0) { spinlock_unlock(&c->shootdown_lock); break; }
                auto req = c->shootdown_queue[--c->shootdown_count];
                if (likely(c->shootdown_count > 0)) PREFETCH_RH(&c->shootdown_queue[c->shootdown_count - 1]);
                spinlock_unlock(&c->shootdown_lock);
                if (req.type == 1) LocalInvlpg(req.pm, req.vaddr);
                else if (req.type == 2) { LocalFullFlush(req.pm); if (req.pm) BitmapClear(req.pm->cpus_with_tlb, c->id); }
                else if (req.type == 3) LocalGlobalFlush();
            }
            LAPIC::EOI();
        }
    }

    namespace Useless {
        uint64_t *NewLevel(uint64_t *level, uint64_t entry) {
            uint64_t *new_level = PMM::Request();
            _memset(HIGHER_HALF(new_level), 0, PAGE_SIZE);
            level[entry] = (uint64_t)new_level | 0b111;
            return new_level;
        }

        PageInfo GetPageInfo(pagemap_t *pagemap, uint64_t vaddr) {
            PageInfo info = {0, 0, 0};
            uint64_t *pml4 = (uint64_t*)pagemap->toplvl;
#if CONFIG_VMM_5LVL_MAP == 1
            if (unlikely(IsPM5LVL)) {
                pml4 = (uint64_t*)pagemap->toplvl[PML5E(vaddr)];
                if (unlikely(!PAGE_EXISTS(pml4))) return info;
                pml4 = HIGHER_HALF(PTE_MASK(pml4));
            }
#endif
            uint64_t pdpte_val = (uint64_t)pml4[PML4E(vaddr)];
            if (unlikely(!PAGE_EXISTS(pdpte_val))) return info;
            uint64_t *pdpt = HIGHER_HALF(PTE_MASK(pdpte_val));
            PREFETCH_RH(&pdpt[PDPTE(vaddr)]);

            uint64_t pde_val = pdpt[PDPTE(vaddr)];
            if (unlikely(!PAGE_EXISTS(pde_val))) return info;
            if (pde_val & VMM_PS_BIT) {
                info.phys = pde_val & 0x000FFFFFC0000000ULL; info.size = PAGE_1GB;
                info.flags = pde_val & PTE_KEEP; return info;
            }
            uint64_t *pd = HIGHER_HALF(PTE_MASK(pde_val));
            PREFETCH_RH(&pd[PDE(vaddr)]);

            uint64_t pte_val = pd[PDE(vaddr)];
            if (unlikely(!PAGE_EXISTS(pte_val))) return info;
            if (pte_val & VMM_PS_BIT) {
                info.phys = pte_val & 0x000FFFFFFFE00000ULL; info.size = PAGE_2MB;
                info.flags = pte_val & PTE_KEEP; return info;
            }
            uint64_t *pt = HIGHER_HALF(PTE_MASK(pte_val));
            uint64_t page_val = pt[PTE(vaddr)];
            if (unlikely(!PAGE_EXISTS(page_val))) return info;
            info.phys = page_val & 0x000FFFFFFFFFF000ULL; info.size = PAGE_SIZE;
            info.flags = page_val & PTE_KEEP; return info;
        }

        uint64_t InternalAlloc(pagemap_t *pagemap, uint64_t page_count, uint64_t flags) {
            return VMM::VMA::InternalAlloc(pagemap, page_count, flags, 0);
        }
    }

    void Init(){
        VMM::LazyTLB::Init();
        uint64_t pat = 0;
        pat |= (0 << 0); pat |= (1 << 8); pat |= (4 << 16); pat |= (6 << 24);
        pat |= (5 << 32); pat |= (1 << 40); pat |= (7 << 48); pat |= (7 << 56);
        wrmsr(0x277, pat);

        struct limine_executable_address_response *ea = limine_executable_address.response;
        kernel_pagemap = HIGHER_HALF((pagemap_t*)PMM::Request());
        kernel_pagemap->toplvl = HIGHER_HALF((uint64_t*)PMM::Request());
        kernel_pagemap->vma_head = kernel_pagemap->vma_cursor = nullptr;
        kernel_pagemap->vm_mappings = nullptr;
        kernel_pagemap->pt_lock = 0; kernel_pagemap->vma_lock = 0;
        kernel_pagemap->pcid = AllocPCID();
        BitmapClearAll(kernel_pagemap->cpus_with_tlb);
        rb_root_init(&kernel_pagemap->vma_tree, nullptr, nullptr, nullptr, nullptr, nullptr);
        _memset(kernel_pagemap->toplvl, 0, PAGE_SIZE);
        VMM::VMA::SetStart(kernel_pagemap, HIGHER_HALF(0x100000000000), 0);

        uint64_t ev = ea->virtual_base, ep = ea->physical_base;
        uint64_t ls = ALIGN_DOWN((uint64_t)ld_limine_start, PAGE_SIZE);
        uint64_t le = ALIGN_UP((uint64_t)ld_limine_end, PAGE_SIZE);
        uint64_t ts = ALIGN_DOWN((uint64_t)ld_text_start, PAGE_SIZE);
        uint64_t te = ALIGN_UP((uint64_t)ld_text_end, PAGE_SIZE);
        uint64_t rs = ALIGN_DOWN((uint64_t)ld_rodata_start, PAGE_SIZE);
        uint64_t re = ALIGN_UP((uint64_t)ld_rodata_end, PAGE_SIZE);
        uint64_t ds = ALIGN_DOWN((uint64_t)ld_data_start, PAGE_SIZE);
        uint64_t de = ALIGN_UP((uint64_t)ld_data_end, PAGE_SIZE);

#define PHYS_BASE2(x) (x - ev + ep)
        VMM::MapRange(kernel_pagemap, ls, PHYS_BASE2(ls), MM_READ, DIV_ROUND_UP(le-ls, PAGE_SIZE));
        VMM::MapRange(kernel_pagemap, ts, PHYS_BASE2(ts), MM_READ, DIV_ROUND_UP(te-ts, PAGE_SIZE));
        VMM::MapRange(kernel_pagemap, rs, PHYS_BASE2(rs), MM_READ|MM_NX, DIV_ROUND_UP(re-rs, PAGE_SIZE));
        VMM::MapRange(kernel_pagemap, ds, PHYS_BASE2(ds), MM_READ|MM_WRITE|MM_NX, DIV_ROUND_UP(de-ds, PAGE_SIZE));
#undef PHYS_BASE2
        for (uint64_t gb4 = 0; gb4 < 0x100000000; gb4 += PAGE_2MB) {
            VMM::Map2M(kernel_pagemap, gb4, gb4, MM_READ | MM_WRITE);
            VMM::Map2M(kernel_pagemap, HIGHER_HALF(gb4), gb4, MM_READ | MM_WRITE);
        }
        VMM::SwitchPageMap(kernel_pagemap);

        /* ★ 引用计数红黑树: 仅跟踪共享页, 无预分配大数组 */
        RefcountTreeInit();
        kpokln("VMM: refcount rb-tree initialized (shared-page tracking only)");
    }

    void Map4K(pagemap_t *pm, uint64_t vaddr, uint64_t paddr, uint64_t flags){
        uint64_t *pml4 = (uint64_t*)pm->toplvl;
#if CONFIG_VMM_5LVL_MAP == 1
        if (unlikely(IsPM5LVL)) {
            pml4 = (uint64_t*)pm->toplvl[PML5E(vaddr)];
            if (unlikely(!PAGE_EXISTS(pml4))) pml4 = VMM::Useless::NewLevel(pm->toplvl, PML5E(vaddr));
            pml4 = HIGHER_HALF(PTE_MASK(pml4));
        }
#endif
        uint64_t *pdpt = (uint64_t*)pml4[PML4E(vaddr)];
        if (unlikely(!PAGE_EXISTS(pdpt))) pdpt = VMM::Useless::NewLevel(pml4, PML4E(vaddr));
        pdpt = HIGHER_HALF(PTE_MASK(pdpt)); PREFETCH_RH(&pdpt[PDPTE(vaddr)]);

        uint64_t *pd = (uint64_t*)pdpt[PDPTE(vaddr)];
        if (unlikely(!PAGE_EXISTS(pd))) pd = VMM::Useless::NewLevel(pdpt, PDPTE(vaddr));
        pd = HIGHER_HALF(PTE_MASK(pd)); PREFETCH_RH(&pd[PDE(vaddr)]);

        uint64_t *pt = (uint64_t*)pd[PDE(vaddr)];
        if (unlikely(!PAGE_EXISTS(pt))) pt = VMM::Useless::NewLevel(pd, PDE(vaddr));
        pt = HIGHER_HALF(PTE_MASK(pt));
        pt[PTE(vaddr)] = (paddr & 0x000FFFFFFFFFF000ULL) | (flags & PTE_KEEP);
    }

    void Map2M(pagemap_t *pm, uint64_t vaddr, uint64_t paddr, uint64_t flags){
        uint64_t *pml4 = (uint64_t*)pm->toplvl;
#if CONFIG_VMM_5LVL_MAP == 1
        if (unlikely(IsPM5LVL)) {
            pml4 = (uint64_t*)pm->toplvl[PML5E(vaddr)];
            if (unlikely(!PAGE_EXISTS(pml4))) pml4 = VMM::Useless::NewLevel(pm->toplvl, PML5E(vaddr));
            pml4 = HIGHER_HALF(PTE_MASK(pml4));
        }
#endif
        uint64_t *pdpt = (uint64_t*)pml4[PML4E(vaddr)];
        if (unlikely(!PAGE_EXISTS(pdpt))) pdpt = VMM::Useless::NewLevel(pml4, PML4E(vaddr));
        pdpt = HIGHER_HALF(PTE_MASK(pdpt)); PREFETCH_RH(&pdpt[PDPTE(vaddr)]);

        uint64_t *pd = (uint64_t*)pdpt[PDPTE(vaddr)];
        if (unlikely(!PAGE_EXISTS(pd))) pd = VMM::Useless::NewLevel(pdpt, PDPTE(vaddr));
        pd = HIGHER_HALF(PTE_MASK(pd));
        pd[PDE(vaddr)] = (paddr & 0x000FFFFFFFE00000ULL) | (flags & PTE_KEEP) | VMM_PS_BIT;
    }

    void Map1G(pagemap_t *pm, uint64_t vaddr, uint64_t paddr, uint64_t flags){
        uint64_t *pml4 = (uint64_t*)pm->toplvl;
#if CONFIG_VMM_5LVL_MAP == 1
        if (unlikely(IsPM5LVL)) {
            pml4 = (uint64_t*)pm->toplvl[PML5E(vaddr)];
            if (unlikely(!PAGE_EXISTS(pml4))) pml4 = VMM::Useless::NewLevel(pm->toplvl, PML5E(vaddr));
            pml4 = HIGHER_HALF(PTE_MASK(pml4));
        }
#endif
        uint64_t *pdpt = (uint64_t*)pml4[PML4E(vaddr)];
        if (unlikely(!PAGE_EXISTS(pdpt))) pdpt = VMM::Useless::NewLevel(pml4, PML4E(vaddr));
        pdpt = HIGHER_HALF(PTE_MASK(pdpt));
        pdpt[PDPTE(vaddr)] = (paddr & 0x000FFFFFC0000000ULL) | (flags & PTE_KEEP) | VMM_PS_BIT;
    }

    void Map(pagemap_t *pm, uint64_t v, uint64_t p, uint64_t f){ VMM::Map4K(pm, v, p, f); }
    void Map(uint64_t v, uint64_t p){ VMM::Map(kernel_pagemap, v, p, MM_READ | MM_WRITE); }

    void UnmapNoFlush(pagemap_t *pm, uint64_t vaddr){
        uint64_t *pml4 = (uint64_t*)pm->toplvl;
#if CONFIG_VMM_5LVL_MAP == 1
        if (unlikely(IsPM5LVL)) {
            pml4 = (uint64_t*)pm->toplvl[PML5E(vaddr)];
            if (unlikely(!PAGE_EXISTS(pml4))) return;
            pml4 = HIGHER_HALF(PTE_MASK(pml4));
        }
#endif
        uint64_t pdpte_val = (uint64_t)pml4[PML4E(vaddr)];
        if (unlikely(!PAGE_EXISTS(pdpte_val))) return;
        uint64_t *pdpt = HIGHER_HALF(PTE_MASK(pdpte_val));
        uint64_t pde_val = pdpt[PDPTE(vaddr)];
        if (unlikely(!PAGE_EXISTS(pde_val))) return;
        if (pde_val & VMM_PS_BIT) { pdpt[PDPTE(vaddr)] = 0; return; }
        uint64_t *pd = HIGHER_HALF(PTE_MASK(pde_val));
        uint64_t pte_val = pd[PDE(vaddr)];
        if (unlikely(!PAGE_EXISTS(pte_val))) return;
        if (pte_val & VMM_PS_BIT) { pd[PDE(vaddr)] = 0; return; }
        uint64_t *pt = HIGHER_HALF(PTE_MASK(pte_val));
        pt[PTE(vaddr)] = 0;
    }

    void Unmap(pagemap_t *pm, uint64_t vaddr){ UnmapNoFlush(pm, vaddr); LazyTLB::ShootdownPage(pm, vaddr); }
    uint64_t GetPhysics(pagemap_t *pm, uint64_t v){ return VMM::Useless::GetPageInfo(pm, v).phys; }

    void MapRange(pagemap_t *pm, uint64_t vaddr, uint64_t paddr, uint64_t flags, uint64_t count){
        uint64_t mapped = 0;
        while (mapped < count) {
            uint64_t cv = vaddr + mapped * PAGE_SIZE;
            uint64_t cp = paddr + mapped * PAGE_SIZE;
            uint64_t rem = count - mapped;
            if ((cv & (PAGE_1GB-1))==0 && (cp & (PAGE_1GB-1))==0 && rem >= 262144) {
                VMM::Map1G(pm, cv, cp, flags); mapped += 262144; continue;
            }
            if ((cv & (PAGE_2MB-1))==0 && (cp & (PAGE_2MB-1))==0 && rem >= 512) {
                VMM::Map2M(pm, cv, cp, flags); mapped += 512; continue;
            }
            VMM::Map4K(pm, cv, cp, flags); mapped += 1;
        }
    }

    pagemap_t *SwitchPageMap(pagemap_t *pm){
        if (unlikely(!pm)) return nullptr;
        uint64_t rflags;
        asm volatile("pushfq\n\tpop %0\n\tcli" : "=r"(rflags) :: "memory");
        pagemap_t *old = nullptr;
        if (likely(smp_started)) { old = this_cpu()->pagemap; this_cpu()->pagemap = pm; }
        uint64_t cr3 = PHYSICAL((uint64_t)pm->toplvl);
        if (likely(CPUFeatures::has_pcid && pm->pcid != 0)) { cr3 |= pm->pcid; cr3 |= (1ULL << 63); }
        __asm__ volatile ("movq %0, %%cr3" : : "r"(cr3) : "memory");
        if (likely(smp_started)) { if (old) LazyTLB::OnDetach(old); LazyTLB::OnAttach(pm); }
        asm volatile("push %0\n\tpopfq" :: "r"(rflags) : "memory");
        return old;
    }

    pagemap_t *NewPM(){
        pagemap_t *pm = HIGHER_HALF((pagemap_t*)PMM::Request());
        pm->toplvl = HIGHER_HALF((uint64_t*)PMM::Request());
        _memset(pm->toplvl, 0, PAGE_SIZE);
        pm->vm_mappings = nullptr; pm->vma_lock = 0; pm->pt_lock = 0;
        pm->vma_head = pm->vma_cursor = nullptr;
        pm->pcid = AllocPCID();
        BitmapClearAll(pm->cpus_with_tlb);
        rb_root_init(&pm->vma_tree, nullptr, nullptr, nullptr, nullptr, nullptr);
        __memcpy(&pm->toplvl[256], &kernel_pagemap->toplvl[256], 256 * sizeof(uint64_t));
        VMM::VMA::SetStart(pm, HIGHER_HALF(0x100000000000), 0);
        return pm;
    }

    vm_mapping_t *NewMapping(pagemap_t *pm, uint64_t start, uint64_t pc, uint64_t flags){
        vm_mapping_t *m = HIGHER_HALF((vm_mapping_t*)PMM::Request());
        m->start = start; m->page_count = pc; m->flags = flags;
        if (likely(pm->vm_mappings)) {
            m->prev = pm->vm_mappings->prev; m->next = pm->vm_mappings;
            pm->vm_mappings->prev->next = m; pm->vm_mappings->prev = m;
            return m;
        }
        m->prev = m->next = m; pm->vm_mappings = m; return m;
    }

    void RemoveMapping(vm_mapping_t *m){
        m->next->prev = m->prev; m->prev->next = m->next;
        PMM::Free(PHYSICAL((void*)m));
    }

    /* ---- 共享区域: 逐 4K 递减引用计数, 归零才 PMM::Free ---- */
    static void FreeSharedRegion(pagemap_t *pm, uint64_t start, uint64_t end) {
        uint64_t v = start;
        while (v < end) {
            Useless::PageInfo info = VMM::Useless::GetPageInfo(pm, v);
            if (info.size == 0) { v += PAGE_SIZE; continue; }
            if (info.size > PAGE_SIZE) {
                /* 大页: 清整块 PTE, 逐 4K 子页处理引用计数 */
                VMM::UnmapNoFlush(pm, v);
                uint64_t sub = info.size / PAGE_SIZE;
                for (uint64_t k = 0; k < sub && v + k * PAGE_SIZE < end; k++) {
                    uint64_t phys = info.phys + k * PAGE_SIZE;
                    if (RefDecPhys(phys)) PMM::Free((void*)phys);
                }
                v += info.size;
            } else {
                VMM::UnmapNoFlush(pm, v);
                if (RefDecPhys(info.phys)) PMM::Free((void*)info.phys);
                v += PAGE_SIZE;
            }
        }
    }

    /* ---- 非共享: 高效大页释放, 不碰引用计数树 ---- */
    static void FreeOwnedRegion(pagemap_t *pm, uint64_t start, uint64_t end) {
        uint64_t v = start;
        while (v < end) {
            Useless::PageInfo info = VMM::Useless::GetPageInfo(pm, v);
            if (info.size == 0) { v += PAGE_SIZE; continue; }
            if (info.size == PAGE_1GB) {
                Useless::PageInfo next = VMM::Useless::GetPageInfo(pm, v + PAGE_1GB);
                if (next.size == PAGE_1GB && next.phys == info.phys + PAGE_1GB) {
                    PMM::Free2GB((void*)info.phys);
                    VMM::UnmapNoFlush(pm, v); VMM::UnmapNoFlush(pm, v + PAGE_1GB);
                    v += PAGE_2GB;
                } else {
                    for (uint32_t j = 0; j < 512; j++) PMM::Free2MB((void*)(info.phys + j * PAGE_2MB));
                    VMM::UnmapNoFlush(pm, v); v += PAGE_1GB;
                }
            } else if (info.size == PAGE_2MB) {
                PMM::Free2MB((void*)info.phys);
                VMM::UnmapNoFlush(pm, v); v += PAGE_2MB;
            } else {
                PMM::Free((void*)info.phys);
                VMM::UnmapNoFlush(pm, v); v += PAGE_SIZE;
            }
        }
    }

    void *Alloc(pagemap_t *pm, uint64_t pc, bool user) {
        if (unlikely(!pc)) return nullptr;
        uint64_t flags = MM_READ | MM_WRITE | (user ? MM_USER : 0);
        spinlock_lock(&pm->vma_lock);
        uint64_t addr = VMM::VMA::InternalAlloc(pm, pc, flags, 0);
        if (unlikely(!addr)) { spinlock_unlock(&pm->vma_lock); return nullptr; }
        spinlock_lock(&pm->pt_lock);
        uint64_t mapped = 0;
        while (mapped < pc) {
            uint64_t cv = addr + mapped * PAGE_SIZE; uint64_t rem = pc - mapped;
            if (rem >= 524288 && (cv % PAGE_2GB) == 0) {
                void* p = PMM::Request2GB();
                if (p) { uint64_t ph=(uint64_t)p; VMM::Map1G(pm,cv,ph,flags); VMM::Map1G(pm,cv+PAGE_1GB,ph+PAGE_1GB,flags); mapped+=524288; continue; }
            }
            if (rem >= 512 && (cv % PAGE_2MB) == 0) {
                void* p = PMM::Request2MB();
                if (p) { VMM::Map2M(pm,cv,(uint64_t)p,flags); mapped+=512; continue; }
            }
            void* p = PMM::Request();
            if (unlikely(!p)) { kerrorln("PMM: OOM in Alloc"); goto err_a; }
            VMM::Map4K(pm, cv, (uint64_t)p, flags); mapped += 1;
        }
        VMM::NewMapping(pm, addr, pc, flags);
        spinlock_unlock(&pm->pt_lock); spinlock_unlock(&pm->vma_lock);
        return (void*)addr;
    err_a:
        FreeOwnedRegion(pm, addr, addr + mapped * PAGE_SIZE);
        LazyTLB::ShootdownFull(pm);
        vma_region_t *r = VMM::VMA::FindRegion(pm, addr);
        if (r) VMM::VMA::RemoveRegion(pm, r);
        spinlock_unlock(&pm->pt_lock); spinlock_unlock(&pm->vma_lock);
        return nullptr;
    }

    void *EAlloc(pagemap_t *pm, uint64_t pc, uint64_t flags) {
        if (unlikely(!pc)) return nullptr;
        spinlock_lock(&pm->vma_lock);
        uint64_t addr = VMM::VMA::InternalAlloc(pm, pc, flags, 0);
        if (unlikely(!addr)) { spinlock_unlock(&pm->vma_lock); return nullptr; }
        spinlock_lock(&pm->pt_lock);
        uint64_t mapped = 0;
        while (mapped < pc) {
            uint64_t cv = addr + mapped * PAGE_SIZE; uint64_t rem = pc - mapped;
            if (rem >= 524288 && (cv % PAGE_2GB) == 0) {
                void* p = PMM::Request2GB();
                if (p) { uint64_t ph=(uint64_t)p; VMM::Map1G(pm,cv,ph,flags); VMM::Map1G(pm,cv+PAGE_1GB,ph+PAGE_1GB,flags); mapped+=524288; continue; }
            }
            if (rem >= 512 && (cv % PAGE_2MB) == 0) {
                void* p = PMM::Request2MB();
                if (p) { VMM::Map2M(pm,cv,(uint64_t)p,flags); mapped+=512; continue; }
            }
            void* p = PMM::Request();
            if (unlikely(!p)) { kerrorln("PMM: OOM in EAlloc"); goto err_e; }
            VMM::Map4K(pm, cv, (uint64_t)p, flags); mapped += 1;
        }
        VMM::NewMapping(pm, addr, pc, flags);
        spinlock_unlock(&pm->pt_lock); spinlock_unlock(&pm->vma_lock);
        return (void*)addr;
    err_e:
        FreeOwnedRegion(pm, addr, addr + mapped * PAGE_SIZE);
        LazyTLB::ShootdownFull(pm);
        vma_region_t *r = VMM::VMA::FindRegion(pm, addr);
        if (r) VMM::VMA::RemoveRegion(pm, r);
        spinlock_unlock(&pm->pt_lock); spinlock_unlock(&pm->vma_lock);
        return nullptr;
    }

    void Free(pagemap_t *pm, void *ptr){
        if (unlikely(!pm || !ptr)) return;
        if (((uint64_t)ptr & 0xfff) != 0) return;
        spinlock_lock(&pm->vma_lock);
        vma_region_t *region = VMM::VMA::FindRegion(pm, (uint64_t)ptr);
        if (unlikely(!region || region->start != (uint64_t)ptr)) { spinlock_unlock(&pm->vma_lock); return; }
        pm->vma_cursor = region->prev;
        spinlock_lock(&pm->pt_lock);
        uint64_t v = region->start, end = v + region->page_count * PAGE_SIZE;
        if (region->flags & VMM_SHARED_BIT) FreeSharedRegion(pm, v, end);
        else                                  FreeOwnedRegion(pm, v, end);
        LazyTLB::ShootdownFull(pm);
        vm_mapping_t *m = pm->vm_mappings;
        if (m) { vm_mapping_t *sm = m; do { if (m->start == (uint64_t)ptr) { RemoveMapping(m); break; } m = m->next; } while (m != sm); }
        VMM::VMA::RemoveRegion(pm, region);
        spinlock_unlock(&pm->pt_lock); spinlock_unlock(&pm->vma_lock);
    }

    pagemap_t *Fork(pagemap_t *parent){
        pagemap_t *pm = HIGHER_HALF((pagemap_t*)PMM::Request());
        pm->toplvl = HIGHER_HALF((uint64_t*)PMM::Request());
        _memset(pm->toplvl, 0, PAGE_SIZE);
        for (uint64_t i = 256; i < 512; i++) pm->toplvl[i] = kernel_pagemap->toplvl[i];
        pm->pt_lock = 0; pm->vma_lock = 0; pm->vm_mappings = nullptr;
        pm->vma_head = pm->vma_cursor = nullptr;
        pm->pcid = AllocPCID(); BitmapClearAll(pm->cpus_with_tlb);
        rb_root_init(&pm->vma_tree, nullptr, nullptr, nullptr, nullptr, nullptr);
        spinlock_lock(&parent->vma_lock); spinlock_lock(&parent->pt_lock);
        if (likely(parent->vma_head)) {
            VMM::VMA::SetStart(pm, parent->vma_head->start, 0);
            vma_region_t *r = parent->vma_head;
            do {
                if (r->start >= HIGHER_HALF(0)) { r = r->next; continue; }
                uint64_t v = r->start, mapped = 0;
                while (mapped < r->page_count) {
                    Useless::PageInfo info = VMM::Useless::GetPageInfo(parent, v);
                    if (info.size == 0) break;
                    uint64_t nf = (info.flags & ~MM_WRITE) | VMM_COW_BIT;
                    if (info.size == PAGE_1GB) {
                        VMM::Map1G(pm, v, info.phys, nf); VMM::Map1G(parent, v, info.phys, nf);
                        for (uint64_t k = 0; k < PAGE_1GB/PAGE_SIZE; k++) RefSharedPhys(info.phys + k*PAGE_SIZE);
                        mapped += 262144;
                    } else if (info.size == PAGE_2MB) {
                        VMM::Map2M(pm, v, info.phys, nf); VMM::Map2M(parent, v, info.phys, nf);
                        for (uint64_t k = 0; k < PAGE_2MB/PAGE_SIZE; k++) RefSharedPhys(info.phys + k*PAGE_SIZE);
                        mapped += 512;
                    } else {
                        VMM::Map4K(pm, v, info.phys, nf); VMM::Map4K(parent, v, info.phys, nf);
                        RefSharedPhys(info.phys); mapped += 1;
                    }
                    v += info.size;
                }
                VMM::VMA::AddRegion(pm, r->start, r->page_count, r->flags | VMM_SHARED_BIT);
                VMM::NewMapping(pm, r->start, r->page_count, r->flags | VMM_SHARED_BIT);
                r = r->next;
            } while (r != parent->vma_head);
        }
        LazyTLB::ShootdownFull(parent);
        spinlock_unlock(&parent->pt_lock); spinlock_unlock(&parent->vma_lock);
        return pm;
    }

    void CleanPM(pagemap_t *pm){
        if (unlikely(!pm)) return;
        spinlock_lock(&pm->vma_lock); spinlock_lock(&pm->pt_lock);
        if (pm->vma_head) {
            vma_region_t *r = pm->vma_head->next;
            while (r != pm->vma_head) {
                vma_region_t *next = r->next;
                if (likely(next != pm->vma_head)) PREFETCH_RH(next);
                if (r->start < HIGHER_HALF(0)) {
                    uint64_t v = r->start, end = v + r->page_count * PAGE_SIZE;
                    if (r->flags & VMM_SHARED_BIT) FreeSharedRegion(pm, v, end);
                    else                            FreeOwnedRegion(pm, v, end);
                }
                VMM::VMA::RemoveRegion(pm, r); r = next;
            }
            PMM::Free(PHYSICAL(pm->vma_head));
            pm->vma_head = pm->vma_cursor = nullptr;
        }
        vm_mapping_t *mapping = pm->vm_mappings;
        if (mapping) {
            size_t mc = 0; vm_mapping_t *curr = mapping;
            do { mc++; curr = curr->next; } while (curr != mapping);
            curr = mapping;
            for (size_t i = 0; i < mc; i++) { vm_mapping_t *next = curr->next; VMM::RemoveMapping(curr); curr = next; }
        }
        pm->vm_mappings = nullptr;
        LazyTLB::ShootdownFull(pm);
        spinlock_unlock(&pm->pt_lock); spinlock_unlock(&pm->vma_lock);
    }

    static void FreePageTablesInternal(uint64_t *table, int level) {
        for (int i = 0; i < 256; i++) {
            uint64_t entry = table[i];
            if (!(entry & 0x1)) continue;
            if (entry & VMM_PS_BIT) continue;
            uint64_t *child = HIGHER_HALF(PTE_MASK(entry));
            if (level > 1) { FreePageTablesInternal(child, level - 1); PMM::Free(PHYSICAL(child)); }
        }
    }

    void DestroyPM(pagemap_t *pm){
        if (unlikely(!pm)) return;
        VMM::CleanPM(pm);
        int sl = IsPM5LVL ? 5 : 4;
        FreePageTablesInternal(pm->toplvl, sl);
        FreePCID(pm->pcid);
        PMM::Free(PHYSICAL(pm->toplvl)); PMM::Free(PHYSICAL(pm));
    }

    uint32_t HandlePF(context_t *ctx){
        uint64_t cr2 = 0; __asm__ volatile ("movq %%cr2, %0" : "=r"(cr2));
        uint64_t ec = ctx->error_code;
        bool p = ec & 1, wr = ec & 2, us = ec & 4, id = ec & 16;

        thread_t *t = Schedule::this_thread();
        if (unlikely(!smp_started || !this_cpu() || !t)) {
            kerror("[#PF] addr=%p rip=%p ec=%#lx : no thread context\n", cr2, ctx->rip, ec);
            return 1;
        }
        pagemap_t *pm = t->pagemap;
        if (unlikely(!pm)) return 1;

        static uint64_t rf_cr2[MAX_CPU], rf_rip[MAX_CPU];
        static uint32_t rf_cnt[MAX_CPU];
        uint32_t cid = this_cpu()->id; if (cid >= MAX_CPU) cid = 0;
        if (unlikely(rf_cr2[cid] == cr2 && rf_rip[cid] == ctx->rip)) {
            if (++rf_cnt[cid] >= 3) {
                kerrorln("[#PF] repeated fault x3 -> kill"); rf_cnt[cid] = 0; return 1;
            }
        } else { rf_cr2[cid] = cr2; rf_rip[cid] = ctx->rip; rf_cnt[cid] = 1; }

        uint64_t fault_addr = ALIGN_DOWN(cr2, PAGE_SIZE);
        spinlock_lock(&pm->pt_lock);
        Useless::PageInfo info = VMM::Useless::GetPageInfo(pm, fault_addr);

        auto segv = [&](const char *why) -> uint32_t {
            spinlock_unlock(&pm->pt_lock);
            kerrorln("Segfault: %s (addr=%p rip=%p)", why, cr2, ctx->rip);
            return 1;
        };
        if (info.size == 0) return segv("unmapped");
        if (id) return segv("NX fetch");
        if (us && !(info.flags & MM_USER)) return segv("user->supervisor");
        if (!wr) return segv("read fault");

        if (!(info.flags & VMM_COW_BIT)) {
            if (info.flags & MM_WRITE) { LazyTLB::LocalInvlpg(pm, fault_addr); spinlock_unlock(&pm->pt_lock); return 0; }
            return segv("write to RO");
        }

        uint64_t new_flags = (info.flags & ~VMM_COW_BIT) | MM_WRITE;
        if (info.size == PAGE_1GB) {
            uint64_t base = fault_addr & ~(PAGE_1GB-1);
            uint64_t sub = fault_addr & (PAGE_1GB-1) & ~(PAGE_2MB-1);
            VMM::UnmapNoFlush(pm, base);
            for (uint64_t off = 0; off < PAGE_1GB; off += PAGE_2MB) {
                if (off == sub) continue;
                VMM::Map2M(pm, base+off, info.phys+off, info.flags);
            }
            uint64_t np = (uint64_t)PMM::Request2MB();
            if (unlikely(!np)) { spinlock_unlock(&pm->pt_lock); kerrorln("OOM 1G CoW"); return 1; }
            __memcpy(HIGHER_HALF((void*)np), HIGHER_HALF((void*)(info.phys+sub)), PAGE_2MB);
            VMM::Map2M(pm, base+sub, np, new_flags);
            for (uint64_t k = 0; k < PAGE_2MB/PAGE_SIZE; k++)
                if (RefDecPhys(info.phys+sub+k*PAGE_SIZE)) PMM::Free((void*)(info.phys+sub+k*PAGE_SIZE));
            LazyTLB::ShootdownPage(pm, base);
        } else if (info.size == PAGE_2MB) {
            uint64_t base = fault_addr & ~(PAGE_2MB-1);
            uint64_t np = (uint64_t)PMM::Request2MB();
            if (unlikely(!np)) { spinlock_unlock(&pm->pt_lock); kerrorln("OOM 2M CoW"); return 1; }
            __memcpy(HIGHER_HALF((void*)np), HIGHER_HALF((void*)info.phys), PAGE_2MB);
            VMM::Map2M(pm, base, np, new_flags);
            for (uint64_t k = 0; k < PAGE_2MB/PAGE_SIZE; k++)
                if (RefDecPhys(info.phys+k*PAGE_SIZE)) PMM::Free((void*)(info.phys+k*PAGE_SIZE));
            LazyTLB::ShootdownPage(pm, base);
        } else {
            uint64_t np = (uint64_t)PMM::Request();
            if (unlikely(!np)) { spinlock_unlock(&pm->pt_lock); kerrorln("OOM 4K CoW"); return 1; }
            __memcpy(HIGHER_HALF((void*)np), HIGHER_HALF((void*)info.phys), PAGE_SIZE);
            VMM::Map4K(pm, fault_addr, np, new_flags);
            if (RefDecPhys(info.phys)) PMM::Free((void*)info.phys);
            LazyTLB::ShootdownPage(pm, fault_addr);
        }
        spinlock_unlock(&pm->pt_lock);
        return 0;
    }
}