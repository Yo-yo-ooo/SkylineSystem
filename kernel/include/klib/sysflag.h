#pragma once
#ifndef SYSFLAG_H
#define SYSFLAG_H

#include <stdint.h>
#include <stddef.h>
#include <pdef.h>

#ifdef __x86_64__

PACK(struct x86_64_sysflag{
    uint8_t SMPStarted : 1;
    uint8_t SSEEnabled : 1;
    uint8_t AVXEnabled : 1;
    uint8_t AVX512Enabled : 1;
    uint8_t RSVD : 4;
});

extern struct x86_64_sysflag sysflag_g;

#endif

#endif