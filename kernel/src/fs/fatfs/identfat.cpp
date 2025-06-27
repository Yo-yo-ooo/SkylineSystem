#include <partition/mbrgpt.h>
#include <partition/identfstype.h>

#include <fs/fatfs/ff.h>
#include <klib/klib.h>
#include <drivers/vsdev/vsdev.h>

#undef MAX_FAT12
#undef MAX_FAT16
#undef MAX_FAT32
#undef MAX_EXFAT

#undef BPB_FATSz16
#undef BPB_NumFATs
#undef BPB_RootEntCnt
#undef BPB_SecPerClus
#undef BPB_TotSec16
#undef BPB_RsvdSecCnt
#undef BPB_FATSz32
#undef SZDIRE
#undef BPB_TotSec32

/* Limits and boundaries */
#define MAX_FAT12	0xFF5			/* Max FAT12 clusters (differs from specs, but right for real DOS/Windows behavior) */
#define MAX_FAT16	0xFFF5			/* Max FAT16 clusters (differs from specs, but right for real DOS/Windows behavior) */
#define MAX_FAT32	0x0FFFFFF5		/* Max FAT32 clusters (not defined in specs, practical limit) */
#define MAX_EXFAT	0x7FFFFFFD		/* Max exFAT clusters (differs from specs, implementation limit) */

#define BPB_NumFATs			16		/* Number of FATs (BYTE) */
#define BPB_FATSz16			22		/* FAT size (16-bit) [sector] (WORD) */
#define BPB_RootEntCnt		17		/* Size of root directory area for FAT [entry] (WORD) */
#define BPB_SecPerClus		13		/* Cluster size [sector] (BYTE) */
#define BPB_TotSec16		19		/* Volume size (16-bit) [sector] (WORD) */
#define BPB_RsvdSecCnt		14		/* Size of reserved area [sector] (WORD) */
#define BPB_FATSz32			36		/* FAT32: FAT size [sector] (DWORD) */
#define SZDIRE				32		/* Size of a directory entry */
#define BPB_TotSec32		32		/* Volume size (32-bit) [sector] (DWORD) */

uint8_t IdentifyFat(uint32_t DriverID,uint32_t PartitionID){
    uint64_t nclst;
    uint64_t PStart;
    uint8_t buffer[36];
    uint32_t fasize,tsect,sysect;
    uint16_t nrsv;
    if(GetPartitionStart(DriverID,PartitionID,PStart) != 0){
        return 1;
    }elif(VsDev::DevList[DriverID].ops.ReadBytes(
            VsDev::DevList[DriverID].classp,
            PStart,36,buffer) == false){
            return 2;
    }else{
        fasize = kld_16(buffer + BPB_FATSz16);		/* Number of sectors per FAT */
		if (fasize == 0) fasize = kld_32(buffer + BPB_FATSz32);

#define fs_n_fats  buffer[BPB_NumFATs]			/* Number of FATs */
		if (fs_n_fats != 1 && fs_n_fats != 2) return PARTITION_TYPE_UNKNOWN;	/* (Must be 1 or 2) */
		fasize *= fs_n_fats;							/* Number of sectors for FAT area */
#undef fs_n_fats

#define	fs_csize  buffer[BPB_SecPerClus]			/* Cluster size */
		if (fs_csize == 0 || (fs_csize & (fs_csize - 1))) return PARTITION_TYPE_UNKNOWN;	/* (Must be power of 2) */

#define	fs_n_rootdir  kld_16(buffer + BPB_RootEntCnt)	/* Number of root directory entries */
		if (fs_n_rootdir % (512 / SZDIRE)) return PARTITION_TYPE_UNKNOWN;	/* (Must be sector aligned) */

		tsect = kld_16(buffer + BPB_TotSec16);			/* Number of sectors on the volume */
		if (tsect == 0) tsect = kld_32(buffer + BPB_TotSec32);

		nrsv = kld_16(buffer + BPB_RsvdSecCnt);			/* Number of reserved sectors */
		if (nrsv == 0) return PARTITION_TYPE_UNKNOWN;			/* (Must not be 0) */

		/* Determine the FAT sub type */
		sysect = nrsv + fasize + fs_n_rootdir / (512 / SZDIRE);	/* RSV + FAT + DIR */
		if (tsect < sysect) return PARTITION_TYPE_UNKNOWN;	/* (Invalid volume size) */
		nclst = (tsect - sysect) / fs_csize;			/* Number of clusters */
		if (nclst == 0) return PARTITION_TYPE_UNKNOWN;		/* (Invalid volume size) */
    }
    if (nclst <= MAX_FAT32) return PARTITION_TYPE_FAT32;
    if (nclst <= MAX_FAT16) return PARTITION_TYPE_FAT16;
    if (nclst <= MAX_FAT12) return PARTITION_TYPE_FAT12;
    return PARTITION_TYPE_UNKNOWN; 
}