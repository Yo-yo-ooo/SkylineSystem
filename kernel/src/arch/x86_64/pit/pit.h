#pragma once
#include <stdint.h>
#include "../../../klib/kio.h"
namespace PIT
{
    extern uint64_t TicksSinceBoot;
    extern uint64_t freq;
    extern uint16_t Divisor;
    static const uint64_t BaseFrequency = 1193182;
    extern bool Inited;
    extern int FreqAdder;
    extern uint16_t NonMusicDiv;


    void Sleepd(uint64_t seconds);
    void Sleep(uint64_t milliseconds);
    void InitPIT();

    void SetDivisor(uint16_t divisor);
    uint64_t TimeSinceBootMS();
    uint64_t TimeSinceBootMicroS();
    uint64_t GetFrequency();
    void SetFrequency(uint64_t frequency);
    void Tick();
}
