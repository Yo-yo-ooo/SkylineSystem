#include "net.h"
#include "../devices/pci/pci.h"
#include "../kernelStuff/IO/IO.h"

static inline void OutS(unsigned short _port, unsigned short _data)
{
	asm volatile("outw %1, %0"
				 :
				 : "dN"(_port), "a"(_data));
}

static inline unsigned short InS(unsigned short _port)
{
	unsigned short rv;
	asm volatile("inw %1, %0"
				 : "=a"(rv)
				 : "dN"(_port));
	return rv;
}

#define TOK (1 << 2)
#define TER (1 << 3)
#define ROK (1 << 0) // Receive OK bit
#define TX_TOK (1 << 15)
#define RX_READ_POINTER_MASK (~3)

#define RX_BUF_SIZE 8192

Network::rtl8139_driver_t Network::RTL8139::rtl8139_device;
uint16_t ioaddr;
uint32_t current_packet_ptr;
uint8_t rtl8139_ready;
uint8_t TSAD_array[4] = {0x20, 0x24, 0x28, 0x2C};
uint8_t TSD_array[4] = {0x10, 0x14, 0x18, 0x1C};

static void Network::RTL8139::ReceivePacket()
{
    uint16_t *t = (uint16_t *)(rtl8139_device.rx_buffer + current_packet_ptr);
    // Skip packet header, get packet length
    uint16_t packet_length = *(t + 1);

    t = t + 2;
    //Video::Xxd(t, packet_length);
    void *packet = _Malloc1(packet_length);
    memcpy(packet, t, packet_length);
    Network::Ethernet::Handler(packet, packet_length);

    current_packet_ptr = (current_packet_ptr + packet_length + 4 + 3) & RX_READ_POINTER_MASK;

    if (current_packet_ptr > RX_BUF_SIZE)
        current_packet_ptr -= RX_BUF_SIZE;

    OutS(ioaddr + 0x38, current_packet_ptr - 0x10);
}

static void Network::RTL8139::Handler()
{
    if (rtl8139_ready)
    {
        uint16_t status = InS(ioaddr + 0x3e);

        if (status & TOK)
        {
            //kprintf("Packet sent\n");
        }
        if (status & ROK)
        {
            // kprintf("Received packet\n");
            ReceivePacket();
        }

        OutS(ioaddr + 0x3E, 0x5);
    }
}

static void Network::RTL8139::ReadMacAddress()
{
    uint32_t mac_part1 = inl(ioaddr + 0x00);
    uint16_t mac_part2 = InS(ioaddr + 0x04);
    rtl8139_device.mac_address[0] = mac_part1 >> 0;
    rtl8139_device.mac_address[1] = mac_part1 >> 8;
    rtl8139_device.mac_address[2] = mac_part1 >> 16;
    rtl8139_device.mac_address[3] = mac_part1 >> 24;

    rtl8139_device.mac_address[4] = mac_part2 >> 0;
    rtl8139_device.mac_address[5] = mac_part2 >> 8;
    //kprintf("\n      MAC Address: %1:%1:%1:%1:%1:%1\n", mac_part1 >> 0, mac_part1 >> 8, mac_part1 >> 16, mac_part1 >> 24, mac_part2 >> 0, mac_part2 >> 8);
}

static void Network::RTL8139::SendPacket(void *data, uint32_t len)
{
    void *transfer_data = _Malloc1(len);
    memcpy(transfer_data, data, len);

    outl(ioaddr + TSAD_array[rtl8139_device.tx_index], (uint32_t)transfer_data);
    outl(ioaddr + TSD_array[rtl8139_device.tx_index++], len);
    if (rtl8139_device.tx_index > 3)
        rtl8139_device.tx_index = 0;
}

static void Network::RTL8139::GetMacAddress(uint8_t *src_mac_addr)
{
    memcpy(src_mac_addr, rtl8139_device.mac_address, 6);
}

static void Network::RTL8139::Init()
{
    //kprintf("\n      Initializing network");

    // Get PCI device information for RTL8139
    //pci_device *rtl8139_pci = //PCI::GetDevice(0x10EC, 0x8139);
    PCI::IOAddress *rtl8139_pci = PCI::GetDeviceName(0x10EC, 0x8139);
    // Read and set the I/O address for RTL8139
    uint16_t ret = PCI::ReadWord(rtl8139_pci.attrs.bus, rtl8139_pci.attrs.slot, rtl8139_pci.attrs.function, 0x10);
    ioaddr = (ret & (~0x3));

    uint32_t pci_command_reg = PCI::ReadWord(rtl8139_pci.attrs.bus, rtl8139_pci.attrs.slot, rtl8139_pci.attrs.function, 0x04);
    if (!(pci_command_reg & (1 << 2)))
    {
        pci_command_reg |= (1 << 2);
        PCI::Write(rtl8139_pci, 0x04, pci_command_reg);
    }

    // Send 0x00 to the CONFIG_1 register (0x52) to set the LWAKE + LWPTN to active high. this should essentially *power on* the device.
    outb(ioaddr + 0x52, 0x0);

    // Soft reset
    outb(ioaddr + 0x37, 0x10);
    while ((inb(ioaddr + 0x37) & 0x10) != 0)
    {
        // Do nothing here...
    }

    // Allocate receive buffer
    rtl8139_device.rx_buffer = (char*)_Malloc1(RX_BUF_SIZE);
    memset(rtl8139_device.rx_buffer, 0x0, RX_BUF_SIZE);
    outl(ioaddr + 0x30, (uint32_t)rtl8139_device.rx_buffer);

    // Sets the TOK and ROK bits high
    OutS(ioaddr + 0x3C, 0x0005);

    // (1 << 7) is the WRAP bit, 0xf is AB+AM+APM+AAP
    outl(ioaddr + 0x44, 0xf | (1 << 7));

    // Sets the RE and TE bits high
    outb(ioaddr + 0x37, 0x0C);
    //Video::Print(0xFFFFFF, " [ ");
    //Video::Print(0x00FF00, "OK");
    //Video::Print(0xFFFFFF, " ]");
    rtl8139_ready = 1;

    uint8_t ip_address[] = {10, 0, 2, 2};
    uint8_t offset = 0x3C;
    //char hello_world[] = "Hello UDP from RTL8139!";
    //int len = strlen(hello_world);

    Network::RTL8139::ReadMacAddress();
    Network::ARP::Init();
    //Network::Dhcp::Discover();
    //Network::Udp::SendPacket(ip_address, 90, 8080, hello_world, len);
}
