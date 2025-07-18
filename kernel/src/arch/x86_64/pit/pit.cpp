#include <arch/x86_64/allin.h>
namespace PIT
{
    int32_t roughCount = (BaseFrequency/200) / 2;
    uint64_t TicksSinceBoot = 0;
    uint64_t MicroSecondOffset = 0;

    uint16_t Divisor = 65535;

    uint16_t NonMusicDiv = 5000;//65535;//2000; // 596.591 Hz

    bool Inited = false;

    uint64_t freq = GetFrequency();
    int32_t FreqAdder = 1;

    void InitPIT()
    {
        TicksSinceBoot = 0;
        SetDivisor(NonMusicDiv /*65535*/);
        freq = GetFrequency();
        irq_register(0, PIT::Handler);
        //Inited = true;
    }

    void Handler(registers *r){
        Tick();
        if (Schedule::sched_sleep_list != NULL)
            asm volatile("nop");
        
        LAPIC::EOI();
    }

    void SetDivisor(uint16_t divisor)
    {
        if (divisor < 5)
            divisor = 5;

        if (Inited)
        {
            MicroSecondOffset += (TicksSinceBoot * 1000000) / freq;
            TicksSinceBoot = 0;
        }

        Divisor = divisor;
        roughCount = (BaseFrequency/divisor) / 2;
        outb(0x43, 0x36);
        outb(0x40, (uint8_t)(divisor & 0x00ff));
        io_wait();
        outb(0x40, (uint8_t)((divisor & 0xff00) >> 8));
        io_wait();
        freq = GetFrequency();
        FreqAdder = 1000000/(BaseFrequency / divisor);
    }

    void Sleep(uint64_t milliseconds)
    {
        if (!Inited)
            return;
        uint64_t endTime = TimeSinceBootMS() + milliseconds;
        while (TimeSinceBootMS() < endTime)
            asm("hlt");
    }

    void Sleepd(uint64_t seconds)
    {
        Sleepd((uint64_t)(seconds * 1000));
    }

    uint64_t GetFrequency()
    {
        return BaseFrequency / Divisor;
    }

    void SetFrequency(uint64_t frequency)
    {
        SetDivisor(BaseFrequency / frequency);
        freq = GetFrequency();
    }

    
    int32_t tempus = 0;
    void Tick()
    {
        TicksSinceBoot++;
        
        if (tempus++ > roughCount)
        {
            tempus = 0;
            RTC::UpdateTimeIfNeeded();
        }
    }

    
    
    uint64_t TimeSinceBootMS()
    {
        if (!Inited)
            return 0;
        return (TicksSinceBoot*1000)/freq + MicroSecondOffset / 1000;
    }

    uint64_t TimeSinceBootMicroS()
    {
        if (!Inited)
            return 0;
        return (TicksSinceBoot*1000000)/freq + MicroSecondOffset;
    }

}