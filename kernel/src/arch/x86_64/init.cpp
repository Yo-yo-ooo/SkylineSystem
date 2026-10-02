//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
#include <arch/pinc.h>
#include <drivers/ahci/ahci.h>
#include <drivers/nvme/nvme.h>
#include <drivers/usb/xhci.h>
#include <drivers/ata/ata.h>
#include <drivers/keyboard/x86/keyboard.h>
#include <drivers/dev/dev.h>
#include <fs/lwext4/ext4.h>
#include <fs/lwext4/blockdev/blockdev.h>
#include <fs/fc.h>
#include <arch/x86_64/simd/simd.h>
#include <mem/heap.h>
#include <arch/x86_64/drivers/hpet/hpet.h>
#include <drivers/framebuffer/fb.h>
#include <klib/renderer/rnd.h>
#include <arch/x86_64/interrupt/gdt.h>
#include <arch/x86_64/interrupt/idt.h>
#include <mem/pmm.h>
#include <arch/x86_64/vmm/vmm.h>
#include <arch/x86_64/ioapic/ioapic.h>
#include <arch/x86_64/schedule/sched.h>
#include <arch/x86_64/schedule/sched_bench.h>
#include <arch/x86_64/pit/pit.h>
#include <drivers/net/e1000.h>
/* 诊断门控: 1 = 启动时由专用内核线程在 cpu 1 后台跑 SchedBench。
   进展 (round 23): init 等待是挂点根因 (bootstrap 上下文不能睡眠/忙等),
   已改为不等待+网络延后; 报告已可完整打印 (污染环境下 step/osc 通过)。
   剩余: 相位2 之后挂起 (疑 step 线程死亡持 stats 锁) —— 采集后置 0。 */
/* 诊断门控: 1 = 启动时由专用内核线程在 cpu 1 后台跑 SchedBench。
   进展 (round 24): 四相位全部跑通, 报告完整 (step t90=4ms ✓, osc 迟滞 ✓,
   pollute/shortwin 未过 = 真内核与模型的真实差异, 见 scheduler.md);
   init 等待挂点已解 (不等待 + wrapper 尾部起网络)。采集后置 0。 */
#define SCHED_BENCH_AUTO 0
#if SCHED_BENCH_AUTO   /* P5-92: 原为 #if 1 架空门控; 现与门控一致 */
/* 调度器真实内核画像实验代码 (round 23 调试中): 相位探针定位挂点 */
static volatile uint32_t g_bench_done = 0;
static void bench_wrapper(void) {
    bool ok = SchedBench::Run(1);
    kinfoln("[sched_bench] 完成=%d (实际 base_quantum=%u ms)\n", (int)ok,
            SchedBench::Report()->base_quantum_ms);
    SchedBench::sched_bench_report *r = SchedBench::Report();
    kinfoln("[sched_bench] step: t90=%llu valid=%d t90_pass=%d overshoot=%d sat=%d\n",
            (unsigned long long)r->step.t90_ms, (int)r->step.valid,
            (int)r->step.t90_pass, (int)r->step.overshoot_pass, (int)r->step.sat_pass);
    kinfoln("[sched_bench] pollute: base=[%llu,%llu] mult=[%llu,%llu] pass=%d/%d\n",
            (unsigned long long)r->pollute.base_min, (unsigned long long)r->pollute.base_max,
            (unsigned long long)r->pollute.mult_min, (unsigned long long)r->pollute.mult_max,
            (int)r->pollute.base_pass, (int)r->pollute.mult_pass);
    kinfoln("[sched_bench] shortwin: windows=%llu resets=%llu stalled=%llu mf=%llu msl=%llu pass=%d\n",
            (unsigned long long)r->shortwin.short_windows, (unsigned long long)r->shortwin.base_resets,
            (unsigned long long)r->shortwin.stalled, (unsigned long long)r->shortwin.mf,
            (unsigned long long)r->shortwin.msl, (int)r->shortwin.pass);
    kinfoln("[sched_bench] osc: w=[%llu,%llu] toggles=%llu entered=%d hyst=%d\n",
            (unsigned long long)r->osc.weight_min, (unsigned long long)r->osc.weight_max,
            (unsigned long long)r->osc.toggles, (int)r->osc.entered_osc, (int)r->osc.hyst_ok);
    g_bench_done = 1;
    /* bench 完成后再起网络 (洪泛线程会污染 RIP 反馈测量) */
    NetStackInit();
    while (true) PIT::Sleep(1000);
}
#endif
#include <atomic/atomic.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/lapic/lapic.h>
#include <arch/x86_64/pit/pit.h>
#include <arch/x86_64/rtc/rtc.h>
#include <drivers/mouse/x86/ps2mouse.h>
#include <drivers/net/e1000.h>

