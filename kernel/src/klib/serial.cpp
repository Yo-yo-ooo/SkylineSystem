#include <klib/klib.h>
#include <klib/kio.h>
#include <klib/serial.h>

#ifdef __x86_64__
#include <arch/x86_64/dev/pci/pci.h>
#include <arch/x86_64/pit/pit.h>
#endif

namespace Serial
{
    int32_t SerialPort = 0x3F8;          // COM1
    uint64_t pciCard = 0;
    PCI::PCI_BAR_TYPE pciType;
    int32_t serialPciOffset = 0;
    bool SerialWorks = false;
    //SerialManager::GenericPacket* currentSerialReadPacket = NULL;
    int32_t currentSerialReadPacketIndex = 0;

    bool Init()
    {
        AddToStack();

        if (pciCard != 0)
        {
            //osData.debugTerminalWindow->Log("Serial PCI CARD AT: 0x{}", ConvertHexToString(pciCard), Colors.yellow);
            uint64_t bar0 = ((PCI::PCIHeader0*)pciCard)->BAR0;
            //osData.debugTerminalWindow->Log("Serial PCI CARD BAR0: {}", ConvertHexToString(bar0), Colors.yellow);
            //pciIoBase = bar0;// & (~0x3);
            //pciIoBase = bar2;// & (~0x3);


            pciType = PCI::pci_get_bar((PCI::PCIHeader0*)pciCard, 0);
            serialPciOffset = 0xC0;

            // try with membase?
            //pciIoBase = bar0 + 0x3A7;

            //pciIoBase += 0x08;
            //osData.debugTerminalWindow->Log("Serial PCI CARD TYPE: {}", (pciType.type < 3 ? "MMIO" : "IO"), Colors.bgreen);
            //osData.debugTerminalWindow->Log("Serial PCI CARD IO BASE: {}", to_string(pciType.io_address), Colors.bgreen);
            //osData.debugTerminalWindow->Log("Serial PCI CARD MEM BASE: {}", ConvertHexToString(pciType.mem_address), Colors.bgreen);
            
            PCI::enable_io_space(pciCard);
            PCI::enable_mem_space(pciCard);
            PCI::enable_interrupt(pciCard);
            PCI::enable_bus_mastering(pciCard);


            // Soutb(1, 0x80);
            // io_wait(100);
        }
        else
        {

        }
        
        // if (pciCard == 0)
        // {
        //     // Disable all interrupts
        //     Soutb(1, 0x00);
        //     // Enable DLAB (set baud rate divisor)
        //     Soutb(3, 0x80);
        //     // Set divisor to 3 (lo byte) 38400 baud
        //     Soutb(0, 0x03);
        //     //                  (hi byte)
        //     Soutb(1, 0x00);
        //     // 8 bits, no parity, one stop bit
        //     Soutb(3, 0x03);
        //     // Enable FIFO, clear them, with 14-byte threshold
        //     Soutb(2, 0xC7);
        //     // IRQs enabled, RTS/DSR set
        //     Soutb(4, 0x0B);
            
        // }
        // else
        // {
        //     // Soutb(3, 0x03);   // Set word size
        //     // Soutb(1, 0x80);    // Reset
        //     // Soutb(3, 0x80);    // Enable DLAB (set baud rate divisor)
        //     // Soutb(0, 0x03);    // Set divisor to 3 (lo byte) 38400 baud
        //     // Soutb(1, 0x00);    //                  (hi byte)
        //     // Soutb(4, 0x80);    // half duplex ig
        //     // Soutb(3, 0x03);    // 8 bits, no parity, one stop bit
        //     // Soutb(1, 0x00);    // Disable all interrupts
        //     // //Soutb(2, 0xC7);    // Enable FIFO, clear them, with 14-byte threshold
        //     // Soutb(2, 0x00);    // No FIFO
        //     // //Soutb(4, 0x0B);    // IRQs enabled, RTS/DSR set
        //     // Soutb(4, 0x08);    // OUT yes

        //     // Disable all interrupts
        //     Soutb(1, 0x00);
        //     // Enable DLAB (set baud rate divisor)
        //     Soutb(3, 0x80);
        //     // Set divisor to 3 (lo byte) 38400 baud
        //     Soutb(0, 0x03);
        //     //                  (hi byte)
        //     Soutb(1, 0x00);
        //     // 8 bits, no parity, one stop bit
        //     Soutb(3, 0x03);
        //     // Enable FIFO, clear them, with 14-byte threshold
        //     Soutb(2, 0xC7);
        //     // IRQs enabled, RTS/DSR set
        //     Soutb(4, 0x0B);

        // }

        {
            // Disable all interrupts
            Soutb(1, 0x00);
            io_wait(100);
            // Enable DLAB (set baud rate divisor)
            Soutb(3, 0x80);
            io_wait(100);
            // Set divisor to 3 (lo byte) 38400 baud
            Soutb(0, 0x01); // NVM WE DO 1 -> 115200 baud
            //                  (hi byte)
            Soutb(1, 0x00);
            io_wait(100);
            // 8 bits, no parity, one stop bit
            Soutb(3, 0x03);
            // Enable FIFO, clear them, with 14-byte threshold
            // actually looking at the data sheet its 224 bytes apparently
            Soutb(2, 0xC7);
            // IRQs enabled, RTS/DSR set
            Soutb(4, 0x0B);
            io_wait(100);
        }
        
        for (int32_t i = 0; i < 5000 && Sinb(5) & 1 == 1; i++)
            Sinb(0);
        
        Soutb(4, 0x1E);    // Set in loopback mode, test the serial chip
        
        for (int32_t i = 0; i < 5000 && Sinb(5) & 1 == 1; i++)
            Sinb(0);
        
        Soutb(0, 0xAE);    // Test serial chip (send byte 0xAE and check if serial returns same byte)
        io_wait(500);
        // Check if serial is faulty (i.e: not same byte as sent)
        if(Sinb(0) != 0xAE)
        {
            // if (osData.debugTerminalWindow != NULL)
            // {
            //     if (pciCard != 0)
            //         osData.debugTerminalWindow->Log("Serial PCI NO WORK :(!");
            //     else
            //         osData.debugTerminalWindow->Log("Serial Port NO WORK :(!");
            // }
            RemoveFromStack();
            SerialWorks = false;
            return false;
        }
        else
        {
            if (pciCard != 0)
                ;//osData.debugTerminalWindow->Log("Serial PCI WORKS! :)");
        }
        
        // If serial is not faulty set it in normal operation mode
        // (not-loopback with IRQs enabled and OUT#1 and OUT#2 bits enabled)
        // if (pciCard == 0)
        //     Soutb(4, 0x0F);
        // else
        //     Soutb(4, 0x03);    // OUT yes

        io_wait(100);
        Soutb(4, 0x0F);
        RemoveFromStack();
        SerialWorks = true;
        //currentSerialReadPacket = NULL;
        currentSerialReadPacketIndex = 0;
        return true;
    }

