#include "net.h"
#include "../cStdLib/cStdLib.h"

char IpAddr[4];
int IsIpAllocated;

static int Network::Dhcp::GetHostAddr(char * addr) {
    memcpy(addr, IpAddr, 4);
    if(!IsIpAllocated) {
        return 0;
    }
    return 1;
}

static void Network::Dhcp::Discover() {
    //kprintf("      Running DHCP discover...\n");
    uint8_t request_ip[4] = {10, 0, 2, 2};
    uint8_t dst_ip[4] = {10, 0, 2, 2};
    Network::dhcp_packet_t * packet = (Network::dhcp_packet_t *)_Malloc1(sizeof(Network::dhcp_packet_t));
    memset(packet, 0, sizeof(Network::dhcp_packet_t));
    Network::Dhcp::MakePacket(packet, 1, request_ip);
    Network::Udp::SendPacket(request_ip, 6800, 6700, packet, sizeof(Network::dhcp_packet_t));
}

static void Network::Dhcp::Request(uint8_t * request_ip) {
    uint8_t dst_ip[4];
    memset(dst_ip, 0xff, 4);
    Network::dhcp_packet_t * packet = (Network::dhcp_packet_t *)_Malloc1(sizeof(Network::dhcp_packet_t));
    memset(packet, 0, sizeof(Network::dhcp_packet_t));
    Network::Dhcp::MakePacket(packet, 3, request_ip);
    Network::Udp::SendPacket(dst_ip, 6800, 6700, packet, sizeof(Network::dhcp_packet_t));
}

/*
 * Handle DHCP offer packet
 * */
static void Network::Dhcp::Handler(Network::dhcp_packet_t * packet) {
    uint8_t * options = packet->options + 4;
    //kprintf("\nGOT DHCP PACKET!!!");
    if(packet->op == DHCP_REPLY) {
        // DHCP Offer or ACK ?
        uint8_t* type = Network::Dhcp::GetOptions(packet, 53);
        //uint8_t * type = 0;
        if(*type == 2) {
            // Offer, return a request
            Network::Dhcp::Request((uint8_t *)&packet->your_ip);
        }
        else if (*type == 5) {
            // ACK, save necessary info(IP for example)
            memcpy(IpAddr, &packet->your_ip, 4);
            IsIpAllocated = 1;
        }
    }
}

/*
 * Search for the value of a type in options
 * */
static void * Network::Dhcp::GetOptions(Network::dhcp_packet_t * packet, uint8_t type) {
    uint8_t * options = packet->options + 4;
    uint8_t curr_type = *options;
    while(curr_type != 0xff) {
        uint8_t len = *(options + 1);
        if(curr_type == type) {
            // Found type, return value
            void * ret = _Malloc1(len);
            memcpy(ret, options + 2, len);
            return ret;
        }
        options += (2 + len);
    }
}

static void Network::Dhcp::MakePacket(Network::dhcp_packet_t * packet, uint8_t msg_type, uint8_t * request_ip) {
    packet->op = DHCP_REQUEST;
    packet->hardware_type = HARDWARE_TYPE_ETHERNET;
    packet->hardware_addr_len = 6; 
    packet->hops = 0;
    packet->xid = htonl(DHCP_TRANSACTION_IDENTIFIER);
    packet->flags = htons(0x8000);
    Network::RTL8139::GetMacAddress(packet->client_hardware_addr);

    // Send dhcp packet using UDP
    uint8_t dst_ip[4];
    memset(dst_ip, 0xff, 4);

    // Options specific to DHCP Discover (required)

    // Magic Cookie
    uint8_t * options = packet->options;
    *((uint32_t*)(options)) = htonl(0x63825363);
    options += 4;

    // First option, message type = DHCP_DISCOVER/DHCP_REQUEST
    *(options++) = 53;
    *(options++) = 1;
    *(options++) = msg_type;

    // Client identifier
    *(options++) = 61;
    *(options++) = 0x07;
    *(options++) = 0x01;
    Network::RTL8139::GetMacAddress(options);
    options += 6;

    // Requested IP address
    *(options++) = 50;
    *(options++) = 0x04;
    *((uint32_t*)(options)) = htonl(0x0a00020e);
    memcpy((uint32_t*)(options), request_ip, 4);
    options += 4;

    // Host Name
    *(options++) = 12;
    *(options++) = 0x09;
    memcpy(options, "hydrogen", StrLen("hydrogen"));
    options += StrLen("hydrogen");
    *(options++) = 0x00;

    // Parameter request list
    *(options++) = 55;
    *(options++) = 8;
    *(options++) = 0x1;
    *(options++) = 0x3;
    *(options++) = 0x6;
    *(options++) = 0xf;
    *(options++) = 0x2c;
    *(options++) = 0x2e;
    *(options++) = 0x2f;
    *(options++) = 0x39;
    *(options++) = 0xff;

}