#pragma once

#include <klib/klib.h>
#include <fs/vfs.h>

#if !defined(__x86_64__)
#error "Panic (/kernel/src/drivers/keyboard/x86): This keyboard driver is only for x86_64!"
#endif

#define EV_KEY 0x1

//extern vfs_node *kb_node;

typedef struct
{
    u16 type;  // key mod etc
    u16 value; // pressed released repeated
    u8 code;
} keyboard_event;

void keyboard_init();