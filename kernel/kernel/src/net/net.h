#pragma once

#include "../kernelStuff/memory/memory.h"
#include "../memory/heap.h"
#include "../serialManager/serialManager.h"

#define memset(a,b,c) _memset(a,b,c)
#define memcpy(a,b,c) _memcpy(a,b,c)
#define memcmp(a,b,c) _memcmp(a,b,c)

#define IP_IPV4 4

#define IP_PACKET_NO_FRAGMENT 2
#define IP_IS_LAST_FRAGMENT 4

#define PROTOCOL_UDP 17
#define PROTOCOL_TCP 6

// RTL8139 I/O port addresses
#define RTL8139_REG_MAC 0x00
#define RTL8139_REG_TX 0x20
#define RTL8139_REG_TXSTAT 0x10
#define RTL8139_REG_COMMAND 0x37
#define RTL8139_CMD_TXEN 0x02

// RTL8139 transmit buffer size
#define RTL8139_TX_BUFFER_SIZE 2048
#define ETHERNET_TYPE_ARP 0x0806
#define ETHERNET_TYPE_IP 0x0800

#define HARDWARE_TYPE_ETHERNET 0x01
#define ARP_REQUEST 1
#define ARP_REPLY 2
#define ICMP_TYPE_ECHO_REQUEST 8
#define ICMP_TYPE_ECHO_REPLY 0
#define ICMP_CODE_ECHO_REPLY 0
#define DHCP_REQUEST 1
#define DHCP_REPLY 2

#define DHCP_TRANSACTION_IDENTIFIER 0x55555555

uint16_t flip_short(uint16_t short_int);

uint32_t flip_long(uint32_t long_int);

uint8_t flip_byte(uint8_t byte, int num_bits);

uint8_t htonb(uint8_t byte, int num_bits);

uint8_t ntohb(uint8_t byte, int num_bits);

uint16_t htons(uint16_t hostshort);

uint32_t htonl(uint32_t hostlong);

uint16_t ntohs(uint16_t netshort);

uint32_t ntohl(uint32_t netlong);

namespace Network
{

    typedef struct dhcp_packet
    {
        uint8_t op;
        uint8_t hardware_type;
        uint8_t hardware_addr_len;
        uint8_t hops;
        uint32_t xid;
        uint16_t seconds;
        uint16_t flags;     // may be broken
        uint32_t client_ip; //
        uint32_t your_ip;
        uint32_t server_ip;
        uint32_t gateway_ip;
        uint8_t client_hardware_addr[16];
        uint8_t server_name[64];
        uint8_t file[128];
        uint8_t options[64];
    } __attribute__((packed)) dhcp_packet_t;

    typedef struct
    {
        uint8_t type;
        uint8_t code;
        uint16_t checksum;
        uint16_t identifier;
        uint16_t sequence_number;
        uint8_t payload[4096];
        uint8_t payload_size;
        uint8_t source_ip;
    } icmp_packet_t;
    typedef struct udp_packet
    {
        uint16_t src_port;
        uint16_t dst_port;
        uint16_t length;
        uint16_t checksum;
        uint8_t data[];
    } __attribute__((packed)) udp_packet_t;

    typedef struct arp_packet
    {
        uint16_t hardware_type;
        uint16_t protocol;
        uint8_t hardware_addr_len;
        uint8_t protocol_addr_len;
        uint16_t opcode;
        uint8_t src_hardware_addr[6];
        uint8_t src_protocol_addr[4];
        uint8_t dst_hardware_addr[6];
        uint8_t dst_protocol_addr[4];
    } __attribute__((packed)) arp_packet_t;

    typedef struct arp_table_entry
    {
        uint32_t ip_addr;
        uint64_t mac_addr;
    } arp_table_entry_t;

    typedef struct ethernet_frame
    {
        uint8_t dst_mac_addr[6];
        uint8_t src_mac_addr[6];
        uint16_t type;
        uint8_t data[];
    } __attribute__((packed)) ethernet_frame_t;

    typedef struct ip_packet
    {
        char version_ihl_ptr[0];
        uint8_t version : 4;
        uint8_t ihl : 4;
        uint8_t tos;
        uint16_t length;
        uint16_t id;
        char flags_fragment_ptr[0];
        uint8_t flags : 3;
        uint8_t fragment_offset_high : 5;
        uint8_t fragment_offset_low;
        uint8_t ttl;
        uint8_t protocol;
        uint16_t header_checksum;
        uint8_t src_ip[4];
        uint8_t dst_ip[4];
        uint8_t data[];
    } __attribute__((packed)) ip_packet_t;
    typedef struct
    {
        uint32_t address;
        uint32_t length;
        uint32_t status;
        uint32_t command;
    } __attribute__((packed)) rtl8139_tx_desc_t;

    // RTL8139 driver data structure
    typedef struct
    {
        uint8_t mac_address[6];
        uint8_t tx_buffer[RTL8139_TX_BUFFER_SIZE];
        char *rx_buffer;
        rtl8139_tx_desc_t *tx_desc;
        size_t tx_index;
    } rtl8139_driver_t;

    class RTL8139
    {
    public:
        static rtl8139_driver_t Network::RTL8139::rtl8139_device;
        static void Network::RTL8139::SendPacket(void *data, uint32_t len);
        static void Network::RTL8139::ReceivePacket();
        static void Network::RTL8139::Handler();
        static void Network::RTL8139::ReadMacAddress();
        static void Network::RTL8139::GetMacAddress(uint8_t *src_mac_addr);
        static void Network::RTL8139::Init();
    };

    class Udp
    {
    public:
        static uint16_t Network::Udp::CalculateChecksum(Network::udp_packet_t * packet);

        static void Network::Udp::SendPacket(uint8_t *dst_ip, uint16_t src_port, uint16_t dst_port, void *data, int len);

        static void Network::Udp::Handler(udp_packet_t *packet);
    };

    class Dhcp
    {
    public:
        static int Network::Dhcp::GetHostAddr(char *addr);

        static void Network::Dhcp::Discover();

        static void Network::Dhcp::Request(uint8_t *request_ip);

        static void Network::Dhcp::Handler(dhcp_packet_t *packet);

        static void * Network::Dhcp::GetOptions(dhcp_packet_t *packet, uint8_t type);

        static void Network::Dhcp::MakePacket(dhcp_packet_t *packet, uint8_t msg_type, uint8_t *request_ip);
    };

    class IPv4
    {
    public:
        static void Network::IPv4::GetIpString(char *ip_str, uint8_t *ip);

        static uint16_t Network::IPv4::CalculateChecksum(ip_packet_t *packet);

        static void Network::IPv4::SendPacket(uint8_t *dst_ip, void *data, int len);

        static void Network::IPv4::Handler(ip_packet_t *packet);
    };

    class ARP
    {
    public:
        static void Network::ARP::Handler(arp_packet_t *arp_packet, int len);

        static void Network::ARP::SendPacket(uint8_t *dst_hardware_addr, uint8_t *dst_protocol_addr);

        static int Network::ARP::Lookup(uint8_t *ret_hardware_addr, uint8_t *ip_addr);

        static void Network::ARP::LookupAdd(uint8_t *ret_hardware_addr, uint8_t *ip_addr);

        static void Network::ARP::Init();
    };

    class Ethernet
    {
    public:
        static int Network::Ethernet::SendPacket(uint8_t *dst_mac_addr, uint8_t *data, int len, uint16_t protocol);

        static void Network::Ethernet::Handler(ethernet_frame_t *packet, int len);

        static void Network::Ethernet::Init();
    };
};