#define PIC1_COMMAND 0x20
#define PIC1_DATA 0x21
#define PIC2_DATA 0xA1

uint32_t PrintFSERIAL = 0;

extern cpu_t *bsp_cpu;
extern "C" int32_t file_cache_writeback_callback(
    uint64_t file_id,
    const uint8_t *key, 
    uint32_t key_len, void *data, size_t data_len
);
extern void sys_sysinfo_init(void);

extern void enable_smep_smap();

/* PCI scan -> trigger probe wrappers. Each driver is bound the moment its
   matching PCI function enumerates, so the board init no longer hard-codes
   FindPCIDev() + constructors. USB MSC follows automatically: once the xHCI
   function probes, its downstream enumeration registers mass-storage LUNs. */
static void pci_probe_ahci(PCI::PCIHeader0* h) { new AHCI::AHCIDriver(&h->Header); }
static void pci_probe_nvme(PCI::PCIHeader0* h) { new class NVME(h); }
static void pci_probe_xhci(PCI::PCIHeader0* h) { XHCI::InitXHCIFromPCI(h); }
static void pci_probe_e1000(PCI::PCIHeader0* h) { E1000::Init(h); }

static const PCI::PCIDriver kAhciDriver =
    {"AHCI", 0x01, 0x06, 0x01, pci_probe_ahci};
static const PCI::PCIDriver kNvmeDriver =
    {"NVMe", 0x01, 0x08, 0x02, pci_probe_nvme};
static const PCI::PCIDriver kXhciDriver =
    {"xHCI", 0x0C, 0x03, 0x30, pci_probe_xhci};
static const PCI::PCIDriver kE1000Driver =
    {"e1000", 0x02, 0x00, 0x00, pci_probe_e1000};

