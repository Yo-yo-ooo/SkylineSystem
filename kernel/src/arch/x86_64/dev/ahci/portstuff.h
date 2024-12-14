#pragma once

#include <stdint.h>

namespace AHCI 
{
    typedef struct SATA_Ident
    {
        uint16_t   config;      /* lots of obsolete bit flags */
        uint16_t   cyls;      /* obsolete */
        uint16_t   reserved2;   /* special config */
        uint16_t   heads;      /* "physical" heads */
        uint16_t   track_bytes;   /* unformatted bytes per track */
        uint16_t   sector_bytes;   /* unformatted bytes per sector */
        uint16_t   sectors;   /* "physical" sectors per track */
        uint16_t   vendor0;   /* vendor unique */
        uint16_t   vendor1;   /* vendor unique */
        uint16_t   vendor2;   /* vendor unique */
        uint8_t   serial_no[20];   /* 0 = not_specified */
        uint16_t   buf_type;
        uint16_t   buf_size;   /* 512 byte increments; 0 = not_specified */
        uint16_t   ecc_bytes;   /* for r/w long cmds; 0 = not_specified */
        uint8_t   fw_rev[8];   /* 0 = not_specified */
        uint8_t   model[40];   /* 0 = not_specified */
        uint16_t  multi_count; /* Multiple Count */
        uint16_t   dword_io;   /* 0=not_implemented; 1=implemented */
        uint16_t   capability1;   /* vendor unique */
        uint16_t   capability2;   /* bits 0:DMA 1:LBA 2:IORDYsw 3:IORDYsup word: 50 */
        uint8_t   vendor5;   /* vendor unique */
        uint8_t   tPIO;      /* 0=slow, 1=medium, 2=fast */
        uint8_t   vendor6;   /* vendor unique */
        uint8_t   tDMA;      /* 0=slow, 1=medium, 2=fast */
        uint16_t   field_valid;   /* bits 0:cur_ok 1:eide_ok */
        uint16_t   cur_cyls;   /* logical cylinders */
        uint16_t   cur_heads;   /* logical heads word 55*/
        uint16_t   cur_sectors;   /* logical sectors per track */
        uint16_t   cur_capacity0;   /* logical total sectors on drive */
        uint16_t   cur_capacity1;   /*  (2 words, misaligned int)     */
        uint8_t   multsect;   /* current multiple sector count */
        uint8_t   multsect_valid;   /* when (bit0==1) multsect is ok */
        uint32_t   lba_capacity;   /* total number of sectors */
        uint16_t   dma_1word;   /* single-word dma info */
        uint16_t   dma_mword;   /* multiple-word dma info */
        uint16_t  eide_pio_modes; /* bits 0:mode3 1:mode4 */
        uint16_t  eide_dma_min;   /* min mword dma cycle time (ns) */
        uint16_t  eide_dma_time;   /* recommended mword dma cycle time (ns) */
        uint16_t  eide_pio;       /* min cycle time (ns), no IORDY  */
        uint16_t  eide_pio_iordy; /* min cycle time (ns), with IORDY */
        uint16_t   words69_70[2];   /* reserved words 69-70 */
        uint16_t   words71_74[4];   /* reserved words 71-74 */
        uint16_t  queue_depth;   /*  */
        uint16_t  sata_capability;   /*  SATA Capabilities word 76*/
        uint16_t  sata_additional;   /*  Additional Capabilities */
        uint16_t  sata_supported;   /* SATA Features supported  */
        uint16_t  features_enabled;   /* SATA features enabled */
        uint16_t  major_rev_num;   /*  Major rev number word 80 */
        uint16_t  minor_rev_num;   /*  */
        uint16_t  command_set_1;   /* bits 0:Smart 1:Security 2:Removable 3:PM */
        uint16_t  command_set_2;   /* bits 14:Smart Enabled 13:0 zero */
        uint16_t  cfsse;      /* command set-feature supported extensions */
        uint16_t  cfs_enable_1;   /* command set-feature enabled */
        uint16_t  cfs_enable_2;   /* command set-feature enabled */
        uint16_t  csf_default;   /* command set-feature default */
        uint16_t  dma_ultra;   /*  */
        uint16_t   word89;      /* reserved (word 89) */
        uint16_t   word90;      /* reserved (word 90) */
        uint16_t   CurAPMvalues;   /* current APM values */
        uint16_t   word92;         /* reserved (word 92) */
        uint16_t   comreset;      /* should be cleared to 0 */
        uint16_t  accoustic;      /*  accoustic management */
        uint16_t  min_req_sz;      /* Stream minimum required size */
        uint16_t  transfer_time_dma;   /* Streaming Transfer Time-DMA */
        uint16_t  access_latency;      /* Streaming access latency-DMA & PIO WORD 97*/
        uint32_t    perf_granularity;   /* Streaming performance granularity */
        uint32_t   total_usr_sectors[2]; /* Total number of user addressable sectors */
        uint16_t   transfer_time_pio;    /* Streaming Transfer time PIO */
        uint16_t   reserved105;       /* Word 105 */
        uint16_t   sector_sz;          /* Puysical Sector size / Logical sector size */
        uint16_t   inter_seek_delay;   /* In microseconds */
        uint16_t   words108_116[9];    /*  */
        uint32_t   words_per_sector;    /* words per logical sectors */
        uint16_t   supported_settings; /* continued from words 82-84 */
        uint16_t   command_set_3;       /* continued from words 85-87 */
        uint16_t  words121_126[6];   /* reserved words 121-126 */
        uint16_t   word127;         /* reserved (word 127) */
        uint16_t   security_status;   /* device lock function
                    * 15:9   reserved
                    * 8   security level 1:max 0:high
                    * 7:6   reserved
                    * 5   enhanced erase
                    * 4   expire
                    * 3   frozen
                    * 2   locked
                    * 1   en/disabled
                    * 0   capability
                    */
        uint16_t  csfo;      /* current set features options
                    * 15:4   reserved
                    * 3   auto reassign
                    * 2   reverting
                    * 1   read-look-ahead
                    * 0   write cache
                    */
        uint16_t   words130_155[26];/* reserved vendor words 130-155 */
        uint16_t   word156;
        uint16_t   words157_159[3];/* reserved vendor words 157-159 */
        uint16_t   cfa; /* CFA Power mode 1 */
        uint16_t   words161_175[15]; /* Reserved */
        uint8_t   media_serial[60]; /* words 176-205 Current Media serial number */
        uint16_t   sct_cmd_transport; /* SCT Command Transport */
        uint16_t   words207_208[2]; /* reserved */
        uint16_t   block_align; /* Alignement of logical blocks in larger physical blocks */
        uint32_t   WRV_sec_count; /* Write-Read-Verify sector count mode 3 only */
        uint32_t      verf_sec_count; /* Verify Sector count mode 2 only */
        uint16_t   nv_cache_capability; /* NV Cache capabilities */
        uint16_t   nv_cache_sz; /* NV Cache size in logical blocks */
        uint16_t   nv_cache_sz2; /* NV Cache size in logical blocks */
        uint16_t   rotation_rate; /* Nominal media rotation rate */
        uint16_t   reserved218; /*  */
        uint16_t   nv_cache_options; /* NV Cache options */
        uint16_t   words220_221[2]; /* reserved */
        uint16_t   transport_major_rev; /*  */
        uint16_t   transport_minor_rev; /*  */
        uint16_t   words224_233[10]; /* Reserved */
        uint16_t   min_dwnload_blocks; /* Minimum number of 512byte units per DOWNLOAD MICROCODE
                                    command for mode 03h */
        uint16_t   max_dwnload_blocks; /* Maximum number of 512byte units per DOWNLOAD MICROCODE
                                    command for mode 03h */
        uint16_t   words236_254[19];   /* Reserved */
        uint16_t   integrity;          /* Cheksum, Signature */
    } SATA_ident_t;
}