#include <fs/fatfs/ff.h>
#include <klib/klib.h>
#include <drivers/dev/dev.h>
#include <fs/fatfs/identfat.h>

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
#undef BPB_ZeroedEx
#undef BPB_FSVerEx
#undef BPB_BytsPerSecEx
#undef BPB_TotSecEx
#undef BPB_FatSzEx
#undef BPB_NumFATsEx	
#undef BPB_SecPerClusEx
#undef BPB_NumClusEx
#undef BPB_DataOfsEx
#undef BPB_FatOfsEx
#undef BPB_RootClusEx

#undef ET_BITMAP

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
#define BPB_ZeroedEx		11		/* exFAT: MBZ field (53-byte) */
#define BPB_FSVerEx			104		/* exFAT: Filesystem version (WORD) */
#define BPB_BytsPerSecEx	108		/* exFAT: Log2 of sector size in unit of byte (BYTE) */
#define BPB_TotSecEx		72		/* exFAT: Volume size [sector] (QWORD) */
#define BPB_FatSzEx			84		/* exFAT: FAT size [sector] (DWORD) */
#define BPB_NumFATsEx		110		/* exFAT: Number of FATs (BYTE) */
#define BPB_SecPerClusEx	109		/* exFAT: Log2 of cluster size in unit of sector (BYTE) */
#define BPB_NumClusEx		92		/* exFAT: Number of clusters (DWORD) */
#define BPB_DataOfsEx		88		/* exFAT: Data offset from top of the volume [sector] (DWORD) */
#define BPB_FatOfsEx		80		/* exFAT: FAT offset from top of the volume [sector] (DWORD) */
#define BPB_RootClusEx		96		/* exFAT: Root directory start cluster (DWORD) */

#define	ET_BITMAP	0x81	/* Allocation bitmap */

static uint64_t _InFile_clst2sect (	/* !=0:Sector number, 0:Failed (invalid cluster#) */
	uint32_t fs_n_fatent,	uint64_t fs_database, uint16_t fs_csize,
	uint32_t clst		/* Cluster# to be converted */
)
{
	clst -= 2;		/* Cluster number is origin from 2 */
	if (clst >= fs_n_fatent - 2) return 0;		/* Is it invalid cluster number? */
	return fs_database + (uint64_t)fs_csize * clst;	/* Start sector number of the cluster */
}

FS_TYPE IdentifyFat(uint32_t DriverID,uint32_t PartitionID,bool IsDebug){
    uint64_t nclst;
    uint64_t PStart;
    uint8_t buffer[36];
    char FSName[8];
    uint32_t fasize,tsect,sysect;
    uint16_t nrsv;
    if(IsDebug == true)
        PStart = 0;
    else{
        Dev::SetSDev(DriverID);
        if(GetPartitionStart(DriverID,PartitionID,PStart) != 0)
            return {PARTITION_TYPE_UNKNOWN,5};
    }
    if(Dev::ReadBytes(PStart + 3,8,FSName) == false){
            return {PARTITION_TYPE_UNKNOWN,6};
    /*A simple check of exfat 
    Some Partition may not write "EXFAT   " 
    in [(BootSecotr + 3) ~ (BootSecotr + 11)] FileSystemName
    so it's unsafe*/
    }elif(strcmp(FSName,"EXFAT   ") == 0){
            return {PARTITION_TYPE_EXFAT,0};
    }else{
        if(Dev::ReadBytes(PStart,36,buffer) == false)
            return {PARTITION_TYPE_UNKNOWN,6};

//check_eexfat: /*Check FS type except ExFat*/
        fasize = kld_16(buffer + BPB_FATSz16);		/* Number of sectors per FAT */
        if (fasize == 0) fasize = kld_32(buffer + BPB_FATSz32);

#define fs_n_fats  buffer[BPB_NumFATs]			/* Number of FATs */
        if (fs_n_fats != 1 && fs_n_fats != 2) return {PARTITION_TYPE_UNKNOWN,7};	/* (Must be 1 or 2) */
        fasize *= fs_n_fats;							/* Number of sectors for FAT area */
#undef fs_n_fats

#define	fs_csize  buffer[BPB_SecPerClus]			/* Cluster size */
        if (fs_csize == 0 || (fs_csize & (fs_csize - 1))) return {PARTITION_TYPE_UNKNOWN,7};	/* (Must be power of 2) */

#define	fs_n_rootdir  kld_16(buffer + BPB_RootEntCnt)	/* Number of root directory entries */
        if (fs_n_rootdir % (512 / SZDIRE)) return {PARTITION_TYPE_UNKNOWN,7};	/* (Must be sector aligned) */

        tsect = kld_16(buffer + BPB_TotSec16);			/* Number of sectors on the volume */
        if (tsect == 0) tsect = kld_32(buffer + BPB_TotSec32);

        nrsv = kld_16(buffer + BPB_RsvdSecCnt);			/* Number of reserved sectors */
        if (nrsv == 0) return {PARTITION_TYPE_UNKNOWN,7};			/* (Must not be 0) */

        /* Determine the FAT sub type */
        sysect = nrsv + fasize + fs_n_rootdir / (512 / SZDIRE);	/* RSV + FAT + DIR */
        if (tsect < sysect) return {PARTITION_TYPE_UNKNOWN,7};	/* (Invalid volume size) */
        nclst = (tsect - sysect) / fs_csize;			/* Number of clusters */
        if (nclst == 0) return {PARTITION_TYPE_UNKNOWN,7};		/* (Invalid volume size) */
        uint8_t FST = PARTITION_TYPE_UNKNOWN;
        if (nclst <= MAX_FAT32) FST = PARTITION_TYPE_FAT32;
        if (nclst <= MAX_FAT16) FST = PARTITION_TYPE_FAT16;
        if (nclst <= MAX_FAT12) FST = PARTITION_TYPE_FAT12;
        return {FST,0};
    }
}