#include "net.h"


static uint16_t Network::Udp::CalculateChecksum(Network::udp_packet_t * packet) {
    // UDP checksum is optional in IPv4
    return 0;
}

static void Network::Udp::SendPacket(uint8_t * dst_ip, uint16_t src_port, uint16_t dst_port, void * data, int len) {
    int length = sizeof(Network::udp_packet_t) + len;
    Network::udp_packet_t * packet = (Network::udp_packet_t *)_Malloc1(length);
    memset(packet, 0, sizeof(Network::udp_packet_t));
    packet->src_port = htons(src_port);
    packet->dst_port = htons(dst_port);
    packet->length = htons(length);
    packet->checksum = Network::Udp::CalculateChecksum(packet);

    // Copy data over
    memcpy((void*)packet + sizeof(Network::udp_packet_t), data, len);
    //Video::Xxd(packet, length);
    Network::IPv4::SendPacket(dst_ip, packet, length);
    _Free(packet);
}

static void Network::Udp::Handler(Network::udp_packet_t * packet) {
    //uint16_t src_port = ntohs(packet->src_port);
    uint16_t dst_port = ntohs(packet->dst_port);
    uint16_t length = ntohs(packet->length);

    void * data_ptr = (void*)packet + sizeof(Network::udp_packet_t);
    uint32_t data_len = length;

    //Arch::Serial::WriteString(SERIAL_PORT_A, "\nReceived UDP packet, port is ");
    //Arch::Serial::WriteString(SERIAL_PORT_A, to_string64(dst_port));
    //Arch::Serial::WriteString(SERIAL_PORT_A, " data is: ");
    //Arch::Serial::WriteString(SERIAL_PORT_A, packet->data);
    //Video::Xxd(data_ptr, data_len);
    //kprintf("%s -- Received UDP packet: dst port: %d, src port: %d\n", Arch::RTC::GetDatetimeString(), packet->src_port, dst_port, packet->data);
    //kprintf("%s -- Received UDP packet: dst port: %d, src port: %d\n", Arch::RTC::GetDatetimeString(), dst_port, packet->src_port);
    if(ntohs(packet->dst_port) == 68) {
        //Video::Xxd(data_ptr, data_len);
        Network::Dhcp::Handler(data_ptr);
    }
    return;
}