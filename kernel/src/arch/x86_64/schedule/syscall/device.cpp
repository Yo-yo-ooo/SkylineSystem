/*
* SPDX-License-Identifier: GPL-2.0-only
* File: device.cpp
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
#include <arch/x86_64/schedule/sched.h>
#include <arch/x86_64/schedule/syscall.h>
#include <klib/errno.h>
#include <elf/elf.h>
#include <mem/pmm.h>
#include <klib/kio.h>
#include <arch/x86_64/asm/prctl.h>

// IN LIBC This SYSCALL EXPRESS: uint64_t sys_dev_read(DevType dt,uint64_t DevIDX,...)
uint64_t sys_dev_read(uint64_t DevType,uint64_t DevIDX,\
uint64_t buffer,uint64_t count,uint64_t offset,uint64_t ign_0){
    IGNORE_VALUE(ign_0);
    return Dev::DeviceRead(DevType,DevIDX,offset,(void*)buffer,count);
}

uint64_t sys_dev_write(uint64_t DevType,uint64_t DevIDX,\
uint64_t buffer,uint64_t count,uint64_t offset,uint64_t ign_0){
    IGNORE_VALUE(ign_0);
    return Dev::DeviceWrite(DevType,DevIDX,offset,(void*)buffer,count);
}

uint64_t sys_dev_mmap(uint64_t DevType,uint64_t DevIDX,
uint64_t length,uint64_t prot,uint64_t offset,uint64_t VADDR){
    return Dev::DeviceMemoryMap(
        (VsDevType)DevType,
        (uint32_t)DevIDX,
        length,
        prot,
        offset,VADDR
    );
}

uint64_t sys_dev_ioctl(
    uint64_t DevType,uint64_t DevIDX,uint64_t cmd,uint64_t arg,
        GENERATE_IGN2()){
    IGNV_2();
    VDL dev = Dev::FindDevice((VsDevType)DevType, (uint32_t)DevIDX);
    return dev.ops.ioctl(cmd,arg);
}

//This function mainly get informathion desc base address of device
uint64_t sys_dev_getinfo(
    uint64_t DevType,uint64_t DevIDX,uint64_t UserDesc,
    GENERATE_IGN3()
){
    IGNV_3();
    uint64_t paddrud = VMM::GetPhysics(Schedule::this_proc()->pagemap, UserDesc) + hhdm_offset;
    VDL dev = Dev::FindDevice((VsDevType)DevType, (uint32_t)DevIDX);
    if (dev.DescLength == 0 || dev.DescBaseAddr == 0)
        return -1; // 设备不存在
    if (UserDesc == 0)
        return -2; // 无效指针
    if(is_user_address(UserDesc) == false)
        return -3; // 只能写入用户态地址
    else
        VMM::UserAccess::CopyToUser(Schedule::this_proc()->pagemap, UserDesc, &dev.DescBaseAddr, dev.DescLength);
    
    return 0; // 成功
}