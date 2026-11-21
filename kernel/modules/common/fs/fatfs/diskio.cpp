/*-----------------------------------------------------------------------*/
/* Low level disk I/O module SKELETON for FatFs     (C)ChaN, 2019        */
/*-----------------------------------------------------------------------*/
/* If a working storage control module is available, it should be        */
/* attached to the FatFs via a glue function rather than modifying it.   */
/* This is an example of glue functions to attach various exsisting      */
/* storage control modules to the FatFs module with a defined API.       */
/*-----------------------------------------------------------------------*/

#include <fs/fatfs/ff.h>			/* Obtains integer types */
#include <fs/fatfs/diskio.h>
#include <fs/fatfs/ffconf.h>		/* Declarations of disk functions */
#include <drivers/Disk_Interfaces/ram/ramDiskInterface.h>
#include <drivers/Disk_Interfaces/sata/sataDiskInterface.h>
#include <drivers/dev/dev.h>
/* Definitions of physical drive number for each drive */
#define DEV_RAM		0	/* Example: Map Ramdisk to physical drive 0 */
#define DEV_MMC		1	/* Example: Map MMC/SD card to physical drive 1 */
#define DEV_USB		2	/* Example: Map USB MSD to physical drive 2 */


/*-----------------------------------------------------------------------*/
/* Get Drive Status                                                      */
/*-----------------------------------------------------------------------*/

DSTATUS disk_status (
	BYTE pdrv		/* Physical drive nmuber to identify the drive */
)
{
	/* P1-41: 本 glue 为空壳 (read/write 恒 RES_ERROR), 却返回 0x0
	   (ready) 自相矛盾 → 诚实返回 STA_NOINIT */
	(void)pdrv;
	return STA_NOINIT;
}



/*-----------------------------------------------------------------------*/
/* Inidialize a Drive                                                    */
/*-----------------------------------------------------------------------*/

DSTATUS disk_initialize (
	BYTE pdrv				/* Physical drive nmuber to identify the drive */
)
{
/* 修复: 未使用变量已删除(原 DSTATUS stat; int32_t result;) */
/*
	switch (pdrv) {
	case DEV_RAM :
		result = RamDiskInterface::Init(512);

		// translate the reslut code here

		return stat;

	case DEV_MMC :
		result = MMC_disk_initialize();

		// translate the reslut code here

		return stat;

	case DEV_USB :
		result = USB_disk_initialize();

		// translate the reslut code here

		return stat;
	}
	return STA_NOINIT;
    */
	return STA_NOINIT;   /* P1-41: 空壳诚实语义 (原 0x0 = 假成功) */
}



/*-----------------------------------------------------------------------*/
/* Read Sector(s)                                                        */
/*-----------------------------------------------------------------------*/

DRESULT disk_read (
	BYTE pdrv,		/* Physical drive nmuber to identify the drive */
	BYTE *buff,		/* Data buffer to store read data */
	LBA_t sector,	/* Start sector in LBA */
	UINT count		/* Number of sectors to read */
)
{
	/* 修复: 原函数体被整体注释且无 return(UB, 返回值未定义)。
	   在接入块设备层之前显式返回错误, 避免 FatFs 拿到垃圾 DRESULT。 */
	(void)pdrv; (void)buff; (void)sector; (void)count;
	return RES_ERROR;
}



/*-----------------------------------------------------------------------*/
/* Write Sector(s)                                                       */
/*-----------------------------------------------------------------------*/

#if FF_FS_READONLY == 0

DRESULT disk_write (
	BYTE pdrv,			/* Physical drive nmuber to identify the drive */
	const BYTE *buff,	/* Data to be written */
	LBA_t sector,		/* Start sector in LBA */
	UINT count			/* Number of sectors to write */
)
{
	/* 修复: 同上 —— 原函数体注释掉且无 return, 属未定义行为。 */
	(void)pdrv; (void)buff; (void)sector; (void)count;
	return RES_ERROR;
}

#endif


/*-----------------------------------------------------------------------*/
/* Miscellaneous Functions                                               */
/*-----------------------------------------------------------------------*/

DRESULT disk_ioctl (
	BYTE pdrv,		/* Physical drive nmuber (0..) */
	BYTE cmd,		/* Control code */
	void *buff		/* Buffer to send/receive control data */
)
{
	/* 修复: 同上 —— 原函数体注释掉且无 return, 属未定义行为。 */
	(void)pdrv; (void)cmd; (void)buff;
	return RES_ERROR;
}

