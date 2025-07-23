#include <arch/x86_64/allin.h>
#define CURRENT_YEAR_STR        (__DATE__ + 7)                            // Change this each year!

// https://wiki.osdev.org/CMOS#RTC_Update_In_Progress



namespace RTC
{

    uint8_t second;
    uint8_t minute;
    uint8_t hour;
    uint8_t day;
    uint8_t month;
    uint32_t year;

    int32_t CURRENT_YEAR = 0;
    void InitRTC()
    {
        CURRENT_YEAR = to_int(CURRENT_YEAR_STR);
        LastUpdateTime = 0;
        minute = 0;
        second = 0;
        hour = 0;
        day = 1;
        month = 1;
        year = CURRENT_YEAR;

        Minute = 0;
        Second = 0;
        Hour = 0;
        Day = 1;
        Month = 1;
        Year = CURRENT_YEAR;
    }

    int32_t century_register = 0x00;                                // Set by ACPI table parsing code if possible
    

    

    
    enum {
        cmos_address = 0x70,
        cmos_data    = 0x71
    };
    
    int32_t get_update_in_progress_flag() {
        outb(cmos_address, 0x0A);
        return (inb(cmos_data) & 0x80);
    }
    
    uint8_t get_RTC_register(int32_t reg) {
        outb(cmos_address, reg);
        return inb(cmos_data);
    }
    
    void read_rtc() 
    {
        uint8_t century;
        uint8_t last_second;
        uint8_t last_minute;
        uint8_t last_hour;
        uint8_t last_day;
        uint8_t last_month;
        uint8_t last_year;
        uint8_t last_century;
        uint8_t registerB;

        // Note: This uses the "read registers until you get the same values twice in a row" technique
        //       to avoid getting dodgy/inconsistent values due to RTC updates

        while (get_update_in_progress_flag());                // Make sure an update isn't in progress
        second = get_RTC_register(0x00);
        minute = get_RTC_register(0x02);
        hour = get_RTC_register(0x04);
        day = get_RTC_register(0x07);
        month = get_RTC_register(0x08);
        year = get_RTC_register(0x09);
        if(century_register != 0) 
            century = get_RTC_register(century_register);
        
    
        do 
        {
            last_second = second;
            last_minute = minute;
            last_hour = hour;
            last_day = day;
            last_month = month;
            last_year = year;
            last_century = century;

            while (get_update_in_progress_flag());           // Make sure an update isn't in progress
            second = get_RTC_register(0x00);
            minute = get_RTC_register(0x02);
            hour = get_RTC_register(0x04);
            day = get_RTC_register(0x07);
            month = get_RTC_register(0x08);
            year = get_RTC_register(0x09);
            if(century_register != 0) 
                century = get_RTC_register(century_register);
        }
        while( (last_second != second) || (last_minute != minute) || (last_hour != hour) ||
                (last_day != day) || (last_month != month) || (last_year != year) ||
                (last_century != century) );
    
        registerB = get_RTC_register(0x0B);

        // Convert BCD to binary values if necessary

        if (!(registerB & 0x04)) {
            second = (second & 0x0F) + ((second / 16) * 10);
            minute = (minute & 0x0F) + ((minute / 16) * 10);
            hour = ( (hour & 0x0F) + (((hour & 0x70) / 16) * 10) ) | (hour & 0x80);
            day = (day & 0x0F) + ((day / 16) * 10);
            month = (month & 0x0F) + ((month / 16) * 10);
            year = (year & 0x0F) + ((year / 16) * 10);
            if(century_register != 0) 
                century = (century & 0x0F) + ((century / 16) * 10);
        }
    
        // Convert 12 hour clock to 24 hour clock if necessary
    
        if (!(registerB & 0x02) && (hour & 0x80)) {
                hour = ((hour & 0x7F) + 12) % 24;
        }
    
        // Calculate the full (4-digit) year
    
        if(century_register != 0) {
            year += century * 100;
        } else {
            year += (CURRENT_YEAR / 100) * 100;
            if(year < CURRENT_YEAR) 
            year += 100;
        }
    }



    int32_t Second, Minute, Hour, Day, Month, Year;
    unsigned long LastUpdateTime = 0;

    void UpdateTimeIfNeeded()
    {
        if (PIT::Inited)
        {
            unsigned long now = PIT::TimeSinceBootMS();
            if (now > LastUpdateTime + 500)
            {
                LastUpdateTime = now;
                read_rtc();
            }
            else
                return;
        }
        else
            read_rtc();

        //int32_t offset = 2;

        Second = second;
        Minute = minute;
        Hour = hour;
        Day = day;
        Month = month;
        Year = year;
    }

}