//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <pdef.h>
#include <arch/x86_64/dev/pci/pci.h>
#include <drivers/usb/usb.h>
#include <klib/klib.h>

#define usb_spin_lock_irqsave(lock, flags) do { flags = irq_save(); spinlock_lock(lock); } while(0)
#define usb_spin_unlock_irqrestore(lock, flags) do { spinlock_unlock(lock); irq_restore(flags); } while(0)


namespace XHCI {

enum TRB_TYPE : uint8_t {
    TRB_NORMAL=1, TRB_SETUP_STAGE=2, TRB_DATA_STAGE=3, TRB_STATUS_STAGE=4,
    TRB_ISOCH=5, TRB_LINK=6, TRB_TRANSFER_EVENT=32, TRB_COMMAND_COMPLETION=33,
    TRB_PORT_STATUS_CHANGE=34, TRB_MFINDEX_WRAP=39, TRB_ENABLE_SLOT=9,
    TRB_DISABLE_SLOT=10, TRB_ADDRESS_DEVICE=11, TRB_CONFIGURE_EP=12,
    TRB_EVALUATE_CTX=13, TRB_RESET_EP=14, TRB_STOP_EP=15, TRB_SET_TR_DEQUEUE=16,
    TRB_RESET_DEVICE=17, TRB_NOP=23,
};

PACK(union TRB {
    struct { uint64_t parameter; uint32_t status; uint32_t control; };
    uint8_t raw[16];
});

PACK(struct InputControlContext { uint32_t drop; uint32_t add; uint8_t pad[24]; });
PACK(struct SlotContext {
    // Dword 0 (dev_info)
    uint32_t routeString:20;  // bits 19:0
    uint32_t speed:4;         // bits 23:20, PORTSC speed encoding (1..4)
    uint32_t rsvd24:1;        // bit 24
    uint32_t mtt:1;           // bit 25: LS/FS via HS hub
    uint32_t hub:1;           // bit 26
    uint32_t ctxEntries:5;    // bits 31:27, last valid DCI
    // Dword 1 (dev_info2)
    uint32_t maxExitLatency:16; // bits 15:0
    uint32_t rootHubPort:8;     // bits 23:16
    uint32_t rsvd_dw1:8;        // bits 31:24
    // Dword 2 (tt_info)
    uint32_t numPorts:8;        // bits 7:0
    uint32_t ttHubSlotID:8;     // bits 15:8
    uint32_t ttPortNum:8;       // bits 23:16
    uint32_t ttThinkTime:2;     // bits 25:24
    uint32_t intrTarget:6;      // bits 31:26
    // Dword 3 (dev_state)
    uint32_t deviceAddress:8;   // bits 7:0
    uint32_t rsvd_dw3:19;       // bits 26:8
    uint32_t slotState:5;       // bits 31:27
    // Dword 4-7 reserved: every context occupies 32 bytes, so EP0 starts at
    // offset 32 of the device/input context.
    uint32_t pad[4];
});
PACK(struct EndpointContext {
    // Dword 0 (ep_info)
    uint32_t epState:3;       // bits 2:0
    uint32_t rsvd0:5;         // bits 7:3
    uint32_t mult:2;          // bits 9:8
    uint32_t maxPStreams:5;   // bits 14:10
    uint32_t lsa:1;           // bit 15
    uint32_t interval:8;      // bits 23:16 (bInterval)
    uint32_t rsvd0b:8;        // bits 31:24
    // Dword 1 (ep_info2)
    uint32_t rsvd1:1;         // bit 0
    uint32_t cerr:2;          // bits 2:1
    uint32_t epType:3;        // bits 5:3
    uint32_t rsvd6:2;         // bits 7:6
    uint32_t maxBurstSize:8;  // bits 15:8
    uint32_t maxPacketSize:16; // bits 31:16
    // Dword 2-3: TR Dequeue Pointer; low bit 0 = Dequeue Cycle State
    uint64_t dequeueCycleState;
    // Dword 4
    uint16_t averageTRBLen;    // bits 15:0
    uint16_t esitPayloadLow;   // bits 31:16
    // Dword 5-7 reserved
    uint32_t pad[3];
});
PACK(struct DeviceContext { SlotContext slot; EndpointContext ep[31]; });
PACK(struct ERSTEntry { uint64_t segAddr; uint32_t segSize; uint32_t rsvd; });

PACK(struct CapRegs { uint8_t capLength; uint8_t reserved; uint16_t hciversion; uint32_t hcsparams1; uint32_t hcsparams2; uint32_t hcsparams3; uint32_t hccparams1; uint32_t dboff; uint32_t rtsoff; uint32_t hccparams2; });
PACK(struct PortReg { uint32_t portsc; uint32_t portpmsc; uint32_t portli; uint32_t porthlpmc; });
PACK(struct OpRegs {
    uint32_t usbcmd; uint32_t usbsts; uint32_t pagesize; uint8_t pad0[8]; uint32_t dnctrl;
    uint32_t crcr_lo; uint32_t crcr_hi;
    uint8_t pad_cr[0x30 - 0x20];
    uint32_t dcbaap_lo; uint32_t dcbaap_hi; uint32_t config;
    uint8_t pad_cfg[0x400 - 0x3C];
    PortReg ports[255];
});
PACK(struct IntRegSet { uint32_t iman; uint32_t imod; uint32_t erstsz; uint32_t rsvd_0c; uint32_t erstba_lo; uint32_t erstba_hi; uint32_t erdp_lo; uint32_t erdp_hi; });

constexpr uint32_t CMD_RING_SIZE = 256;
constexpr uint32_t EVT_RING_SIZE = 256;
constexpr uint32_t TRANSFER_RING_SIZE = 256;
constexpr uint32_t MAX_SLOTS = 256;

constexpr uint32_t PORTSC_CCS=1<<0, PORTSC_PED=1<<1, PORTSC_OCA=1<<3, PORTSC_PR=1<<4;
constexpr uint32_t PORTSC_PLS=0xF<<5, PORTSC_PP=1<<9, PORTSC_PSPD=0xF<<10, PORTSC_LWS=1<<16;
constexpr uint32_t PORTSC_CSC=1<<17, PORTSC_PEC=1<<18, PORTSC_WRC=1<<19, PORTSC_PRC=1<<21;
constexpr uint32_t PORTSC_PLC=1<<22, PORTSC_CEC=1<<23, PORTSC_WCE=1<<25, PORTSC_WDE=1<<26, PORTSC_WOE=1<<27;

constexpr uint32_t USBCMD_RUN=1<<0, USBCMD_HCRST=1<<1, USBCMD_INTE=1<<2, USBCMD_HSEE=1<<3, USBCMD_EWE=1<<10;
constexpr uint32_t USBSTS_HCH=1<<0, USBSTS_HSE=1<<2, USBSTS_EINT=1<<3, USBSTS_PCD=1<<4, USBSTS_CNR=1<<11;

enum CC : uint8_t {
    CC_INVALID=0, CC_SUCCESS=1, CC_DATA_BUFFER=2, CC_BABBLE=3, CC_USB_TX_ERR=4,
    CC_TRB_ERR=5, CC_STALL=6, CC_RESOURCE=7, CC_BANDWIDTH=8, CC_NO_SLOTS=9,
    CC_INVALID_STREAM=10, CC_SLOT_NOT_EN=11, CC_EP_NOT_EN=12, CC_SHORT_PKT=13,
    CC_RING_UNDERRUN=14, CC_RING_OVERRUN=15, CC_VF_EVENT=16, CC_PARAM=17,
    CC_BW_OVERRUN=18, CC_CTX_STATE=19, CC_NO_PING_RESP=20, CC_EV_RING_FULL=21,
    CC_INCOMPAT_DEV=22, CC_MISSED_SRV=23, CC_CMD_RING_STOP=24, CC_XACT_ERR=25,
    CC_STOPPED=26, // 新增：表示被主动停止的传输
};

struct TRBRingState {
    volatile TRB* base;
    volatile TRB* enqueue;
    volatile TRB* dequeue; 
    uint8_t cycle;
    spinlock_t lock;
};

struct SlotInfo {
    bool used;
    DeviceContext* ctx;
    TRBRingState rings[31];
    uint8_t port;
    USB::USB_SPEED speed;
};

void InitXHCIFromPCI(PCI::PCIHeader0* hdr);
void PollEventRing();
void HandlePortChange(uint8_t port);
bool EnumerateDevice(uint8_t port, USB::USB_SPEED speed);
void DestroyDevice(uint8_t slotID);
void ResetEndpoint(uint8_t slotID, uint8_t epAddr);

bool SubmitControlTransfer(uint8_t slotID, USB::SetupPacket* setup, void* buf, uint16_t len, bool inDir);
bool SubmitNormalTransfer(uint8_t slotID, uint8_t epAddr, void* buf, uint32_t len, bool inDir, bool isoch);
bool ConfigureEndpoint(uint8_t slotID, uint8_t epAddr, USB::EP_TYPE type, uint16_t mps, uint8_t interval);
void StartAsyncInterrupt(uint8_t slotID, uint8_t epAddr, void* buf, uint32_t len, void(*cb)(uint8_t*, uint32_t, void*), void* ctx);
void StartAsyncIsoch(uint8_t slotID, uint8_t epAddr, void* buf, uint32_t len, bool inDir, void(*cb)(uint8_t*, uint32_t, void*), void* ctx);

} // namespace XHCI