    bool CanRead()
    {
        return _CanRead();
    }

    char Read()
    {
        return _Read();
    }

    bool CanWrite()
    {
        return _CanWrite();
    }
    void Write(char chr)
    {
        _Write(chr);
    }

    bool _CanRead()
    {
        return SerialWorks && (Sinb(5) & 1 == 1);
    }

    char _Read()
    {
        if (!SerialWorks)
            return 0;
        while (!_CanRead());
        return Sinb(0);
    }

    bool _CanWrite()
    {
        return SerialWorks && (Sinb(5) & 0x20);
    }

    void _Write(char chr)
    {
        if (!SerialWorks)
            return;
        while (!_CanWrite());
        Soutb(0, chr);
    }

    void Writeln()
    {
        Write("\r\n");
    }

    void Writeln(const char* str)
    {
        Write(str);
        Writeln();
    }
    void Writeln(const char *chrs, const char *var)
    {
        Writeln(chrs, var, true);
    }
    void Writeln(const char *chrs, const char *var, bool allowEscape)
    {
        Write(chrs, var, allowEscape);
        Writeln();
    }

    void Write(const char* str)
    {
        if (!SerialWorks || str == NULL)
            return;
        
        while (*str != 0)
        {
            Write(*str);
            str++;
        }
    }

    void Write(const char *chrs, const char *var)
    {
        Write(chrs, var, true);
    }

    void Write(const char *chrs, const char *var, bool allowEscape)
    {
        if (!SerialWorks || chrs == 0)
            return;

        uint32_t index = 0;
        while (chrs[index] != 0)
        {
            if (chrs[index] == '\n')
            {
                Serial::Write('\n');
            }
            else if (chrs[index] == '\r')
            {
                Serial::Write('\r');
            }
            else if (chrs[index] == '{' && allowEscape && var != NULL)
            {
                if (chrs[index + 1] == '}')
                {
                    Write(var, NULL, false);
                    index++;
                }
            }
            else if (chrs[index] == '\\' && allowEscape)
            {
                if (chrs[index + 1] == '\\')
                {
                    index++;
                    Write(chrs[index]);
                }
                else if (chrs[index + 1] == '%')
                {
                    index++;
                    Write('%');
                }
                else if (chrs[index + 1] == '{')
                {
                    index++;
                    Write('{');
                }
                else if (chrs[index + 1] == '}')
                {
                    index++;
                    Write('}');
                }
                else if (chrs[index + 1] == 'C')
                {
                    index++;
                    if (chrs[index + 1] == 0 || chrs[index + 2] == 0 || chrs[index + 3] == 0 || chrs[index + 4] == 0 || chrs[index + 5] == 0 || chrs[index + 6] == 0)
                    {
                        Write('?');
                    }
                    else
                    {
                        index++;
                        //color = ConvertStringToHex(&chrs[index]);
                        Write("<Switching Color>");
                        index += 5;
                    }
                }
                else
                {
                    Write(chrs[index]);
                }
            }
            else
            {
                Write(chrs[index]);
            }

            index++;
        }
    }

