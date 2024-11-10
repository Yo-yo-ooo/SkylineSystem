#pragma once
#include <stdint.h>

#include "../../../klib/klib.h"
#include "../interrupt/idt.h"

namespace PIT
{
#define PIT_FREQ 1000

    extern u64 pit_ticks;

    void Handler(registers* r);

    void Init();

    void Sleep(u64 ms);
}
