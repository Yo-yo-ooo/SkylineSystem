#include "net.h"

static int Network::Ethernet::SendPacket(uint8_t *dst_mac_addr, uint8_t *data, int len, uint16_t protocol)
{
    uint8_t src_mac_addr[6];
    Network::ethernet_frame_t *frame = (Network::ethernet_frame_t *)_Malloc1(sizeof(Network::ethernet_frame_t) + len);
    void *frame_data = (void *)frame + sizeof(Network::ethernet_frame_t);

    // Get source mac address from network card driver
    Network::RTL8139::GetMacAddress(src_mac_addr);

    // Fill in source and destination mac address
    memcpy(frame->src_mac_addr, src_mac_addr, 6);
    memcpy(frame->dst_mac_addr, dst_mac_addr, 6);

    // Fill in data
    memcpy(frame_data, data, len);

    // Fill in type
    frame->type = htons(protocol);

    // Send packet
    Network::RTL8139::SendPacket(frame, sizeof(ethernet_frame_t) + len);
    _Free(frame);

    return len;
}

static void Network::Ethernet::Handler(Network::ethernet_frame_t *packet, int len)
{
    void *data = (void *)packet + sizeof(Network::ethernet_frame_t);
    int data_len = len - sizeof(Network::ethernet_frame_t);

    // ARP packet
    // kprintf("\npacket->type %d", ntohs(packet->type));
    if (ntohs(packet->type) == ETHERNET_TYPE_ARP)
    {
        Network::ARP::Handler(data, data_len);
    }
    else if (ntohs(packet->type) == 0x08)
    {
        //kprintf("\nicmp");
    }
    else if (ntohs(packet->type) == ETHERNET_TYPE_IP)
    {
        // Convert Ethernet packet to IP packet
        Network::ip_packet_t *ip_packet = (ip_packet_t *)data;

        // Handle the IP packet
        Network::IPv4::Handler(ip_packet);
    }
}
