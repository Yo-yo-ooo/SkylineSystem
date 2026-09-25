//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
#include <drivers/usb/xhci.h>
#include <drivers/usb/usb_descriptor.h>
#include <drivers/usb/usb_device.h>
#include <drivers/usb/hid.h>
#include <drivers/usb/msc.h>
#include <drivers/usb/uac.h>
#include <drivers/usb/uvc.h>
#include <klib/kio.h>
#include <klib/kprintf.h>
#include <klib/algorithm/rbtree.h>
#include <arch/x86_64/vmm/vmm.h>
#include <arch/x86_64/interrupt/idt.h>

#ifdef __x86_64__
#include <arch/x86_64/pit/pit.h>
#include <arch/x86_64/schedule/sched.h>

static inline void xhci_cpuid0(uint32_t leaf, uint32_t vals[4]) {
    asm volatile("cpuid"
        : "=a"(vals[0]), "=b"(vals[1]), "=c"(vals[2]), "=d"(vals[3])
        : "0"(leaf));
}

static inline uint64_t xhci_rdtsc0(void) {
    uint32_t lo, hi;
    asm volatile("lfence\n\trdtsc" : "=a"(lo), "=d"(hi) :: "memory");
    return ((uint64_t)hi << 32) | lo;
}

// TSC ticks per microsecond. Prefer CPUID leaf 0x15 (core crystal clock
// ratio), then leaf 0x16 (nominal TSC frequency in MHz), and fall back to a
// conservative 1 GHz assumption.
static uint64_t xhci_tsc_per_usec(void) {
    uint32_t r[4];
    xhci_cpuid0(0x15, r);
    if (r[0] != 0 && r[1] != 0 && r[2] != 0) {
        uint64_t tscHz = (uint64_t)r[2] * r[1] / r[0];
        if (tscHz >= 1000000) return tscHz / 1000000ULL;
    }
    xhci_cpuid0(0x16, r);
    if (r[1] != 0) return (uint64_t)r[1]; // EBX = nominal TSC MHz
    return 1000;
}

// Interrupt-independent microsecond delay used during early controller
// bring-up, where IF may be clear and PIT ticks therefore cannot advance.
static void xhci_udelay_poll(uint64_t usec) {
    static uint64_t perUsec = 0;
    if (!perUsec) perUsec = xhci_tsc_per_usec();
    uint64_t start = xhci_rdtsc0();
    uint64_t target = perUsec * usec;
    while (xhci_rdtsc0() - start < target) asm volatile("pause");
}

void usleep_usec(uint64_t usec){
    uint64_t rf;
    asm volatile("pushfq\n\tpop %0" : "=r"(rf));
    if ((rf >> 9) & 1) {
        // Interrupts are live: use the tick-driven sleep (which also yields).
        // PIT::Sleep takes milliseconds; round up so a sub-millisecond wait
        // still covers at least one timer tick instead of returning at once.
        PIT::Sleep((usec + 999) / 1000);
    } else {
        // Early enumeration runs with interrupts masked (the driver starts
        // before the global sti): busy-wait on the TSC so we never spin
        // forever waiting for a PIT tick that cannot arrive.
        xhci_udelay_poll(usec);
    }
}
#endif

extern "C" void XHCI_IRQHandler(context_t* ctx) {
    (void)ctx;
    XHCI::PollEventRing();
}

