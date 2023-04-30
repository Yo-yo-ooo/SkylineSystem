#pragma once
#include <stdint.h>

void wrmsr(uint32_t msr, uint32_t lo, uint32_t hi);
void outb(uint16_t port, uint8_t value);
uint8_t inb(uint16_t port);
void io_wait();   


void outw(uint16_t port, uint16_t value);
uint16_t inw(uint16_t port);

