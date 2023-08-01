#include "net.h"

Network::arp_table_entry_t arp_table[512];
int arp_table_size;
int arp_table_curr;

uint8_t broadcast_mac_address[] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

void remove_last_bytes(uint8_t* data, size_t size, size_t N) {
    if (N >= size) {
        // If N is greater than or equal to the array size, clear the entire array
        _memset(data, 0, size);
    } else {
        // Set the last N bytes to zero
        _memset(data + size - N, 0, N);
    }
}

static void Network::ARP::Handler(arp_packet_t *arp_packet, int len) {
    char dst_hardware_addr[6];
    char dst_protocol_addr[4];
    // Save some packet field
    _memcpy(dst_hardware_addr, arp_packet->src_hardware_addr, 6);
    _memcpy(dst_protocol_addr, arp_packet->src_protocol_addr, 4);
    if(ntohs(arp_packet->opcode) == ARP_REQUEST) {
        uint8_t my_ip[] = {10, 0, 2, 14};
        remove_last_bytes((uint8_t *)arp_packet, len, 24); // some silly fix
        //Video::Xxd(arp_packet, len);
        //Arch::Serial::WriteString(SERIAL_PORT_A, "\nReceived ARP packet");
        //kprintf("\ndebug: arp_packet->dst_protocol_addr = %x, my_ip = %x", arp_packet->dst_protocol_addr, &my_ip);
        if(_memcmp(arp_packet->dst_protocol_addr, &my_ip, 4)) {
            //Arch::Serial::WriteString(SERIAL_PORT_A, "\nResponding to ARP packet");
            Network::RTL8139::GetMacAddress(arp_packet->src_hardware_addr);
            arp_packet->src_protocol_addr[0] = 10;
            arp_packet->src_protocol_addr[1] = 0;
            arp_packet->src_protocol_addr[2] = 2;
            arp_packet->src_protocol_addr[3] = 14;

            memcpy(arp_packet->dst_hardware_addr, dst_hardware_addr, 6);
            memcpy(arp_packet->dst_protocol_addr, dst_protocol_addr, 4);
            arp_packet->opcode = htons(ARP_REPLY);
            arp_packet->hardware_addr_len = 6;
            arp_packet->protocol_addr_len = 4;
            arp_packet->hardware_type = htons(HARDWARE_TYPE_ETHERNET);
            arp_packet->protocol = htons(ETHERNET_TYPE_IP);

            Network::Ethernet::SendPacket(dst_hardware_addr, (uint8_t*)arp_packet, sizeof(arp_packet_t), ETHERNET_TYPE_ARP);

        }
    }
    else if(ntohs(arp_packet->opcode) == ARP_REPLY){

    }
    else {
        kprintf(" arp opcode = %d\n", arp_packet->opcode);
    }

    // Now, store the ip-mac address mapping relation
    memcpy(&arp_table[arp_table_curr].ip_addr, dst_protocol_addr, 4);
    memcpy(&arp_table[arp_table_curr].mac_addr, dst_hardware_addr, 6);
    if(arp_table_size < 512)
        arp_table_size++;
    // Wrap around
    if(arp_table_curr >= 512)
        arp_table_curr = 0;

}


static void Network::ARP::SendPacket(uint8_t *dst_hardware_addr, uint8_t *dst_protocol_addr) {
    arp_packet_t * arp_packet = (arp_packet_t *)_Malloc1(sizeof(arp_packet_t));

    Network::RTL8139::GetMacAddress(arp_packet->src_hardware_addr);
    arp_packet->src_protocol_addr[0] = 10;
    arp_packet->src_protocol_addr[1] = 0;
    arp_packet->src_protocol_addr[2] = 2;
    arp_packet->src_protocol_addr[3] = 14;

    // Set destination MAC address, IP address
    memcpy(arp_packet->dst_hardware_addr, dst_hardware_addr, 6);
    memcpy(arp_packet->dst_protocol_addr, dst_protocol_addr, 4);

    // Set opcode
    arp_packet->opcode = htons(ARP_REQUEST);

    // Set lengths
    arp_packet->hardware_addr_len = 6;
    arp_packet->protocol_addr_len = 4;

    // Set hardware type
    arp_packet->hardware_type = htons(HARDWARE_TYPE_ETHERNET);

    // Set protocol = IPv4
    arp_packet->protocol = htons(ETHERNET_TYPE_IP);

    // Now send it with ethernet
    Network::Ethernet::SendPacket(broadcast_mac_address, (uint8_t*)arp_packet, sizeof(arp_packet_t), ETHERNET_TYPE_ARP);
    _Free(arp_packet);
}

static void Network::ARP::LookupAdd(uint8_t *ret_hardware_addr, uint8_t *ip_addr) {
    memcpy(&arp_table[arp_table_curr].ip_addr, ip_addr, 4);
    memcpy(&arp_table[arp_table_curr].mac_addr, ret_hardware_addr, 6);
    if(arp_table_size < 512)
        arp_table_size++;
    // Wrap around
    if(arp_table_curr >= 512)
        arp_table_curr = 0;
}

static int Network::ARP::Lookup(uint8_t *ret_hardware_addr, uint8_t *ip_addr){
    uint32_t ip_entry = *((uint32_t*)(ip_addr));
    for(int i = 0; i < 512; i++) {
        if(arp_table[i].ip_addr == ip_entry) {
            memcpy(ret_hardware_addr, &arp_table[i].mac_addr, 6);
            return 1;
        }
    }
    return 0;
}

static void Network::ARP::Init() {
    uint8_t broadcast_ip[4];
    uint8_t broadcast_mac[6];

    memset(broadcast_ip, 0xff, 4);
    memset(broadcast_mac, 0xff, 6);
    LookupAdd(broadcast_mac, broadcast_ip);
    //kprintf("      ARP is ready\n");
}