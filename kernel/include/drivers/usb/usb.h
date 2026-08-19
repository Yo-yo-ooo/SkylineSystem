//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <pdef.h>

namespace USB {

enum class USB_SPEED : uint8_t {
    FULL  = 0, LOW   = 1, HIGH  = 2, SUPER = 3,
};

enum class EP_TYPE : uint8_t {
    CONTROL_OUT = 0, ISOCH_OUT   = 1, BULK_OUT    = 2, INT_OUT     = 3,
    CONTROL_IN  = 4, ISOCH_IN    = 5, BULK_IN     = 6, INT_IN      = 7,
};

enum class EP_DIR : uint8_t { OUT=0, IN=1 };

PACK(struct SetupPacket {
    uint8_t  bmRequestType;
    uint8_t  bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
});

enum : uint8_t {
    GET_STATUS=0, CLEAR_FEATURE=1, SET_FEATURE=3, SET_ADDRESS=5,
    GET_DESCRIPTOR=6, SET_DESCRIPTOR=7, GET_CONFIGURATION=8,
    SET_CONFIGURATION=9, GET_INTERFACE=10, SET_INTERFACE=11, SYNCH_FRAME=12,
};

enum : uint8_t {
    DT_DEVICE=1, DT_CONFIG=2, DT_STRING=3, DT_INTERFACE=4, DT_ENDPOINT=5,
};

#define USB_REQ_DIR_IN   (1u << 7)
#define USB_REQ_DIR_OUT  (0u << 7)
#define USB_REQ_TYPE_STD (0u << 5)
#define USB_REQ_TYPE_CLS (1u << 5)
#define USB_REQ_TYPE_VND (2u << 5)
#define USB_REQ_RCPT_DEV 0
#define USB_REQ_RCPT_IF  1
#define USB_REQ_RCPT_EP  2

enum : uint8_t {
    HID_GET_REPORT=0x01, HID_GET_IDLE=0x02, HID_GET_PROTOCOL=0x03,
    HID_SET_REPORT=0x09, HID_SET_IDLE=0x0A, HID_SET_PROTOCOL=0x0B,
};

enum : uint8_t {
    MSC_BBB_RESET=0xFF, MSC_BBB_GET_MAX_LUN=0xFE,
};

enum : uint8_t {
    CC_AUDIO=0x01, CC_CDC=0x02, CC_HID=0x03, CC_Physical=0x05,
    CC_Image=0x06, CC_Printer=0x07, CC_MSC=0x08, CC_Hub=0x09,
    CC_CDCData=0x0A, CC_SmartCard=0x0B, CC_Video=0x0E,
};

} // namespace USB