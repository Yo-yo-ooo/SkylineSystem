#include "net.h"

uint8_t my_ip[] = {10, 0, 2, 14};
uint8_t test_target_ip[] = {10, 0, 2, 1};
uint8_t zero_hardware_addr[] = {0, 0, 0, 0, 0, 0};

static uint16_t Network::IPv4::CalculateChecksum(Network::ip_packet_t * packet)
{
    // Treat the packet header as a 2-byte-integer array
    // Sum all integers up and flip all bits
    int array_size = sizeof(ip_packet_t) / 2;
    uint16_t *array = (uint16_t *)packet;
    uint8_t *array2 = (uint8_t *)packet;
    uint32_t sum = 0;
    for (int i = 0; i < array_size; i++)
    {
        sum += flip_short(array[i]);
    }
    uint32_t carry = sum >> 16;
    sum = sum & 0x0000ffff;
    sum = sum + carry;
    uint16_t ret = ~sum;
    return ret;
}

static void Network::IPv4::SendPacket(uint8_t *dst_ip, void *data, int len)
{
    int arp_sent = 3;
    Network::ip_packet_t *packet = (ip_packet *)_Malloc1(sizeof(ip_packet_t) + len);
    memset(packet, 0, sizeof(ip_packet_t));
    packet->version = IP_IPV4;
    // 5 * 4 = 20 byte
    packet->ihl = 5;
    // Don't care, set to 0
    packet->tos = 0;
    packet->length = sizeof(ip_packet_t) + len;
    // Used for ip fragmentation, don't care now
    packet->id = 0;
    // Tell router to not divide the packet, and this is packet is the last piece of the fragments.
    packet->flags = 0;
    packet->fragment_offset_high = 0;
    packet->fragment_offset_low = 0;

    packet->ttl = 64;
    // Normally there should be some other protocols embedded in ip protocol, we set it to udp for now, just for testing, the data could contain strings and anything
    // Once we test the ip packeting sending works, we'll replace it with a packet corresponding to some protocol
    packet->protocol = PROTOCOL_UDP;

    // gethostaddr(my_ip);
    memcpy(packet->src_ip, my_ip, 4);
    memcpy(packet->dst_ip, dst_ip, 4);

    void *packet_data = (void *)packet + packet->ihl * 4;
    memcpy(packet_data, data, len);

    // Fix packet data order
    *((uint8_t *)(&packet->version_ihl_ptr)) = htonb(*((uint8_t *)(&packet->version_ihl_ptr)), 4);
    *((uint8_t *)(packet->flags_fragment_ptr)) = htonb(*((uint8_t *)(packet->flags_fragment_ptr)), 3);
    packet->length = htons(sizeof(ip_packet_t) + len);

    // Make sure checksum is 0 before checksum calculation
    packet->header_checksum = 0;
    packet->header_checksum = htons(CalculateChecksum(packet));

    uint8_t dst_hardware_addr[6];

    while (!Network::ARP::Lookup(dst_hardware_addr, dst_ip))
    {
        if (arp_sent != 0)
        {
            arp_sent--;
            // Send an arp packet here
            Network::ARP::SendPacket(zero_hardware_addr, dst_ip);
        }
    }
    // Got the mac address! Now send an ethernet packet
    
    Network::Ethernet::SendPacket(dst_hardware_addr, (uint8_t *)packet, htons(packet->length), ETHERNET_TYPE_IP);
    // Video::Xxd(packet, ntohs(packet->length));
    _Free(packet);
}

static void Network::IPv4::Handler(Network::ip_packet_t *packet)
{
    // Fix packet data order (be careful with the endiness problem within a byte)
    *((uint8_t *)(&packet->version_ihl_ptr)) = ntohb(*((uint8_t *)(&packet->version_ihl_ptr)), 4);
    *((uint8_t *)(packet->flags_fragment_ptr)) = ntohb(*((uint8_t *)(packet->flags_fragment_ptr)), 3);

    // kprintf("Receive: the whole ip packet \n");
    // Video::Xxd(packet, ntohs(packet->length));

    if (packet->version == IP_IPV4)
    {
        // get_ip_str(src_ip, packet->src_ip);

        void *data_ptr = (void *)packet + packet->ihl * 4;
        int data_len = ntohs(packet->length) - sizeof(ip_packet_t);

        // kprintf("src: %s, data dump: \n", src_ip);
        //  Video::Xxd(data_ptr, data_len);

        // If this is a UDP packet
        
        if (packet->protocol == PROTOCOL_UDP)
        {
            Network::Udp::Handler(data_ptr);
        }
        if (packet->protocol == PROTOCOL_TCP)
        {
            //kprintf("\nTCP not implemented");
        }
    }
}