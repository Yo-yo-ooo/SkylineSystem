#pragma once
#include "../../kernelStuff/diskStuff/Disk_Interfaces/generic/genericDiskInterface.h"
#include "../../osData/osData.h"
#include "../../devices/rtc/rtc.h"
//#define bool short int
//#define true 1
//#define false 0
//规定磁盘信息
#define SECTOR_SIZE 512
#define ROOT_DICT_NUM 224
#define DATA_SECTOR_NUM 2880
#define FAT_SECTOR_NUM 9
#define CLUS_NUM 384
#define ROOT_DICT_NUM 224
//规定文件类型
#define REGULAR_TYPE 0x20
#define DIRECTORY_TYPE 0x10

//规定数据类型
//typedef unsigned char uint8_t;
//typedef unsigned short uint16_t;
//typedef unsigned int uint32_t;
typedef char Sector[512];
struct MBRHeader;
struct FileDescriptor;

// MBR首部结构
typedef struct MBRHeader
{
    char BS_jmpBOOT[3];
    char BS_OEMName[8];
    // 每个扇区字节数 512
    uint16_t BPB_BytesPerSec;
    // 每簇扇区数 1
    uint8_t BPB_SecPerClus;
    // boot引导占扇区数 1
    uint16_t BPB_ResvdSecCnt;
    //一共有几个FAT表 2
    uint8_t BPB_NumFATs;
    //根目录文件最大数  0xe0 = 224
    uint16_t BPB_RootEntCnt;
    //扇区总数  0xb40 = 2880
    uint16_t BPB_TotSec16;
    //介质描述  0xf0
    uint8_t BPB_Media;
    //每个FAT表占扇区数 9
    uint16_t BPB_FATSz16;
    // 每个磁道占扇区数 0x12
    uint16_t BPB_SecPerTrk;
    // 磁头数   0x2
    uint16_t BPB_NumHeads;
    // 隐藏扇区数 0
    uint32_t BPB_HiddSec;
    // 如果BPB_TotSec16=0,则由这里给出扇区数 0
    uint32_t BPB_TotSec32;
    // INT 13H的驱动号 0
    uint8_t BS_DrvNum;
    //保留，未用    0
    uint8_t BS_Reserved1;
    //扩展引导标记  0x29
    uint8_t BS_BootSig;
    // 卷序列号 0
    uint32_t BS_VollD;
    // 卷标 'yxr620'
    uint8_t BS_VolLab[11];
    // 文件系统类型 'FAT12'
    uint8_t BS_FileSysType[8];
    //引导代码
    char code[448];
    //结束标志
    char end_point[2];

} __attribute__((packed)) MBRHeader;

// FAT项结构，由于计算机是按字节操作空间，所以扩展一个FAT项（1.5字节）为两个
typedef struct FAT2Entry
{
    int firstEntry : 12;
    int secondEntry : 12;
} __attribute__((packed)) FAT2Entry;

//文件描述符定义
typedef struct FileDescriptor
{
    uint8_t DIR_Name[8];   //文件名
    uint8_t DIR_Type[3];   //文件类型/扩展名
    uint8_t DIR_Attr;      //文件类型
    uint8_t Reserved[10];  //保留位
    uint16_t WrtTime;      //最后一次写入时间
    uint16_t WrtDate;      //最后一次写入日期
    uint16_t DIR_FstClus;  //此条目对应的开始簇数
    uint32_t DIR_FileSize; //文件大小
} __attribute__((packed)) FileDescriptor;

//磁盘定义
typedef struct Disk
{
    MBRHeader MBR;                               // 1个扇区
    FAT2Entry FAT1[CLUS_NUM / 2];                // 9个扇区;
    FAT2Entry FAT2[CLUS_NUM / 2];                // 9个扇区
    FileDescriptor rootDirectory[ROOT_DICT_NUM]; // 14个扇区
    Sector dataSector[DATA_SECTOR_NUM];          // 2880个扇区
} __attribute__((packed)) Disk;

//实例化一个磁盘disk
extern Disk disk;

void InitMBR();
void InitFAT();
void MakeFp(int hd,FileDescriptor *fp, char dirName[8], char dirType[3], uint8_t dirAttr, uint32_t fileSz);
void SetTime(FileDescriptor *fp);
int AllocSector(int hd,uint32_t sz);
int CreatRootDict(int hd,char *dictName);
void CreateDotDict(FileDescriptor *dotFp, const FileDescriptor *fp);
void CreateDDotDict(int hd,FileDescriptor *ddotFp, int fatherClus);
void WriteSector(int hd,char *buf, int idx);
void ReadSector(int hd,char *buf, int idx);
int ReadFp(int hd,char *path, FileDescriptor *fp);
int WriteFp(int hd,char *path, FileDescriptor *fp);
void WriteNewEntry(int hd,FileDescriptor *faFp, FileDescriptor *fp, int *newClus);
void ReadData(int hd,FileDescriptor *fp, char *buf);
void WriteData(int hd,char *buf, int bufSz, FileDescriptor *fp);
int ReadRootDict(char *rootName, FileDescriptor *fp);
int WriteRootDict(char *rootName, FileDescriptor *fp);
int MatchDict(char *fileName, char *buf, int bufSz, FileDescriptor *fp);
int CreateFile(int hd,char *fatherPath, char fileName[8], char fileType[3], uint8_t fileAttr, uint32_t fileSz);
void RemoveFp(int hd,FileDescriptor *fp);
int RemoveFile(int hd,char *fatherPath, char *fileName);

int ReadFile(int hd,char *path, char *buf);
int WriteFile(int hd,char *path, char *buf, int bufSz);