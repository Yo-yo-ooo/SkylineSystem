/*
* SPDX-License-Identifier: GPL-2.0-only
* File: atapi.h
* Copyright (C) 2026 Yo-yo-ooo
*
* This file is part of SkylineSystem.
*
* SkylineSystem is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
*/
#ifdef __x86_64__
#include <drivers/atapi/atapi.h>
#include <klib/kio.h>

// Handy register number defines
#define DATA 0
#define ERROR_R 1
#define SECTOR_COUNT 2
#define LBA_LOW 3
#define LBA_MID 4
#define LBA_HIGH 5
#define DRIVE_SELECT 6
#define COMMAND_REGISTER 7

// Control register defines
#define CONTROL 0x206

#define ALTERNATE_STATUS 0

// This code is to wait 400 ns
static void ata_io_wait(const uint8_t p) {
	inb(p + CONTROL + ALTERNATE_STATUS);
	inb(p + CONTROL + ALTERNATE_STATUS);
	inb(p + CONTROL + ALTERNATE_STATUS);
	inb(p + CONTROL + ALTERNATE_STATUS);
}


uint8_t ATAPI::Read(bool slave, uint32_t lba, uint32_t sectors, uint16_t *buffer){
    volatile uint8_t read_cmd[12] = {0xA8, 0,
	                                 (lba >> 0x18) & 0xFF, (lba >> 0x10) & 0xFF, (lba >> 0x08) & 0xFF,
	                                 (lba >> 0x00) & 0xFF,
	                                 (sectors >> 0x18) & 0xFF, (sectors >> 0x10) & 0xFF, (sectors >> 0x08) & 0xFF,
	                                 (sectors >> 0x00) & 0xFF,
	                                 0, 0};

	outb(this->Port + DRIVE_SELECT, 0xA0 & (slave << 4)); // Drive select
	ata_io_wait(this->Port);
	outb(this->Port + ERROR_R, 0x00); 
	outb(this->Port + LBA_MID, 2048 & 0xFF);
	outb(this->Port + LBA_HIGH, 2048 >> 8);
	outb(this->Port + COMMAND_REGISTER, 0xA0); // Packet command
	ata_io_wait(this->Port); // I think we might need this delay, not sure, so keep this
 
    // Wait for status
	while (true) {
		uint8_t status = inb(this->Port + COMMAND_REGISTER);
		if ((status & 0x01) == 1)
			return 1;
		if (!(status & 0x80) && (status & 0x08))
			break;
		ata_io_wait(this->Port);
	}

    // Send command
	outsw(this->Port + DATA, (uint16_t *) read_cmd, 6);

    // Read words
	for (uint32_t i = 0; i < sectors; i++) {
        // Wait until ready
		while (true) {
			uint8_t status = inb(this->Port + COMMAND_REGISTER);
			if (status & 0x01)
				return 1;
			if (!(status & 0x80) && (status & 0x08))
				break;
		}

		int size = inb(this->Port + LBA_HIGH) << 8
		           | inb(this->Port + LBA_MID); // Get the size of transfer

		insw(this->Port + DATA, (uint16_t *) ((uint8_t *) buffer + i * 0x800), size / 2); // Read it
	}

	return 0;
}
uint8_t ATAPI::Write(bool slave, uint32_t lba, uint32_t sectors, uint16_t *buffer) {
    // 0xAA is the opcode for WRITE (12)
    volatile uint8_t write_cmd[12] = {
        0xAA, 0,
        (uint8_t)((lba >> 24) & 0xFF), (uint8_t)((lba >> 16) & 0xFF), (uint8_t)((lba >> 8) & 0xFF),
        (uint8_t)((lba >> 0) & 0xFF),
        (uint8_t)((sectors >> 24) & 0xFF), (uint8_t)((sectors >> 16) & 0xFF), (uint8_t)((sectors >> 8) & 0xFF),
        (uint8_t)((sectors >> 0) & 0xFF),
        0, 0
    };

    // 1. Select Drive
    outb(this->Port + DRIVE_SELECT, 0xA0 | (slave << 4));
    ata_io_wait(this->Port);

    // 2. Inform the drive of the byte count limit (2048 bytes per sector)
    outb(this->Port + ERROR_R, 0x00); 
    outb(this->Port + LBA_MID, 0x00);
    outb(this->Port + LBA_HIGH, 0x08); // 0x0800 = 2048
    
    // 3. Send PACKET command
    outb(this->Port + COMMAND_REGISTER, 0xA0); 
    ata_io_wait(this->Port);

    // 4. Wait for the drive to be ready for the packet (DRQ must be set)
    while (true) {
        uint8_t status = inb(this->Port + COMMAND_REGISTER);
        if (status & 0x01) return 1;          // Error bit set
        if (!(status & 0x80) && (status & 0x08)) break; // Not busy and DRQ set
        ata_io_wait(this->Port);
    }

    // 5. Send the 12-byte ATAPI command packet
    outsw(this->Port + DATA, (uint16_t *)write_cmd, 6);

    // 6. Data Transfer Loop
    for (uint32_t i = 0; i < sectors; i++) {
        // Wait for DRQ to indicate drive is ready to receive data
        while (true) {
            uint8_t status = inb(this->Port + COMMAND_REGISTER);
            if (status & 0x01) return 1; 
            if (!(status & 0x80) && (status & 0x08)) break;
        }

        // Get the size the drive expects (should be 2048)
        uint16_t size = inb(this->Port + LBA_HIGH) << 8 | inb(this->Port + LBA_MID);

        // Write the data from the buffer to the drive
        outsw(this->Port + DATA, (uint16_t *)((uint8_t *)buffer + (i * 2048)), size / 2);
    }

    // 7. Wait for final completion
    while (inb(this->Port + COMMAND_REGISTER) & 0x80);

    return 0;
}
#endif