    void Writef(const char* str, ...)
    {
        va_list arg;
        va_start(arg, str);
        _Writef(str, arg);
        va_end(arg);
    }

    void Writelnf(const char* str, ...)
    {
        va_list arg;
        va_start(arg, str);
        _Writef(str, arg);
        va_end(arg);
        Writeln();
    }

    void PrintVal(int32_t val, int32_t CharCount)
    {
        const char* str = to_string(val);
        int32_t len = strlen(str);
        for (int32_t i = 0; i < CharCount - len; i++)
            Write('0');
        Write(str);
    }

    void WriteTime()
    {
        if (!PIT::Inited)
        {
            Write("[??:??.???] ");
            return;
        }

        int32_t s = PIT::TimeSinceBootMS() / 1000;
        int32_t minutes = s / 60;
        int32_t seconds = s % 60;
        int32_t ms = PIT::TimeSinceBootMS() % 1000;

        Write("[");
        PrintVal(minutes, 2);
        Write(":");
        PrintVal(seconds, 2);
        Write(".");
        PrintVal(ms, 3);
        Write("] ");
    }

    void TWrite(const char* str)
    {
        WriteTime();
        Write(str);
    }

    void TWriteln(const char* str)
    {
        WriteTime();
        Writeln(str);
    }

    void TWritef(const char* str, ...)
    {
        WriteTime();
        va_list arg;
        va_start(arg, str);
        _Writef(str, arg);
        va_end(arg);
    }

    void TWritelnf(const char* str, ...)
    {
        WriteTime();
        va_list arg;
        va_start(arg, str);
        _Writef(str, arg);
        va_end(arg);
        Writeln();
    }


    // %s -> string
    // %c -> char
    // %d/i -> int32_t (32 bit)
    // %D/I -> int32_t (64 bit)
    // %x -> hex (32 bit)
    // %X -> hex (64 bit)
    // %b -> byte
    // %B -> bool
    // %f -> float
    // %F -> double
    // %% -> %

    __ffunc void _Writef(const char* str, va_list arg)
    {
        int32_t len = strlen(str);

        for (int32_t i = 0; i < len; i++)
        {
            if (str[i] == '%' && i + 1 < len)
            {
                i++;
                if (str[i] == 's')
                {
                    char* argStr = va_arg(arg, char*);
                    if (argStr != NULL)
                        Write(argStr);
                    else
                        Write("(null)");
                }
                else if (str[i] == 'c')
                {
                    char argChar = va_arg(arg, int32_t);
                    Write(argChar);
                }
                else if (str[i] == 'd' || str[i + 1] == 'i')
                {
                    int32_t argInt = va_arg(arg, int32_t);
                    Write(to_string(argInt));
                }
                else if (str[i] == 'D' || str[i + 1] == 'I')
                {
                    uint64_t argInt = va_arg(arg, uint64_t);
                    Write(to_string(argInt));
                }
                else if (str[i] == 'x')
                {
                    uint32_t argInt = va_arg(arg, uint32_t);
                    Write(ConvertHexToString(argInt));
                }
                else if (str[i] == 'X')
                {
                    uint64_t argInt = va_arg(arg, uint64_t);
                    Write(ConvertHexToString(argInt));
                }
                else if (str[i] == 'b')
                {
                    uint8_t argInt = (uint8_t)va_arg(arg, int32_t);
                    Write(to_string(argInt));
                }
                else if (str[i] == 'B')
                {
                    bool argInt = (bool)va_arg(arg, int32_t);
                    Write(to_string(argInt));
                }
                else if (str[i] == 'f')
                {
                    // compiler be trolling me
                    // float argFloat = va_arg(arg, float);
                    // Write(to_string(argFloat));
                    

                    double argDouble = va_arg(arg, double);
                    Write(to_string(argDouble));
                }
                else if (str[i] == 'F')
                {
                    double argDouble = va_arg(arg, double);
                    Write(to_string(argDouble));
                }
                else if (str[i] == '%')
                {
                    Write('%');
                }
                else
                {
                    Write(str[i]);
                }
            }
            else
            {
                Write(str[i]);
            }
        }
    }

    void Soutb(uint16_t port, uint8_t value)
    {
        if (pciCard == 0)
            outb(SerialPort + port, value);
        else
            PCI::write_byte(pciCard, pciType, serialPciOffset + port, value);
    }

    uint8_t Sinb(uint16_t port)
    {
        if (pciCard == 0)
            return inb(SerialPort + port);
        else
            return PCI::read_byte(pciCard, pciType, serialPciOffset + port);
    }
}