void __init x86_64_init(void){
    InitFunc("Serial(Simulater)",Serial::Init());
    WELCOME_X86_64
    kinfo("INIT x86_64 ARCH\n");
    InitFunc("SSE",sse_enable());
    kinfoln("HHDM OFFSET:0x%X",hhdm_offset);

    InitFunc("GDT",GDT::Init(0));
    bsp_cpu->self = bsp_cpu;
    wrmsr(KERNEL_GS_BASE, (uint64_t)bsp_cpu);
    wrmsr(IA32_GS_MSR,(uint64_t)bsp_cpu);
    InitFunc("IDT",idt_init());
    if (fpu_init()){
        kerror("FPU INIT FAILED: x86_64 CPU doesn't support FPU.\n");
        hcf();
    }
    kpok("FPU INIT!\n");
    InitFunc("PMM",PMM::Init());
    InitFunc("VMM",VMM::Init());
    InitFunc("SLAB",SLAB::Init());
#ifdef __x86_64__
    // SLUB fuses into kmalloc only once SLAB itself is live: SLUB::Create borrows
    // SLAB for its cache descriptors, and the self-test mixes both allocators.
    if (SLUB::InitKmalloc()) { kpokln("SLUB: kmalloc caches online (16..1024 B)"); }
    else                     { kpokln("SLUB: kmalloc caches unavailable, using SLAB"); }
    if (SLUB::SelfTest()) { kpokln("SLUB: self-test OK (named cache + kmalloc fusion)"); }
    else                  { kpokln("SLUB: self-test FAILED stage=%u", SLUB::LastFailStage()); }
#endif
    InitFunc("ACPI",ACPI::Init((void*)RSDP_ADDR));
    InitFunc("MADT",MADT_Init());
    //DISABLE PIC
    outb(PIC1_DATA, 0xff);
    outb(PIC2_DATA, 0xff);
    kinfoln("DISABLED PIC!");
    //ENABLE ICMR
    outb(0x22,0x70);
    outb(0x23,0x01);
    kinfoln("ENABLED ICMR!");
    //simd_cpu_init(get_cpu(0));
    InitFunc("LAPIC",LAPIC::Init());
    InitFunc("IOAPIC",IOAPIC::Init());
    InitFunc("PIT & RTC",PIT::InitPIT());
    InitFunc("HPET",HPET::InitHPET());
    bsp_cpu->file_cache = (file_cache_cpu_t*)kmalloc(sizeof(file_cache_cpu_t));
    file_cache_cpu_init(bsp_cpu->file_cache, bsp_cpu->id,file_cache_writeback_callback);
    InitFunc("SMP",smp_init());
    /* Route the legacy PIT (IRQ0/GSI0) to vector 32 on the BSP and unmask it.
       The 8259 PIC is fully masked above, so without this IOAPIC redirection no
       timer IRQ ever arrives: TicksSinceBoot stays at 0 and TimeSinceBootMS() is
       frozen. That stalls EEVDF vruntime accounting, Rate-aware quantum feedback
       and every timeout, which produced the ~14s worker-startup stall and the
       runtime compositor/mouse freeze. */
    IOAPIC::RemapIRQ(bsp_cpu->lapic_id, 0, 32, false);
    InitFunc("RTC",RTC::InitRTC());
    InitFunc("SIMD Core 0",simd_cpu_init(this_cpu()));
    InitFunc("Intel SMEP & SMAP",enable_smep_smap());
    
    InitFunc("Schedule",Schedule::Init());
    InitCPUThread();
    InitFunc("Syscall",syscall_init());
    

    InitFunc("VsDev",Dev::Init());
    InitFunc("File & MP MAN",InitFFMAN());
    //InitFunc("ATA",ATA::Init());
    /* Register PCI drivers first, then enumerate the bus: each matching
       function is probed the instant it is discovered (AHCI / NVMe / xHCI;
       USB mass storage follows automatically once xHCI probes). */
    PCI::RegisterDriver(&kAhciDriver);
    PCI::RegisterDriver(&kNvmeDriver);
    PCI::RegisterDriver(&kXhciDriver);
    PCI::RegisterDriver(&kE1000Driver);
    if(ACPI::mcfg == NULL){PCI::DoPCIWithoutMCFG();}
    else{InitFunc("PCI",PCI::EnumeratePCI(ACPI::mcfg));}

    InitFunc("PS/2 MOUSE(x86)",ps2_mouse_init());
    InitFunc("KEYBOARD(x86)",keyboard_init());
    

    /* P1-50: 根文件系统设备探测列表 (原硬编码 "sata0" —— 纯 NVMe
       机器直接 hcf)。依次尝试常见块设备名, 全部失败才停机。 */
    {
        static const char *kRootCandidates[] = {"sata0", "nvme0", "usb0", "sata1", nullptr};
        bool root_ok = false;
        for (int i = 0; kRootCandidates[i]; i++) {
            if (ext4_kernel_init(kRootCandidates[i], "/mp/", 0)) {
                root_ok = true;
                break;
            }
        }
        if (!root_ok) {
            kerrorln("init: no usable root filesystem device, halting");
            hcf();
        }
    }

    FrameBufferDevice::Init();

    //ext4_fs_test_all();

    Schedule::Install();
    extern void sched_calibrate_tsc(void);
    sched_calibrate_tsc();   /* TSC 校准: RIP 采样执行期分母 (pollute 复测前提) */

#if SCHED_BENCH_AUTO
    {
        proc_t *bproc = Schedule::NewProcess(false);
        if (bproc) {
            /* 不等待: init 的忙等/睡眠都会饿死或挂起 (bootstrap 上下文)。
               bench 完成后 wrapper 自会调用 NetStackInit (网络延后 = 安静机器) */
            Schedule::NewKernelThreadEx(bproc, 0, 8, (void *)bench_wrapper, 8);
        }
    }
#else
    /* 网络栈: e1000 已在 PCI 枚举时探测完成, 调度器就绪后上线 lwIP */
    NetStackInit();
#endif

    atomic_store_4(&PrintFSERIAL,1,0);
    sys_sysinfo_init();

    proc_t *proc = Schedule::NewProcess(true);

    kinfoln("Creating desktop process...");
    /* 审计卫生 (round 19): 原 VMM::Alloc(1000)+Free 为无消费者的
       疑似调试遗留 (4MB 引导开销), 删除 */
    char *argv[] = {(char*)"Test Main Thread"};
    char *envp[] = {nullptr};
    thread_t *desktop = Schedule::NewThread(proc, 0, 0, 
        "/mp/desktop.elf", 1, argv, envp);  
    (void)desktop;
    proc_t *proc2 = Schedule::NewProcess(true);
    thread_t *desktop2 = Schedule::NewThread(proc2, 0, 1, 
        "/mp/hw.elf", 1, argv, envp);  
    (void)desktop2;
    /*proc_t *proc3 = Schedule::NewProcess(true);
    thread_t *desktop3 = Schedule::NewThread(proc3, 0, 1, 
        "/mp/hw2.elf", 1, argv, envp);   */
    kinfoln("desktop PROCESS: %d",proc->id);
    asm volatile("sti");
    LAPIC::IPI(smp_bsp_cpu,SCHED_VEC);
    LAPIC::IPIOthers(smp_bsp_cpu, SCHED_VEC);

    while (true) {
        asm volatile("hlt");
    }
}