//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
#include <arch/x86_64/schedule/sched.h>
#include <arch/x86_64/schedule/syscall.h>
#include <klib/errno.h>
#include <elf/elf.h>
#include <mem/pmm.h>
#include <klib/kio.h>
#include <arch/x86_64/asm/prctl.h>

// IN LIBC This SYSCALL EXPRESS: uint64_t sys_dev_read(DevType dt,uint64_t DevIDX,...)
uint64_t sys_dev_read(uint64_t DevType,uint64_t DevIDX,\
uint64_t buffer,uint64_t count,uint64_t offset,uint64_t ign_0,syscall_frame_t* nullframe){
    IGNORE_VALUE(ign_0);IGNORE_VALUE(nullframe);
    /* 修复: 原实现把裸用户指针直接交给驱动 —— SMAP 开启时内核写用户页
       会 #PF, 且无任何范围校验。改为内核 bounce buffer + CopyToUser。 */
    if (count == 0) return 1;
    if (count > (1u << 20)) return 0; /* 单次上限 1MB, 防大分配 */
    void *kbuf = kmalloc(count);
    if (!kbuf) return 0;
    uint8_t r = Dev::DeviceRead((VsDevType)DevType,(uint32_t)DevIDX,(size_t)offset,kbuf,(size_t)count);
    uint64_t ret = 0;
    if (r && VMM::UserAccess::CopyToUser(Schedule::this_proc()->pagemap, buffer, kbuf, count))
        ret = 1;
    kfree(kbuf);
    return ret;
}

uint64_t sys_dev_write(uint64_t DevType,uint64_t DevIDX,\
uint64_t buffer,uint64_t count,uint64_t offset,uint64_t ign_0,syscall_frame_t* nullframe){
    IGNORE_VALUE(ign_0);IGNORE_VALUE(nullframe);
    /* 修复: 同上 —— CopyFromUser 到 bounce buffer 再交给驱动 */
    if (count == 0) return 1;
    if (count > (1u << 20)) return 0;
    void *kbuf = kmalloc(count);
    if (!kbuf) return 0;
    if (!VMM::UserAccess::CopyFromUser(Schedule::this_proc()->pagemap, kbuf, (void*)buffer, count)) {
        kfree(kbuf);
        return 0;
    }
    uint8_t r = Dev::DeviceWrite((VsDevType)DevType,DevIDX,offset,kbuf,count);
    kfree(kbuf);
    return r ? 1 : 0;
}

uint64_t sys_dev_mmap(uint64_t DevType,uint64_t DevIDX,
uint64_t length,uint64_t prot,uint64_t offset,uint64_t VADDR,syscall_frame_t *nullframe){
    IGNORE_VALUE(nullframe);
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
    /* 修复: (1) 删除对未校验 UserDesc 的 GetPhysics 空操作;
       (2) 原代码把 &dev.DescBaseAddr(栈上字段地址)当数据源, 拷贝
           DescLength 字节会越界读栈并泄漏内核地址 —— 应拷贝
           DescBaseAddr 指向的真实描述符内容;
       (3) 增加长度上限并检查拷贝结果。 */
    VDL dev = Dev::FindDevice((VsDevType)DevType, (uint32_t)DevIDX);
    if (dev.DescLength == 0 || dev.DescBaseAddr == 0)
        return -1; // 设备不存在
    if (UserDesc == 0)
        return -2; // 无效指针
    if(is_user_address(UserDesc) == false)
        return -3; // 只能写入用户态地址
    if (dev.DescLength > 4096)
        return -4; // 异常长度, 拒绝
    if (!VMM::UserAccess::CopyToUser(Schedule::this_proc()->pagemap, UserDesc,
                                     (const void*)dev.DescBaseAddr, dev.DescLength))
        return -5; // 拷贝失败
    return 0; // 成功
}