namespace XHCI {

static CapRegs* g_cap = nullptr;
static OpRegs* g_op = nullptr;
static IntRegSet* g_intRegs = nullptr;
static uint32_t* g_doorbells = nullptr;
static uint8_t g_maxSlots = 0, g_maxPorts = 0;

static uint64_t* g_dcbaap = nullptr;
static uint64_t* g_scratchpadBufArray = nullptr;
static void* g_scratchpadBuffers = nullptr;

static volatile TRB* g_cmdRing = nullptr;
static volatile TRB* g_cmdRingEnq = nullptr;
static uint8_t g_cmdRingCycle = 1;
static spinlock_t g_cmdRingLock = 0;

static volatile TRB* g_evtRing = nullptr;
static volatile TRB* g_evtRingDeq = nullptr;
static uint8_t g_evtRingCycle = 1;
static ERSTEntry* g_erst = nullptr;
static spinlock_t g_evtRingLock = 0;

static SlotInfo g_slots[MAX_SLOTS];
// Set while a port is being reset/enumerated so a Port Status Change event
// delivered during the blocking enumeration cannot re-enter HandlePortChange.
static bool g_portBusy[256];

struct PendingCommand {
    rb_node_t node; uint64_t trbPtr;
    volatile bool completed; CC completionCode; uint8_t slotID;
};

struct PendingTransfer {
    rb_node_t node; uint64_t trbPtr;
    volatile bool completed; CC completionCode; uint32_t transferred;
    bool async; void* callback; void* ctx; void* buf; uint32_t len;
    uint8_t slotID;
};

static rb_root_t g_pendingCmds;
static rb_root_t g_pendingTransfers;
static spinlock_t g_pendingCmdsLock = 0;
static spinlock_t g_pendingTransfersLock = 0;

static int pending_cmd_cmp(const rb_node_t *a, const rb_node_t *b) {
    PendingCommand *ea = container_of(a, PendingCommand, node);
    PendingCommand *eb = container_of(b, PendingCommand, node);
    if (ea->trbPtr < eb->trbPtr) return -1;
    if (ea->trbPtr > eb->trbPtr) return 1;
    return 0;
}

static int pending_xfer_cmp(const rb_node_t *a, const rb_node_t *b) {
    PendingTransfer *ea = container_of(a, PendingTransfer, node);
    PendingTransfer *eb = container_of(b, PendingTransfer, node);
    if (ea->trbPtr < eb->trbPtr) return -1;
    if (ea->trbPtr > eb->trbPtr) return 1;
    return 0;
}

static void* allocDMA(size_t bytes) {
    size_t pages = (bytes + 0xFFF) >> 12;
    void *p = VMM::Alloc((pagemap_t*)kernel_pagemap, pages, false);
    if (p) _memset(p, 0, pages << 12);
    return p;
}

static void freeDMA(void* p) {
    if (p) VMM::Free((pagemap_t*)kernel_pagemap, p);
}

// DMA buffers come from VMM::Alloc, whose virtual addresses are not in the
// HHDM, so resolve the physical frame through the page map instead of
// subtracting hhdm_offset.
static inline uint64_t virt_to_phys(void* v) {
    uint64_t va = (uint64_t)v;
    // GetPhysics resolves only the page-frame base; OR in the within-page
    // offset so a buffer not aligned to a page still gets the correct
    // physical TRB / buffer address.
    return VMM::GetPhysics((pagemap_t*)kernel_pagemap, va) | (va & 0xFFF);
}
static inline void writeOp(uint32_t off, uint32_t v) { *((volatile uint32_t*)((uint64_t)g_op + off)) = v; }
static inline uint32_t readOp(uint32_t off) { return *((volatile uint32_t*)((uint64_t)g_op + off)); }
static inline void ringDoorbell(uint8_t slot, uint8_t target) { *((volatile uint32_t*)((uint64_t)g_doorbells + (slot * 4))) = target; }

static bool EnqueueAndTrack(TRBRingState* ring, uint8_t slotID, uint8_t dbTarget, const TRB& trb, PendingTransfer* pt) {
    uint64_t flags;
    usb_spin_lock_irqsave(&ring->lock, flags);
    
    uint32_t idx = ((uint64_t)ring->enqueue - (uint64_t)ring->base) / sizeof(TRB);
    volatile TRB* next = (idx == TRANSFER_RING_SIZE - 1) ? ring->base : ring->enqueue + 1;
    
    if (next == ring->dequeue) {
        usb_spin_unlock_irqrestore(&ring->lock, flags);
        return false; 
    }

    pt->trbPtr = (idx == TRANSFER_RING_SIZE - 1) ? virt_to_phys((void*)ring->base) : virt_to_phys((void*)ring->enqueue);
    pt->slotID = slotID;
    
    uint64_t pflags;
    usb_spin_lock_irqsave(&g_pendingTransfersLock, pflags);
    rb_insert(&g_pendingTransfers, &pt->node, pending_xfer_cmp);
    usb_spin_unlock_irqrestore(&g_pendingTransfersLock, pflags);

    if (idx == TRANSFER_RING_SIZE - 1) {
        ring->enqueue->parameter = virt_to_phys((void*)ring->base);
        ring->enqueue->status = 0;
        ring->enqueue->control = (TRB_LINK << 10) | (ring->cycle << 0) | (1u << 1);
        ring->cycle ^= 1;
        ring->enqueue = ring->base;
    }
    
    ring->enqueue->parameter = trb.parameter;
    ring->enqueue->status = trb.status;
    ring->enqueue->control = (trb.control & ~1u) | ring->cycle;
    ring->enqueue++;
    
    usb_spin_unlock_irqrestore(&ring->lock, flags);
    ringDoorbell(slotID, dbTarget);
    return true;
}

static void processEvent(volatile TRB* evt) {
    uint8_t trbType = (evt->control >> 10) & 0x3F;
    // Completion Code is the high byte of the TRB Status field; the low 24
    // bits hold the TRB Transfer Length.
    uint8_t cc = (evt->status >> 24) & 0xFF;
    uint8_t slotID = (evt->control >> 24) & 0xFF;

    switch (trbType) {
        case TRB_TRANSFER_EVENT: {
            uint64_t trbPtr = evt->parameter;
            uint32_t xferred = evt->status & 0xFFFFFFu;

            // 1. Advance the ring dequeue pointer past the completed TRB.
            if (slotID > 0 && slotID <= MAX_SLOTS) {
                for (int i = 0; i < 31; i++) {
                    TRBRingState* ring = &g_slots[slotID].rings[i];
                    if (ring->base) {
                        uint64_t rs = virt_to_phys((void*)ring->base);
                        uint64_t re = rs + TRANSFER_RING_SIZE * sizeof(TRB);
                        if (trbPtr >= rs && trbPtr < re) {
                            // Resolve the completed TRB by its physical index
                            // in the ring; the ring VA is not in the HHDM, so
                            // do not convert via hhdm_offset.
                            uint32_t physIdx = (uint32_t)((trbPtr - rs) / sizeof(TRB));
                            uint64_t rf;
                            usb_spin_lock_irqsave(&ring->lock, rf);
                            ring->dequeue = (physIdx == TRANSFER_RING_SIZE - 1)
                                ? ring->base : ring->base + physIdx + 1;
                            usb_spin_unlock_irqrestore(&ring->lock, rf);
                            break;
                        }
                    }
                }
            }

            // 2. Resolve the pending transfer.
            PendingTransfer* async_pt = nullptr;
            uint64_t flags;
            usb_spin_lock_irqsave(&g_pendingTransfersLock, flags);
            PendingTransfer key; key.trbPtr = trbPtr;
            rb_node_t* found = rb_search(&g_pendingTransfers, &key.node, pending_xfer_cmp);
            if (found) {
                PendingTransfer* pt = container_of(found, PendingTransfer, node);
                if (pt->async) {
                    rb_erase(&g_pendingTransfers, &pt->node);
                    async_pt = pt;
                } else {
                    pt->completionCode = (CC)cc;
                    pt->transferred = xferred;
                    pt->completed = true;
                }
            }
            usb_spin_unlock_irqrestore(&g_pendingTransfersLock, flags);

            // 3. Run async callbacks outside the lock.
            if (async_pt) {
                ((void(*)(uint8_t*, uint32_t, void*))async_pt->callback)((uint8_t*)async_pt->buf, xferred, async_pt->ctx);
                kfree(async_pt);
            }
            break;
        }
        case TRB_COMMAND_COMPLETION: {
            uint64_t cmdTrbPtr = evt->parameter;
            uint64_t flags;
            usb_spin_lock_irqsave(&g_pendingCmdsLock, flags);
            PendingCommand key; key.trbPtr = cmdTrbPtr;
            rb_node_t* found = rb_search(&g_pendingCmds, &key.node, pending_cmd_cmp);
            if (found) {
                PendingCommand* pc = container_of(found, PendingCommand, node);
                pc->completionCode = (CC)cc;
                pc->slotID = slotID;
                pc->completed = true;
            }
            usb_spin_unlock_irqrestore(&g_pendingCmdsLock, flags);
            break;
        }
        case TRB_PORT_STATUS_CHANGE: {
            uint8_t port = (evt->parameter >> 24) & 0xFF;
            HandlePortChange(port);
            break;
        }
        default: break;
    }
}

void PollEventRing() {
    uint64_t flags;
    usb_spin_lock_irqsave(&g_evtRingLock, flags);
    while (true) {
        volatile TRB* trb = g_evtRingDeq;
        uint8_t cv = (trb->control & 1u);
        if (cv != g_evtRingCycle) break;

        TRB evtCopy;
        __memcpy(&evtCopy, (const void*)trb, sizeof(TRB));
        g_evtRingDeq++;
        if (((uint64_t)g_evtRingDeq - (uint64_t)g_evtRing) >= EVT_RING_SIZE * sizeof(TRB)) {
            g_evtRingDeq = g_evtRing;
            g_evtRingCycle ^= 1;
        }
        usb_spin_unlock_irqrestore(&g_evtRingLock, flags);

        processEvent(&evtCopy);

        usb_spin_lock_irqsave(&g_evtRingLock, flags);
    }

    // Publish the new Event Ring Dequeue Pointer (bit3 = EHB).
    uint64_t erdp = virt_to_phys((void*)g_evtRingDeq) | (1u << 3);
    g_intRegs->erdp_lo = (uint32_t)erdp;
    g_intRegs->erdp_hi = (uint32_t)(erdp >> 32);
    usb_spin_unlock_irqrestore(&g_evtRingLock, flags);
}

uint8_t SubmitCommandBlocking(volatile TRB* cmdTrb, uint32_t timeoutMs) {
    PendingCommand pc;
    rb_init_node(&pc.node);
    pc.completed = false;

    uint64_t flags;
    usb_spin_lock_irqsave(&g_cmdRingLock, flags);
    uint32_t idx = ((uint64_t)g_cmdRingEnq - (uint64_t)g_cmdRing) / sizeof(TRB);
    if (idx == CMD_RING_SIZE - 1) {
        g_cmdRingEnq->parameter = virt_to_phys((void*)g_cmdRing);
        g_cmdRingEnq->status = 0;
        g_cmdRingEnq->control = (TRB_LINK << 10) | (g_cmdRingCycle << 0) | (1u << 1);
        g_cmdRingCycle ^= 1;
        g_cmdRingEnq = g_cmdRing;
    }
    pc.trbPtr = virt_to_phys((void*)g_cmdRingEnq);
    g_cmdRingEnq->parameter = cmdTrb->parameter;
    g_cmdRingEnq->status = cmdTrb->status;
    g_cmdRingEnq->control = (cmdTrb->control & ~1u) | g_cmdRingCycle;
    g_cmdRingEnq++;
    usb_spin_unlock_irqrestore(&g_cmdRingLock, flags);

    usb_spin_lock_irqsave(&g_pendingCmdsLock, flags);
    rb_insert(&g_pendingCmds, &pc.node, pending_cmd_cmp);
    usb_spin_unlock_irqrestore(&g_pendingCmdsLock, flags);

    ringDoorbell(0, 0);
    
    for(uint32_t i=0; i<timeoutMs*1000; i++) {
        if (pc.completed) break;
        PollEventRing();
        if (pc.completed) break;
        usleep_usec(1);
    }

    usb_spin_lock_irqsave(&g_pendingCmdsLock, flags);
    rb_erase(&g_pendingCmds, &pc.node);
    usb_spin_unlock_irqrestore(&g_pendingCmdsLock, flags);
    
    if (!pc.completed) return 0;
    return pc.slotID;
}

static bool resetController() {
    writeOp(offsetof(OpRegs, usbcmd), USBCMD_HCRST);
    for (uint32_t i = 0; i < 100000; i++) {
        if (!(readOp(offsetof(OpRegs, usbsts)) & USBSTS_CNR)) return true;
    }
    return false;
}

static bool takeFromBIOS(PCI::PCIHeader0* hdr) {
    if (hdr->CapabilitiesPtr == 0) return true;
    uint8_t* ptr = (uint8_t*)((uint64_t)hdr + hdr->CapabilitiesPtr);
    while (true) {
        if (ptr[0] == 0x01) {
            uint32_t* usblegsup = (uint32_t*)ptr;
            *usblegsup |= (1u << 24);
            for (uint32_t i = 0; i < 100000; i++) {
                if (!(*usblegsup & (1u << 16))) return true;
                usleep_usec(10);
            }
            return false;
        }
        if (ptr[1] == 0) break;
        ptr = (uint8_t*)((uint64_t)hdr + ptr[1]);
    }
    return true;
}

void InitXHCIFromPCI(PCI::PCIHeader0* hdr) {
    PCI::PCI_BAR_TYPE bar = PCI::pci_get_bar(hdr, 0);
    if (bar.type == PCI::PCI_BAR_TYPE_ENUM::MMIO64 || bar.type == PCI::PCI_BAR_TYPE_ENUM::MMIO32) {
        uint64_t physBase = bar.mem_address;
        for (uint64_t off = 0; off < 0x10000; off += 0x1000) {
            VMM::Map((pagemap_t*)kernel_pagemap, physBase + off, physBase + off, VMM_FLAGS_MMIO);
        }
        g_cap = (CapRegs*)(physBase + hhdm_offset);
    } else return;

    PCI::enable_bus_mastering((uint64_t)hdr);
    PCI::enable_mem_space((uint64_t)hdr);
    takeFromBIOS(hdr);

    g_maxSlots = (g_cap->hcsparams1 >> 0) & 0xFF;
    g_maxPorts = (g_cap->hcsparams1 >> 24) & 0xFF;
    g_op = (OpRegs*)((uint64_t)g_cap + g_cap->capLength);
    // Interrupter 0's register set starts 0x20 bytes into the runtime space;
    // rtsoff itself points at the Microframe Index register.
    g_intRegs = (IntRegSet*)((uint64_t)g_cap + g_cap->rtsoff + 0x20);
    g_doorbells = (uint32_t*)((uint64_t)g_cap + g_cap->dboff);

    uint32_t hcs2 = g_cap->hcsparams2;
    uint32_t scratchpadBufs = (hcs2 >> 21) & 0x1F;
    scratchpadBufs *= (1u << ((hcs2 >> 27) & 0x7));

    rb_root_init(&g_pendingCmds, nullptr, nullptr, nullptr, nullptr, nullptr);
    rb_root_init(&g_pendingTransfers, nullptr, nullptr, nullptr, nullptr, nullptr);
    for (uint32_t i = 0; i < MAX_SLOTS; i++) g_slots[i].used = false;

    writeOp(offsetof(OpRegs, usbcmd), 0);
    if (!resetController()) return;
    kinfo("[xHCI] v%x, %u slots, %u ports\n",
          g_cap->hciversion, g_maxSlots, g_maxPorts);

    g_dcbaap = (uint64_t*)allocDMA(sizeof(uint64_t) * (g_maxSlots + 1));
    if (scratchpadBufs > 0) {
        g_scratchpadBufArray = (uint64_t*)allocDMA(sizeof(uint64_t) * scratchpadBufs);
        g_scratchpadBuffers  = allocDMA(0x1000 * scratchpadBufs);
        for (uint32_t i = 0; i < scratchpadBufs; i++) {
            g_scratchpadBufArray[i] = virt_to_phys((void*)((uint64_t)g_scratchpadBuffers + i * 0x1000));
        }
        g_dcbaap[0] = virt_to_phys(g_scratchpadBufArray);
    }
    writeOp(offsetof(OpRegs, dcbaap_lo), (uint32_t)virt_to_phys(g_dcbaap));
    writeOp(offsetof(OpRegs, dcbaap_hi), (uint32_t)(virt_to_phys(g_dcbaap) >> 32));

    g_cmdRing = (volatile TRB*)allocDMA(CMD_RING_SIZE * sizeof(TRB));
    g_cmdRingEnq = g_cmdRing; g_cmdRingCycle = 1;
    uint64_t crcr = virt_to_phys((void*)g_cmdRing) | g_cmdRingCycle;
    writeOp(offsetof(OpRegs, crcr_lo), (uint32_t)crcr);
    writeOp(offsetof(OpRegs, crcr_hi), (uint32_t)(crcr >> 32));

    writeOp(offsetof(OpRegs, config), (readOp(offsetof(OpRegs, config)) & ~0xFFu) | g_maxSlots);

    g_evtRing = (volatile TRB*)allocDMA(EVT_RING_SIZE * sizeof(TRB));
    g_erst = (ERSTEntry*)allocDMA(sizeof(ERSTEntry));
    g_erst[0].segAddr = virt_to_phys((void*)g_evtRing); g_erst[0].segSize = EVT_RING_SIZE;
    g_intRegs->erstsz = 1;
    g_intRegs->erstba_lo = (uint32_t)virt_to_phys(g_erst);
    g_intRegs->erstba_hi = (uint32_t)(virt_to_phys(g_erst) >> 32);
    g_evtRingDeq = g_evtRing; g_evtRingCycle = 1;
    g_intRegs->erdp_lo = (uint32_t)virt_to_phys((void*)g_evtRing);
    g_intRegs->erdp_hi = (uint32_t)(virt_to_phys((void*)g_evtRing) >> 32);
    g_intRegs->iman |= (1u << 1);

    uint32_t cmd = readOp(offsetof(OpRegs, usbcmd));
    cmd |= USBCMD_INTE | USBCMD_HSEE | USBCMD_EWE | USBCMD_RUN;
    writeOp(offsetof(OpRegs, usbcmd), cmd);

    for (uint32_t p = 1; p <= g_maxPorts; p++) {
        volatile PortReg* portReg = &g_op->ports[p-1];
        if (!(portReg->portsc & PORTSC_PP)) {
            portReg->portsc = PORTSC_PP;
            usleep_usec(20000);
        }
    }
    for (uint32_t p = 1; p <= g_maxPorts; p++) {
        volatile PortReg* portReg = &g_op->ports[p-1];
        uint32_t psc = portReg->portsc;
        if (psc & PORTSC_CCS) HandlePortChange(p);
    }
}

// Map the xHCI Port Speed field (PORTSC bits 13:10) to the USB speed enum.
// xHCI encoding: 1=Full, 2=Low, 3=High, 4=Super (5=SuperSpeed Plus).
static USB::USB_SPEED map_port_speed(uint32_t pspd) {
    switch (pspd) {
        case 1:  return USB::USB_SPEED::FULL;
        case 2:  return USB::USB_SPEED::LOW;
        case 4:  return USB::USB_SPEED::SUPER;
        case 3:
        default: return USB::USB_SPEED::HIGH;
    }
}

// Inverse mapping: USB speed enum to the PORTSC speed value stored in the
// Speed field of the Slot Context.
static uint32_t speed_to_pspd(USB::USB_SPEED s) {
    switch (s) {
        case USB::USB_SPEED::FULL: return 1;
        case USB::USB_SPEED::LOW:  return 2;
        case USB::USB_SPEED::HIGH: return 3;
        case USB::USB_SPEED::SUPER: return 4;
    }
    return 3;
}

void HandlePortChange(uint8_t port) {
    volatile PortReg* portReg = &g_op->ports[port-1];
    uint32_t portsc = portReg->portsc;

    // Acknowledge the change bits up front. If enumeration is already running
    // for this port, drop the event instead of re-entering the state machine.
    bool alreadyBusy = g_portBusy[port];
    if (portsc & PORTSC_CSC) portReg->portsc = PORTSC_CSC;
    if (portsc & PORTSC_PRC) portReg->portsc = PORTSC_PRC;
    if (portsc & PORTSC_PEC) portReg->portsc = PORTSC_PEC;
    if (alreadyBusy || !(portsc & PORTSC_CSC)) return;

    g_portBusy[port] = true;
    if (portsc & PORTSC_CCS) {
        portReg->portsc = (portsc & ~PORTSC_PLS) | PORTSC_PR;
        for (uint32_t i = 0; i < 100000; i++) {
            if (portReg->portsc & PORTSC_PRC) break;
            usleep_usec(1000);
        }
        uint32_t afterRst = portReg->portsc;
        if (afterRst & PORTSC_PRC) {
            uint32_t pspd = (afterRst & PORTSC_PSPD) >> 10;
            EnumerateDevice(port, map_port_speed(pspd));
        } else {
            kwarn("[xHCI] port %u reset timeout\n", port);
        }
    } else {
        for (uint32_t i = 1; i <= g_maxSlots; i++) {
            if (g_slots[i].used && g_slots[i].port == port) {
                DestroyDevice(i);
                break;
            }
        }
    }
    g_portBusy[port] = false;
}

bool EnumerateDevice(uint8_t port, USB::USB_SPEED speed) {
    TRB cmd = {}; cmd.control = (TRB_ENABLE_SLOT << 10) | (1u << 5);
    uint8_t slot = SubmitCommandBlocking(&cmd, 1000);
    if (slot == 0) return false;

    DeviceContext* devCtx = (DeviceContext*)allocDMA(sizeof(DeviceContext));
    g_dcbaap[slot] = virt_to_phys(devCtx);
    g_slots[slot].used = true; g_slots[slot].ctx = devCtx; g_slots[slot].port = port; g_slots[slot].speed = speed;

    struct InputCtx { InputControlContext ic; SlotContext slot; EndpointContext ep[31]; };
    InputCtx* inputCtx = (InputCtx*)allocDMA(sizeof(InputCtx));
    inputCtx->ic.add = (1u << 0) | (1u << 1);
    inputCtx->slot.speed = speed_to_pspd(speed);
    inputCtx->slot.ctxEntries = 1;
    inputCtx->slot.rootHubPort = port;
    
    // Control endpoint 0 max packet size by enumerated speed: SuperSpeed is
    // 512, high speed 64, and full/low start at 8 (refined after the device
    // descriptor is read).
    uint16_t mps;
    switch (speed) {
        case USB::USB_SPEED::LOW:
        case USB::USB_SPEED::FULL: mps = 8;   break;
        case USB::USB_SPEED::SUPER: mps = 512; break;
        default:                   mps = 64;  // HIGH
    }
    inputCtx->ep[0].epType = 4; inputCtx->ep[0].cerr = 3;
    inputCtx->ep[0].maxPacketSize = mps; inputCtx->ep[0].averageTRBLen = mps;
    
    volatile TRB* ep0Ring = (volatile TRB*)allocDMA(TRANSFER_RING_SIZE * sizeof(TRB));
    inputCtx->ep[0].dequeueCycleState = virt_to_phys((void*)ep0Ring) | 1u;
    g_slots[slot].rings[0].base = ep0Ring; 
    g_slots[slot].rings[0].enqueue = ep0Ring; 
    g_slots[slot].rings[0].dequeue = ep0Ring;
    g_slots[slot].rings[0].cycle = 1;
    g_slots[slot].rings[0].lock = 0;

    TRB addrCmd = {}; addrCmd.parameter = virt_to_phys(inputCtx); addrCmd.control = (TRB_ADDRESS_DEVICE << 10) | (slot << 24) | (1u << 5);
    if (SubmitCommandBlocking(&addrCmd, 1000) == 0) return false;
    freeDMA(inputCtx);

    USB::DeviceDescriptor devDesc = {};
    if (!USB::ControlTransfer(slot, 0, USB_REQ_DIR_IN | USB_REQ_TYPE_STD | USB_REQ_RCPT_DEV, USB::GET_DESCRIPTOR, (USB::DT_DEVICE << 8), 0, &devDesc, 8)) return false;
    if (!USB::ControlTransfer(slot, 0, USB_REQ_DIR_IN | USB_REQ_TYPE_STD | USB_REQ_RCPT_DEV, USB::GET_DESCRIPTOR, (USB::DT_DEVICE << 8), 0, &devDesc, sizeof(devDesc))) return false;

    uint8_t cfgBuf[1024] = {};
    if (!USB::ControlTransfer(slot, 0, USB_REQ_DIR_IN | USB_REQ_TYPE_STD | USB_REQ_RCPT_DEV, USB::GET_DESCRIPTOR, (USB::DT_CONFIG << 8), 0, cfgBuf, sizeof(USB::ConfigDescriptor))) return false;
    uint16_t totalLen = ((USB::ConfigDescriptor*)cfgBuf)->wTotalLength;
    if (totalLen > sizeof(cfgBuf)) totalLen = sizeof(cfgBuf);
    if (!USB::ControlTransfer(slot, 0, USB_REQ_DIR_IN | USB_REQ_TYPE_STD | USB_REQ_RCPT_DEV, USB::GET_DESCRIPTOR, (USB::DT_CONFIG << 8), 0, cfgBuf, totalLen)) return false;
    if (!USB::ControlTransfer(slot, 0, USB_REQ_DIR_OUT | USB_REQ_TYPE_STD | USB_REQ_RCPT_DEV, USB::SET_CONFIGURATION, 1, 0, nullptr, 0)) return false;

    USB::Device* dev = USB::CreateDevice(slot, port, speed, devDesc, cfgBuf, totalLen);
    kinfo("[xHCI] dev slot%u %04x:%04x on p%u\n",
          slot, devDesc.idVendor, devDesc.idProduct, port);
    USB::RouteDeviceToClassDriver(dev);
    return true;
}

void DestroyDevice(uint8_t slotID) {
    if (!g_slots[slotID].used) return;
    
    // 1. 先禁用槽位，硬件停止产生新事件
    TRB cmd = {}; cmd.control = (TRB_DISABLE_SLOT << 10) | (slotID << 24) | (1u << 5);
    SubmitCommandBlocking(&cmd, 1000);
    
    // 2. 排干事件环
    PollEventRing();
    
    // 3. 清除该 slot 的所有 pending transfer
    uint64_t flags;
    usb_spin_lock_irqsave(&g_pendingTransfersLock, flags);
    rb_node_t* node = rb_first(g_pendingTransfers.node);
    while (node) {
        rb_node_t* next = rb_next(node);
        PendingTransfer* pt = container_of(node, PendingTransfer, node);
        if (pt->slotID == slotID) {
            if (pt->async) {
                rb_erase(&g_pendingTransfers, &pt->node);
                kfree(pt); 
            } else {
                // 同步传输在栈上，仅标记完成唤醒等待线程，由等待线程自行清理
                pt->completionCode = CC_STOPPED;
                pt->completed = true;
            }
        }
        node = next;
    }
    usb_spin_unlock_irqrestore(&g_pendingTransfersLock, flags);
    
    // 4. 释放 rings 和设备上下文
    for (int i = 0; i < 31; i++) {
        if (g_slots[slotID].rings[i].base) {
            freeDMA((void*)g_slots[slotID].rings[i].base);
            g_slots[slotID].rings[i].base = nullptr;
        }
    }
    if (g_slots[slotID].ctx) freeDMA(g_slots[slotID].ctx);
    g_dcbaap[slotID] = 0;
    g_slots[slotID].used = false;
    
    // 5. 最后释放 USB 层设备和类驱动资源
    USB::DestroyDevice(slotID);
}

void ResetEndpoint(uint8_t slotID, uint8_t epAddr) {
    uint8_t epIdx = (epAddr & 0xF) * 2 + ((epAddr & 0x80) ? 1 : 0);
    uint8_t dci = epIdx;

    TRB stopCmd = {}; stopCmd.control = (TRB_STOP_EP << 10) | (slotID << 24) | (dci << 16) | (1u << 5);
    SubmitCommandBlocking(&stopCmd, 1000);

    TRB resetCmd = {}; resetCmd.control = (TRB_RESET_EP << 10) | (slotID << 24) | (dci << 16) | (1u << 5);
    SubmitCommandBlocking(&resetCmd, 1000);

    struct SetDequeueCtx { uint64_t dequeuePtr; uint32_t reserved[2]; };
    SetDequeueCtx ctx;
    ctx.dequeuePtr = virt_to_phys((void*)g_slots[slotID].rings[epIdx].base) | 1u;
    
    TRB setDeqCmd = {}; setDeqCmd.parameter = virt_to_phys(&ctx);
    setDeqCmd.control = (TRB_SET_TR_DEQUEUE << 10) | (slotID << 24) | (dci << 16) | (1u << 5);
    SubmitCommandBlocking(&setDeqCmd, 1000);

    // 清理 stale pending entries
    uint64_t flags;
    usb_spin_lock_irqsave(&g_pendingTransfersLock, flags);
    rb_node_t* node = rb_first(g_pendingTransfers.node);
    while (node) {
        rb_node_t* next = rb_next(node);
        PendingTransfer* pt = container_of(node, PendingTransfer, node);
        if (pt->slotID == slotID) {
            uint64_t ring_phys_start = virt_to_phys((void*)g_slots[slotID].rings[epIdx].base);
            uint64_t ring_phys_end = ring_phys_start + TRANSFER_RING_SIZE * sizeof(TRB);
            if (pt->trbPtr >= ring_phys_start && pt->trbPtr < ring_phys_end) {
                if (pt->async) {
                    rb_erase(&g_pendingTransfers, &pt->node);
                    kfree(pt);
                } else {
                    pt->completionCode = CC_STOPPED;
                    pt->completed = true;
                }
            }
        }
        node = next;
    }
    usb_spin_unlock_irqrestore(&g_pendingTransfersLock, flags);

    usb_spin_lock_irqsave(&g_slots[slotID].rings[epIdx].lock, flags);
    g_slots[slotID].rings[epIdx].enqueue = g_slots[slotID].rings[epIdx].base;
    g_slots[slotID].rings[epIdx].dequeue = g_slots[slotID].rings[epIdx].base;
    g_slots[slotID].rings[epIdx].cycle = 1;
    usb_spin_unlock_irqrestore(&g_slots[slotID].rings[epIdx].lock, flags);
}

bool SubmitControlTransfer(uint8_t slotID, USB::SetupPacket* setup, void* buf, uint16_t len, bool inDir) {
    TRBRingState* ring = &g_slots[slotID].rings[0];

    // The caller's data buffer can live on a stack or any virtual address whose
    // virt_to_phys translation is not reliable for DMA. Stage every data phase
    // through a page-aligned DMA bounce buffer and copy in/out around it.
    void* dmaBuf = nullptr;
    if (len > 0) {
        size_t dmaPages = (len + 0xFFF) >> 12;
        dmaBuf = allocDMA(dmaPages << 12);
        if (!dmaBuf) return false;
        if (!inDir) __memcpy(dmaBuf, buf, len); // OUT: stage host->device data
    }

    uint64_t flags;
    usb_spin_lock_irqsave(&ring->lock, flags);

    volatile TRB* start_enqueue = ring->enqueue;
    uint8_t start_cycle = ring->cycle;
    
    auto try_enqueue = [&](const TRB& trb) -> bool {
        uint32_t idx = ((uint64_t)ring->enqueue - (uint64_t)ring->base) / sizeof(TRB);
        volatile TRB* next = (idx == TRANSFER_RING_SIZE - 1) ? ring->base : ring->enqueue + 1;
        if (next == ring->dequeue) return false;
        
        if (idx == TRANSFER_RING_SIZE - 1) {
            ring->enqueue->parameter = virt_to_phys((void*)ring->base);
            ring->enqueue->status = 0;
            ring->enqueue->control = (TRB_LINK << 10) | (ring->cycle << 0) | (1u << 1);
            ring->cycle ^= 1;
            ring->enqueue = ring->base;
        }
        ring->enqueue->parameter = trb.parameter;
        ring->enqueue->status = trb.status;
        ring->enqueue->control = (trb.control & ~1u) | ring->cycle;
        ring->enqueue++;
        return true;
    };
    
    TRB setupTrb = {};
    setupTrb.parameter = *(uint64_t*)setup; setupTrb.status = 8;
    setupTrb.control = (TRB_SETUP_STAGE << 10) | (1u << 6) | (1u << 5);
    if (len > 0) setupTrb.control |= (inDir ? 3u : 2u) << 16;
    
    if (!try_enqueue(setupTrb)) {
        ring->enqueue = start_enqueue; ring->cycle = start_cycle;
        usb_spin_unlock_irqrestore(&ring->lock, flags);
        if (dmaBuf) freeDMA(dmaBuf);
        return false;
    }
    
    if (len > 0) {
        TRB dataTrb = {};
        dataTrb.parameter = virt_to_phys(dmaBuf ? dmaBuf : buf); dataTrb.status = len;
        dataTrb.control = (TRB_DATA_STAGE << 10) | (1u << 5) | (inDir ? (1u << 16) : 0);
        if (!try_enqueue(dataTrb)) {
            ring->enqueue = start_enqueue; ring->cycle = start_cycle;
            usb_spin_unlock_irqrestore(&ring->lock, flags);
            if (dmaBuf) freeDMA(dmaBuf);
            return false;
        }
    }
    
    uint32_t status_idx = ((uint64_t)ring->enqueue - (uint64_t)ring->base) / sizeof(TRB);
    uint64_t status_trb_ptr = (status_idx == TRANSFER_RING_SIZE - 1) ? virt_to_phys((void*)ring->base) : virt_to_phys((void*)ring->enqueue);
    
    PendingTransfer pt = {}; rb_init_node(&pt.node); pt.completed = false; pt.async = false;
    pt.slotID = slotID; pt.trbPtr = status_trb_ptr;
    
    uint64_t pflags;
    usb_spin_lock_irqsave(&g_pendingTransfersLock, pflags);
    rb_insert(&g_pendingTransfers, &pt.node, pending_xfer_cmp);
    usb_spin_unlock_irqrestore(&g_pendingTransfersLock, pflags);
    
    TRB statusTrb = {};
    statusTrb.control = (TRB_STATUS_STAGE << 10) | (1u << 5) | (inDir ? 0 : (1u << 16));
    if (!try_enqueue(statusTrb)) {
        usb_spin_lock_irqsave(&g_pendingTransfersLock, pflags);
        rb_erase(&g_pendingTransfers, &pt.node);
        usb_spin_unlock_irqrestore(&g_pendingTransfersLock, pflags);
        
        ring->enqueue = start_enqueue; ring->cycle = start_cycle;
        usb_spin_unlock_irqrestore(&ring->lock, flags);
        if (dmaBuf) freeDMA(dmaBuf);
        return false;
    }
    
    ringDoorbell(slotID, 1);
    usb_spin_unlock_irqrestore(&ring->lock, flags);

    for(uint32_t i=0; i<100000; i++) {
        if (pt.completed) break;
        PollEventRing();
        usleep_usec(10);
    }

    usb_spin_lock_irqsave(&g_pendingTransfersLock, pflags);
    rb_erase(&g_pendingTransfers, &pt.node);
    usb_spin_unlock_irqrestore(&g_pendingTransfersLock, pflags);

    bool ok = pt.completed &&
              (pt.completionCode == CC_SUCCESS || pt.completionCode == CC_SHORT_PKT);
    if (dmaBuf) {
        if (inDir && ok) __memcpy(buf, dmaBuf, len); // IN: hand device data out
        freeDMA(dmaBuf);
    }
    return ok;
}

bool SubmitNormalTransfer(uint8_t slotID, uint8_t epAddr, void* buf, uint32_t len, bool inDir, bool isoch) {
    uint8_t epIdx = (epAddr & 0xF) * 2 + (inDir ? 1 : 0);
    TRBRingState* ring = &g_slots[slotID].rings[epIdx];

    // Stage the transfer through a page-aligned DMA bounce buffer; the caller's
    // buffer may live on a stack or another virtual address whose virt_to_phys
    // translation is not reliable for DMA (same hazard as control transfers).
    void* dmaBuf = nullptr;
    if (len > 0) {
        size_t dmaPages = (len + 0xFFF) >> 12;
        dmaBuf = allocDMA(dmaPages << 12);
        if (!dmaBuf) return false;
        if (!inDir) __memcpy(dmaBuf, buf, len); // OUT: stage host->device data
    }

    TRB trb = {};
    trb.parameter = virt_to_phys(dmaBuf ? dmaBuf : buf); trb.status = len;
    trb.control = ((isoch ? TRB_ISOCH : TRB_NORMAL) << 10) | (1u << 5);

    PendingTransfer pt = {}; rb_init_node(&pt.node); pt.completed = false; pt.async = false;
    pt.slotID = slotID;
    if (!EnqueueAndTrack(ring, slotID, epIdx, trb, &pt)) {
        if (dmaBuf) freeDMA(dmaBuf);
        return false;
    }

    for(uint32_t i=0; i<100000; i++) {
        if (pt.completed) break;
        PollEventRing();
        usleep_usec(10);
    }
    uint64_t flags;
    usb_spin_lock_irqsave(&g_pendingTransfersLock, flags);
    rb_erase(&g_pendingTransfers, &pt.node);
    usb_spin_unlock_irqrestore(&g_pendingTransfersLock, flags);

    bool ok = pt.completed && (pt.completionCode == CC_SUCCESS || pt.completionCode == CC_SHORT_PKT);
    if (dmaBuf) {
        if (inDir && ok) __memcpy(buf, dmaBuf, len); // IN: hand device data out
        freeDMA(dmaBuf);
    }
    return ok;
}

bool ConfigureEndpoint(uint8_t slotID, uint8_t epAddr, USB::EP_TYPE type, uint16_t mps, uint8_t interval) {
    struct InputCtx { InputControlContext ic; SlotContext slot; EndpointContext ep[31]; };
    InputCtx* inputCtx = (InputCtx*)allocDMA(sizeof(InputCtx));
    
    DeviceContext* devCtx = (DeviceContext*)g_dcbaap[slotID]; 
    __memcpy(&inputCtx->slot, &devCtx->slot, sizeof(SlotContext));
    
    uint8_t epIdx = (epAddr & 0xF) * 2 + ((epAddr & 0x80) ? 1 : 0);
    // DCI for a non-control endpoint is 2*EPnum + direction, equal to epIdx;
    // only control EP0 uses DCI 1. The earlier epIdx+1 shifted every
    // bulk/interrupt context up by one, so the real device endpoint stalled.
    uint8_t dci = epIdx;

    inputCtx->ic.add |= (1u << 0); 
    inputCtx->ic.add |= (1u << dci);
    if (dci > inputCtx->slot.ctxEntries) inputCtx->slot.ctxEntries = dci;
    
    // Input context: ep[0] holds DCI 1 (EP0), so the context for DCI d lives at
    // ep[d-1], matching the controller's 32+32*d layout.
    EndpointContext* epCtx = &inputCtx->ep[dci - 1];
    epCtx->epType = (uint32_t)type & 0x7;
    epCtx->maxPacketSize = mps; epCtx->interval = interval; epCtx->averageTRBLen = mps;
    
    if (g_slots[slotID].rings[epIdx].base) {
        freeDMA((void*)g_slots[slotID].rings[epIdx].base);
    }
    
    volatile TRB* ring = (volatile TRB*)allocDMA(TRANSFER_RING_SIZE * sizeof(TRB));
    epCtx->dequeueCycleState = virt_to_phys((void*)ring) | 1u;
    g_slots[slotID].rings[epIdx].base = ring; 
    g_slots[slotID].rings[epIdx].enqueue = ring; 
    g_slots[slotID].rings[epIdx].dequeue = ring;
    g_slots[slotID].rings[epIdx].cycle = 1;
    g_slots[slotID].rings[epIdx].lock = 0;

    TRB cmd = {}; cmd.parameter = virt_to_phys(inputCtx); cmd.control = (TRB_CONFIGURE_EP << 10) | (slotID << 24) | (1u << 5);
    uint8_t ret = SubmitCommandBlocking(&cmd, 1000);
    freeDMA(inputCtx);
    return ret != 0;
}

void StartAsyncInterrupt(uint8_t slotID, uint8_t epAddr, void* buf, uint32_t len, void(*cb)(uint8_t*, uint32_t, void*), void* ctx) {
    uint8_t epIdx = (epAddr & 0xF) * 2 + ((epAddr & 0x80) ? 1 : 0);
    TRBRingState* ring = &g_slots[slotID].rings[epIdx];
    
    TRB trb = {}; trb.parameter = virt_to_phys(buf); trb.status = len; trb.control = (TRB_NORMAL << 10) | (1u << 5);

    PendingTransfer* pt = (PendingTransfer*)kmalloc(sizeof(PendingTransfer));
    rb_init_node(&pt->node); pt->completed = false; pt->async = true; pt->callback = (void*)cb; pt->ctx = ctx; pt->buf = buf; pt->len = len;
    pt->slotID = slotID;
    
    if (!EnqueueAndTrack(ring, slotID, epIdx, trb, pt)) {
        uint64_t flags;
        usb_spin_lock_irqsave(&g_pendingTransfersLock, flags);
        rb_erase(&g_pendingTransfers, &pt->node);
        usb_spin_unlock_irqrestore(&g_pendingTransfersLock, flags);
        kfree(pt);
    }
}

void StartAsyncIsoch(uint8_t slotID, uint8_t epAddr, void* buf, uint32_t len, bool inDir, void(*cb)(uint8_t*, uint32_t, void*), void* ctx) {
    uint8_t epIdx = (epAddr & 0xF) * 2 + (inDir ? 1 : 0);
    TRBRingState* ring = &g_slots[slotID].rings[epIdx];
    
    TRB trb = {}; trb.parameter = virt_to_phys(buf); trb.status = len; trb.control = (TRB_ISOCH << 10) | (1u << 5);
    
    PendingTransfer* pt = (PendingTransfer*)kmalloc(sizeof(PendingTransfer));
    rb_init_node(&pt->node); pt->completed = false; pt->async = true; pt->callback = (void*)cb; pt->ctx = ctx; pt->buf = buf; pt->len = len;
    pt->slotID = slotID;
    
    if (!EnqueueAndTrack(ring, slotID, epIdx, trb, pt)) {
        uint64_t flags;
        usb_spin_lock_irqsave(&g_pendingTransfersLock, flags);
        rb_erase(&g_pendingTransfers, &pt->node);
        usb_spin_unlock_irqrestore(&g_pendingTransfersLock, flags);
        kfree(pt);
    }
}

} // namespace XHCI