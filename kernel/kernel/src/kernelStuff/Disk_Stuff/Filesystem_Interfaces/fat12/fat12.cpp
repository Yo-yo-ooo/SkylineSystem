#include "fat12.h"
#include "../vfs/vfs.h"
#include "../../../../cmdParsing/cstrTools.h"
#include "../../../../memory/heap.h"
#include "../../../memory/memory.h"

#define align8(x) (((((x)-1)>>3)<<3)+8)

char *fgets(char *s, int n,  FILE *stream)
{
   register int c;
   register char *cs;
   cs = s;

   while(--n > 0 && (c = getc(stream)) != EOF)
   {
      if((*cs++ =   c) == '\n')
      {
         break;
      }
   }

   *cs = '\0';
   return (c == EOF && cs == s) ? NULL : s ;
}

void *calloc(size_t number, size_t size) {
    size_t *news;
    size_t s8, i;
    news = _Malloc1(number * size);
    if(news) {
        s8 = align8(number * size) >> 3;
        for(i = 0; i < s8; i++)
            news[i] = 0;
    }
    return news;
}


#define memset(s, c, n) _memset(s, c, n)
#define memcpy(d, s, n) _memcpy(d, s, n)

Disk disk;

//初始化MBR首部
void InitMBR()
{
    memset(disk.MBR.BS_jmpBOOT, 3, 0);     //跳转指令，这里不需要就设置为0
    memcpy(disk.MBR.BS_OEMName, "yhy", 3); //厂商名字，随便设置
    disk.MBR.BPB_BytesPerSec = 512;
    disk.MBR.BPB_SecPerClus = 1;
    disk.MBR.BPB_ResvdSecCnt = 1;
    disk.MBR.BPB_NumFATs = 2;
    disk.MBR.BPB_RootEntCnt = ROOT_DICT_NUM;
    disk.MBR.BPB_TotSec16 = 2880;
    disk.MBR.BPB_Media = 0xf0;
    disk.MBR.BPB_FATSz16 = FAT_SECTOR_NUM;
    disk.MBR.BPB_SecPerTrk = 0x12;
    disk.MBR.BPB_NumHeads = 0x2;
    disk.MBR.BPB_HiddSec = 0;
    disk.MBR.BPB_TotSec32 = 0;
    disk.MBR.BS_DrvNum = 0;
    disk.MBR.BS_Reserved1 = 0;
    disk.MBR.BS_BootSig = 0x29;
    disk.MBR.BS_VollD = 0;
    memcpy(disk.MBR.BS_VolLab, "C", 1);
    memcpy(disk.MBR.BS_FileSysType, "FAT12", 5);
    memset(disk.MBR.code, 0, 448);
    disk.MBR.end_point[0] = 0x55;
    disk.MBR.end_point[1] = 0xAA;
}

//初始化所有FAT表项
void InitFAT()
{
    memset(disk.FAT1, 0x00, sizeof(disk.FAT1));
    memset(disk.FAT2, 0x00, sizeof(disk.FAT2));
    //第一个簇是“坏簇”
    disk.FAT1[0].firstEntry = 0xFF0;
    disk.FAT2[0].firstEntry = 0xFF0;
}

//根据给定的参数，填写一个文件描述符
void MakeFp(FileDescriptor *fp, char dirName[8], char dirType[3], uint8_t dirAttr, uint32_t fileSz)
{
    //根据给定参数填写Fp
    memcpy(fp->DIR_Name, dirName, 8);
    memcpy(fp->DIR_Type, dirType, 3);
    fp->DIR_Attr = dirAttr;
    memset(fp->Reserved, 0, 10);
    SetTime(fp);
    fp->DIR_FileSize = fileSz;
    int firstClus = AllocSector(fileSz);
    if (firstClus == -1)
    {
        return;
    }
    else
    {
        fp->DIR_FstClus = firstClus;
    }
}

//获取当前时间并按照规则填写到文件描述符中
void SetTime(FileDescriptor *fp)
{
    //获取时间
    fp->WrtDate = NULL;
    fp->WrtTime = NULL;
}

//申请能容纳sz大小的空间（512的整数倍），成功返回申请到的首簇号，失败返回-1
int AllocSector(uint32_t sz)
{
    //分配大小为sz字节的空间，若成功返回首簇号，失败返回-1
    int secNum = sz / SECTOR_SIZE + (sz % SECTOR_SIZE == 0 ? 0 : 1); //所需扇区数
    int *secRecord = NULL;                                           //记录分配扇区序号的动态数组，大小为secNum
    secRecord = (int *)calloc(secNum, sizeof(int));
    bool judge = false;
    int i = 0, j;
    // printf("i=%d,secNum=%d\n", i, secNum);
    for (j = 1; j < CLUS_NUM / 2; j++) //从第3个簇开始分配，依次寻找空闲簇
    {
        if (i == secNum)
        {
            judge = true;
            break;
        }
        // printf("firstEntry=%x,secondEntry=%x\n", disk.FAT1[1].firstEntry, disk.FAT1[1].secondEntry);
        if ((disk.FAT1[j].firstEntry & 0xFFF) == 0x000) //偶数簇
        {
            secRecord[i] = j * 2;
            i++;
        }
        if (i == secNum)
        {
            judge = true;
            break;
        }
        if ((disk.FAT1[j].secondEntry & 0xFFF) == 0x000) //奇数簇
        {
            secRecord[i] = j * 2 + 1;
            i++;
        }
    }
    if (judge == false)
    {
        //printf("磁盘空间不足！");
        return -1;
    }
    else
    {
        //分配扇区
        for (j = 0; j < secNum - 1; j++)
        {
            int entryIndex = secRecord[j] / 2;
            if (secRecord[j] % 2 == 0)
            {
                disk.FAT1[entryIndex].firstEntry = (secRecord[j + 1] & 0xFFF);
                disk.FAT2[entryIndex].firstEntry = (secRecord[j + 1] & 0xFFF);
            }
            else
            {
                disk.FAT1[entryIndex].secondEntry = (secRecord[j + 1] & 0xFFF);
                disk.FAT2[entryIndex].firstEntry = (secRecord[j + 1] & 0xFFF);
            }
        }
        int lastClus = secRecord[secNum - 1];
        if (lastClus % 2 == 0)
        {
            disk.FAT1[lastClus / 2].firstEntry = 0xFFF;
            disk.FAT2[lastClus / 2].firstEntry = 0xFFF;
        }
        else
        {
            disk.FAT1[lastClus / 2].secondEntry = 0xFFF;
            disk.FAT2[lastClus / 2].secondEntry = 0xFFF;
        }

        //将分配的数据区清0
        char emptyBuf[SECTOR_SIZE];
        memset(emptyBuf, 0, SECTOR_SIZE);
        for (int i = 0; i < secNum; i++)
        {
            WriteSector(emptyBuf, secRecord[i]);
        }
    }
    int ret = secRecord[0];
    free(secRecord);
    return ret;
}

//创建一个名为dictName的根目录项
int CreatRootDict(char *dictName)
{
    FileDescriptor fp;
    memset(&fp, 0, sizeof(fp));
    char fileType[3] = "";
    MakeFp(&fp, dictName, fileType, DIRECTORY_TYPE, 512);
    //创建根目录项，成功返回0，失败返回-1
    for (int i = 0; i < ROOT_DICT_NUM; i++)
    {
        if (disk.rootDirectory[i].DIR_Name[0] == 0)
        {
            memcpy(&disk.rootDirectory[i], &fp, sizeof(FileDescriptor));
            FileDescriptor dotFp, ddotFp;
            CreateDotDict(&dotFp, &fp);
            CreateDDotDict(&ddotFp, 0);

            //将创建好的dot和ddot目录项填充到对应磁盘中
            int newClus;
            WriteNewEntry(&fp, &dotFp, &newClus);
            WriteNewEntry(&fp, &ddotFp, &newClus);
            return 0;
        }
    }
    return -1;
}

//创建"."目录项
void CreateDotDict(FileDescriptor *dotFp, const FileDescriptor *fp)
{
    //为文件目录项创建相应的dot目录项，指向文件本身
    memcpy(dotFp, fp, sizeof(FileDescriptor));
    char dotName[8] = "."; // dot目录和文件目录仅名字不一样
    memcpy(dotFp->DIR_Name, dotName, sizeof(dotFp->DIR_Name));
    dotFp->DIR_FileSize = 0; //除此之外可以把它的size设置为0
}

//创建".."目录项
void CreateDDotDict(FileDescriptor *ddotFp, int fatherClus)
{
    char ddotName[8] = "..";
    char ddotType[3] = "";
    MakeFp(ddotFp, ddotName, ddotType, DIRECTORY_TYPE, 0);
    ddotFp->DIR_FstClus = fatherClus; // ddot的FstClus字段为父目录FstClus，若父目录为根目录，则设置为0
}

//根据路径读取文件描述符，成功返回0，失败返回-1
//参数说明：
// path：目录路径
// fp：找到的文件描述符
//返回值：
int ReadFp(char *path, FileDescriptor *fp)
{
    //根据路径读取对应的文件描述符

    // step1：先读取根目录的内容
    FileDescriptor tmpFp;
    ReadRootDict("/", &tmpFp);
    int tmpBufSz = ((tmpFp.DIR_FileSize + SECTOR_SIZE - 1) / SECTOR_SIZE) * SECTOR_SIZE;
    char *tmpBuf = (char *)calloc(tmpBufSz, sizeof(char));
    ReadData(&tmpFp, tmpBuf);

    // step2：再逐层读取路径目录
    char tmp[20] = {0};
    int i = 1, j = 0;
    bool isEmpty; //用于判断tmp字符串是否为全0
    int ret;
    while (1)
    {
        memset(tmp, 0, 20);
        j = 0;
        isEmpty = true;
        for (; path[i] != '/' && path[i] != 0; i++)
        {
            isEmpty = false;
            tmp[j] = path[i];
            j++;
        }
        if (isEmpty == true)
            break;

        ret = MatchDict(tmp, tmpBuf, tmpBufSz, &tmpFp); //从对应容器中读取fp
        if (ret == -1)
        {
            //printf("No such file or directory.\n");
            return -1;
        }
        else
        {
            if (path[i] != 0 && tmpFp.DIR_Attr == REGULAR_TYPE)
            {
                //printf("Not a directory.\n");
                return -1;
            }
        }

        if (path[i] == 0)
        {
            break;
        }

        //更新bufSz及buf
        tmpBufSz = ((tmpFp.DIR_FileSize + SECTOR_SIZE - 1) / SECTOR_SIZE) * SECTOR_SIZE;
        tmpBuf = (char *)calloc(tmpBufSz, sizeof(char));
        ReadData(&tmpFp, tmpBuf);

        i++;
    }
    memcpy(fp, &tmpFp, sizeof(FileDescriptor));
    free(tmpBuf);
    return 0;
}

//根据路径修改并写回对应的文件描述符
int WriteFp(char *path, FileDescriptor *fp)
{
    // step0:如果path与根目录（这里是"/"相同），则直接写根目录即可
    if (strcmp("/", path) == 0)
    {
        return WriteRootDict(path, fp);
    }
    // step1：先读取根目录的内容
    FileDescriptor tmpFp;  //用于迭代
    FileDescriptor tmpFp2; //用于匹配
    ReadRootDict("/", &tmpFp);

    int tmpBufSz = ((tmpFp.DIR_FileSize + SECTOR_SIZE - 1) / SECTOR_SIZE) * SECTOR_SIZE;
    char *tmpBuf = (char *)calloc(tmpBufSz, sizeof(char));
    ReadData(&tmpFp, tmpBuf);
    // step2：再逐层读取路径目录
    char tmp[20] = {0};
    int i = 1, j = 0;
    int ret;
    while (1)
    {
        memset(tmp, 0, 20);
        j = 0;
        for (; path[i] != '/' && path[i] != 0; i++)
        {
            tmp[j] = path[i];
            j++;
        }

        ret = MatchDict(tmp, tmpBuf, tmpBufSz, &tmpFp2); //从对应容器中读取fp
        if (ret == -1)
        {
            //printf("No such file or directory.\n");
            return -1;
        }
        else
        {
            if (path[i] != 0 && tmpFp2.DIR_Attr == REGULAR_TYPE)
            {
                //printf("Not a directory.\n");
                return -1;
            }
        }

        if (path[i] == 0)
        {
            break;
        }

        //更新bufSz及buf
        tmpBufSz = ((tmpFp2.DIR_FileSize + SECTOR_SIZE - 1) / SECTOR_SIZE) * SECTOR_SIZE;
        tmpBuf = (char *)calloc(tmpBufSz, sizeof(char));
        ReadData(&tmpFp2, tmpBuf);
        memcpy(&tmpFp, &tmpFp2, sizeof(FileDescriptor));
        i++;
    }
    memcpy(tmpBuf + ret, fp, sizeof(FileDescriptor));
    WriteData(tmpBuf, tmpBufSz, &tmpFp);
    free(tmpBuf);
    return 0;
}

//向faFp的数据区中填写表项fp
//参数说明
// faFp:父目录
// fp:新目录
// newClus:用于指示是否在填写表项的时候申请了新空间，若没有返回-1，有则返回申请的首簇号
void WriteNewEntry(FileDescriptor *faFp, FileDescriptor *fp, int *newClus)
{
    //先读出faFp中的数据内容
    int bufSz = ((faFp->DIR_FileSize + SECTOR_SIZE - 1) / SECTOR_SIZE) * SECTOR_SIZE;
    char *buf = (char *)calloc(bufSz, sizeof(char));
    ReadData(faFp, buf);
    //再检查buf中有没有合适的空位用于填写fp
    FileDescriptor *tmpFp;
    char cmpStr[32] = {0}; //用于判断tmpFp所指fp是否为全0
    for (int i = 0; i < bufSz; i += sizeof(FileDescriptor))
    {
        tmpFp = (FileDescriptor *)(buf + i);
        if (strcmp((char *)tmpFp, cmpStr) == 0)
        { //用于比较是否有“空位”
            memcpy(tmpFp, fp, sizeof(FileDescriptor));
            WriteData(buf, bufSz, faFp);
            *newClus = -1;
            return;
        }
    }

    //如果没有“空位”，则需要先申请空间，再另外填入
    //另外申请一块即可
    *newClus = AllocSector(SECTOR_SIZE);
    char newBuf[SECTOR_SIZE] = {0};
    memcpy(newBuf, fp, sizeof(FileDescriptor));
    WriteSector(newBuf, *newClus);

    //修改父目录的大小和FAT表
    faFp->DIR_FileSize = faFp->DIR_FileSize + SECTOR_SIZE;
    int faFstClus = faFp->DIR_FstClus;
    int lastClus = faFstClus;
    int tmpClus = (faFstClus % 2 == 0) ? disk.FAT1[faFstClus / 2].firstEntry : disk.FAT1[faFstClus / 2].secondEntry;
    while ((tmpClus & 0xFFF) != 0xFFF)
    {
        lastClus = tmpClus;
        tmpClus = (tmpClus % 2 == 0) ? disk.FAT1[tmpClus / 2].firstEntry : disk.FAT1[tmpClus / 2].secondEntry;
    }
    if (lastClus % 2 == 0)
    {
        disk.FAT1[lastClus / 2].firstEntry = (*newClus & 0xFFF);
        disk.FAT2[lastClus / 2].firstEntry = (*newClus & 0xFFF);
    }
    else
    {
        disk.FAT1[lastClus / 2].secondEntry = (*newClus & 0xFFF);
        disk.FAT2[lastClus / 2].secondEntry = (*newClus & 0xFFF);
    }
    free(buf);
    return;
}

//创给定参数在父目录下创建新文件
//返回值 0：创建成功；-1：父目录不存在；-2：文件已在当前目录下存在
int CreateFile(char *fatherPath, char fileName[8], char fileType[3], uint8_t fileAttr, uint32_t fileSz)
{
    // step0:找到父路径的文件描述符
    FileDescriptor fp, fatherFp;
    MakeFp(&fp, fileName, fileType, fileAttr, fileSz);

    if (ReadFp(fatherPath, &fatherFp) == -1)
        return -1;
    // step1:检查当前目录下是否已经存在所需创建的文件
    char newFileName[20] = {0};
    char tmpFileName[20] = {0};
    strcpy(newFileName, fileName);
    strcat(newFileName, fileType);
    int bufSz = ((fatherFp.DIR_FileSize + SECTOR_SIZE - 1) / SECTOR_SIZE) * SECTOR_SIZE;
    char *buf = (char *)calloc(bufSz, sizeof(char));
    ReadData(&fatherFp, buf);
    FileDescriptor *tmpFp;
    for (int i = 0; i < bufSz; i += sizeof(FileDescriptor))
    {
        tmpFp = (FileDescriptor *)(buf + i);
        if (tmpFp->DIR_Name[0] != 0)
        {

            strcpy(tmpFileName, (char *)tmpFp->DIR_Name);
            strcat(tmpFileName, (char *)tmpFp->DIR_Type);
            if (strcmp(newFileName, tmpFileName) == 0 && fileAttr == tmpFp->DIR_Attr)
            {
                return -2;
            }
        }
    }
    // step2:如果创建的文件是目录项，创建新fp，并填写它的dot项和ddot项
    int newClus;
    if (fileAttr == DIRECTORY_TYPE)
    {
        FileDescriptor dotFp, ddotFp;
        CreateDotDict(&dotFp, &fp);
        CreateDDotDict(&ddotFp, fp.DIR_FstClus);
        WriteNewEntry(&fp, &dotFp, &newClus);
        WriteNewEntry(&fp, &ddotFp, &newClus);
    }
    // step3:将新fp写入faFp的数据区内
    WriteNewEntry(&fatherFp, &fp, &newClus);
    // step4:设置faFp的修改时间，并将其写回
    SetTime(&fatherFp);
    WriteFp(fatherPath, &fatherFp);
    free(buf);
    return 0;
}

//模拟磁盘写
void WriteSector(char *buf, int idx)
{
    memcpy(disk.dataSector[idx], buf, SECTOR_SIZE);
}
//模拟磁盘写
void ReadSector(char *buf, int idx)
{
    memcpy(buf, disk.dataSector[idx], SECTOR_SIZE);
}

//读取文件数据
void ReadData(FileDescriptor *fp, char *buf)
{
    int fstClus = fp->DIR_FstClus;
    //读取以fstClus为首簇的内容
    int tmpClus = fstClus;
    int i = 0;
    do
    {
        // printf("tmpClus=%x\n", disk.FAT1[0].firstEntry);
        ReadSector(buf + i * SECTOR_SIZE, tmpClus);

        if (tmpClus % 2 == 0)
        {
            tmpClus = disk.FAT1[tmpClus / 2].firstEntry;
        }
        else
        {
            tmpClus = disk.FAT1[tmpClus / 2].secondEntry;
        }
    } while ((tmpClus & 0xFFF) != 0xFFF);
}

//将buf内容写入文件fp的数据区
void WriteData(char *buf, int bufSz, FileDescriptor *fp)
{
    int fstClus = fp->DIR_FstClus;
    // step0:如果首簇号为0，说明此时还没有为该文件分配簇
    if (fstClus == 0)
    {
        if (bufSz > 0)
        {
            //如果写入内容长度大于0，那么得先为该文件分配簇
            int newFstClus = AllocSector(bufSz);
            if (newFstClus == -1)
            {
                return;
            }
            fp->DIR_FstClus = newFstClus;
            fp->DIR_FileSize = bufSz;
            fstClus = newFstClus; //更新fstClus
        }
        else
        {
            return;
        }
    }

    // step1:先判断该簇共“包含”了多少个簇，并记录最后一个簇的序号
    int clusNum = (fp->DIR_FileSize + SECTOR_SIZE - 1) / SECTOR_SIZE;
    int *clusRecord = (int *)calloc(clusNum, sizeof(int));
    int idx = 0;

    int tmpClus = fstClus;
    do
    {
        clusRecord[idx] = tmpClus;
        tmpClus = (tmpClus % 2 == 0) ? disk.FAT1[tmpClus / 2].firstEntry : disk.FAT1[tmpClus / 2].secondEntry;

    } while (tmpClus & 0xFFF != 0xFFF);
    int lastClus = clusRecord[clusNum - 1];
    // printf("lastClus = %d\n", lastClus);

    // step2:判断是否需要“扩容”或“缩容”
    int origSz = clusNum * SECTOR_SIZE;                                  //原本的大小
    int nowSz = ((bufSz + SECTOR_SIZE - 1) / SECTOR_SIZE) * SECTOR_SIZE; //满足写入buf所需的大小
    if (origSz < nowSz)
    {
        //先扩容
        int extraSz = ((bufSz + SECTOR_SIZE - 1) / SECTOR_SIZE) * SECTOR_SIZE - origSz;
        tmpClus = AllocSector(extraSz); //额外申请空间
        // printf("tmpClus = %d\n", tmpClus);
        if (tmpClus == -1) //申请失败
        {
            return;
        }
        if (lastClus % 2 == 0)
        {
            disk.FAT1[lastClus / 2].firstEntry = tmpClus & 0xFFF;
            disk.FAT2[lastClus / 2].firstEntry = tmpClus & 0xFFF;
        }
        else
        {
            disk.FAT1[lastClus / 2].secondEntry = tmpClus & 0xFFF;
            disk.FAT2[lastClus / 2].firstEntry = tmpClus & 0xFFF;
        }
    }
    else if (origSz > nowSz)
    {
        //这时候“缩容”
        int shrinkNum = (nowSz - origSz) / SECTOR_SIZE;

        //释放FAT表项
        int i;
        for (i = clusNum - 1; i > clusNum - 1 - shrinkNum; i--)
        {
            int clusIdx = clusRecord[i];
            if (clusIdx % 2 == 0)
            {
                disk.FAT1[clusIdx / 2].firstEntry = 0x000;
                disk.FAT2[clusIdx / 2].firstEntry = 0x000;
            }
            else
            {
                disk.FAT1[clusIdx / 2].secondEntry = 0x000;
                disk.FAT1[clusIdx / 2].secondEntry = 0x000;
            }
        }

        //设置fp的相关信息
        fp->DIR_FileSize = bufSz;
        if (i < 0)
        {
            fp->DIR_FstClus = 0;
            return;
        }
    }

    //写入内容
    int i = 0;
    tmpClus = fstClus;
    do
    {
        WriteSector(buf + i, tmpClus);
        tmpClus = (tmpClus % 2 == 0) ? disk.FAT1[tmpClus / 2].firstEntry : disk.FAT1[tmpClus / 2].secondEntry;
        i += SECTOR_SIZE;
    } while ((tmpClus & 0xFFF) != 0xFFF);
    free(clusRecord);
}

//根据名字读取对应的根目录，成功返回0，失败返回-1
int ReadRootDict(char *rootName, FileDescriptor *fp)
{

    for (int i = 0; i < ROOT_DICT_NUM; i++)
    {

        memcpy(fp, (void *)&disk.rootDirectory[i], sizeof(FileDescriptor));
        if (strcmp((char *)fp->DIR_Name, rootName) == 0)
        {
            return 0;
        }
    }
    return -1;
}
//根据名字写入对应的根目录，成功返回0，失败返回-1
int WriteRootDict(char *rootName, FileDescriptor *fp)
{
    for (int i = 0; i < ROOT_DICT_NUM; i++)
    {
        if (strcmp((char *)disk.rootDirectory[i].DIR_Name, rootName) == 0)
        {
            memcpy((void *)&disk.rootDirectory[i], fp, sizeof(FileDescriptor));
            return 0;
        }
    }
    return -1;
}

//从容器中匹配相应目录，匹配成功fileType返回相应文件描述符
//成功返回在该buf中的相对偏移量i(单位是fp的大小)，失败返回-1
int MatchDict(char *fileName, char *buf, int bufSz, FileDescriptor *fp)
{
    FileDescriptor tmpFp;
    int isMatch = -1;
    int i = 0;
    // printf("bufsz=%d\n", bufSz);
    while (i < bufSz)
    {
        memcpy(&tmpFp, buf + i, sizeof(FileDescriptor));
        char *fullName = (char *)calloc(8 + 3, sizeof(char)); //拼接获得文件全名
        strcpy(fullName, (char *)tmpFp.DIR_Name);
        if (tmpFp.DIR_Type[0] != 0) //如果文件类型不为空（通过第一个字节判断），还需要加上"."
        {
            strcat(fullName, ".");
        }
        strcat(fullName, (char *)tmpFp.DIR_Type);
        if (strcmp(fullName, fileName) == 0)
        {
            //匹配成功
            memcpy(fp, &tmpFp, sizeof(FileDescriptor));
            return i;
        }
        i += sizeof(FileDescriptor);
        free(fullName);
    }
    return -1;
}

//将以fstClus为首簇的“簇”链表簇所有FAT项清0
void ClearClus(int fstClus)
{
    //如果是初始的两个簇，则不能清0
    if (fstClus == 0 || fstClus == 1)
        return;
    //采取“头删法”，从头开始清零
    int tmpClus;
    while ((fstClus & 0xFFF) != 0xFFF)
    {
        tmpClus = ((fstClus % 2 == 0) ? disk.FAT1[fstClus / 2].firstEntry : disk.FAT1[fstClus / 2].secondEntry);
        //也不能释放未分配的簇
        if ((tmpClus & 0xFFF) == 0x000)
            return;
        if (fstClus % 2 == 0)
        {
            disk.FAT1[fstClus / 2].firstEntry = 0x000;
            disk.FAT2[fstClus / 2].firstEntry = 0x000;
        }
        else
        {
            disk.FAT1[fstClus / 2].secondEntry = 0x000;
            disk.FAT2[fstClus / 2].secondEntry = 0x000;
        }
        fstClus = tmpClus;
    }
}
//递归删除文件目录项，删除其所有子文件
void RemoveFp(FileDescriptor *fp)
{
    //如果文件为空(通过名字的第一个字符判断)，直接返回
    if (fp->DIR_Name[0] == 0)
        return;
    if (fp->DIR_Attr == REGULAR_TYPE)
    {
        //普通类型文件
        ClearClus(fp->DIR_FstClus);
    }
    else if (fp->DIR_Attr == DIRECTORY_TYPE)
    {
        int bufSz = ((fp->DIR_FileSize + SECTOR_SIZE - 1) / SECTOR_SIZE) * SECTOR_SIZE;
        char *buf = (char *)calloc(bufSz, sizeof(char));
        ReadData(fp, buf);
        int idx = 0;
        FileDescriptor *tmpFp;
        while (idx < bufSz)
        {
            tmpFp = (FileDescriptor *)(buf + idx);
            if (strcmp((char *)tmpFp->DIR_Name, ".") == 0 || strcmp((char *)tmpFp->DIR_Name, "..") == 0)
            {
                idx += sizeof(FileDescriptor);
                continue;
            }
            RemoveFp(tmpFp);
            idx += sizeof(FileDescriptor);
        }
        ClearClus(fp->DIR_FstClus);
        free(buf);
    }
}

//删除文件
//参数说明:
// filePath:要删除文件的父目录文件路径
// fileName:要删除的文件名称
int RemoveFile(char *fatherPath, char *fileName)
{
    //读取父目录的fp
    FileDescriptor fatherFp, rmFp;
    if (ReadFp(fatherPath, &fatherFp) == -1)
        return -1;
    if (fatherFp.DIR_Attr != DIRECTORY_TYPE)
    {
        //如果父目录不是目录类型，则报错并返回
        //printf("Not a dictionary.\n");
        return -1;
    }

    //读取父目录的内容
    int bufSz = ((fatherFp.DIR_FileSize + SECTOR_SIZE - 1) / SECTOR_SIZE) * SECTOR_SIZE;
    char *buf = (char *)calloc(bufSz, sizeof(char));
    ReadData(&fatherFp, buf);
    //从父目录的内容中读取目标文件描述符
    int ret = MatchDict(fileName, buf, bufSz, &rmFp);
    if (ret == -1)
    {
        printf("No such file or directory.\n");
        return -1;
    }

    //先移除目标目录下的所有子文件
    RemoveFp(&rmFp);

    //再从父目录的数据中清除该子目录并写回
    memset(buf + ret, 0, sizeof(FileDescriptor));

    WriteData(buf, bufSz, &fatherFp);
    free(buf);
    return 0;
}
//进一步的封装，读取文件内容
int ReadFile(char *path, char *buf)
{
    FileDescriptor fp;
    if (ReadFp(path, &fp) == -1)
    {
        return -1;
    }
    ReadData(&fp, buf);
    return 0;
}

//进一步的封装，写入文件内容
int WriteFile(char *path, char *buf, int bufSz)
{
    FileDescriptor fp;
    if (ReadFp(path, &fp) == -1)
    {
        return -1;
    }

    WriteData(buf, bufSz, &fp);

    //信息发生改变，回写fp
    WriteFp(path, &fp);
    return 0;
}

int fat12_mkdir(char* pathname){
    int lenpath = Strlen(pathname);

    if (lenpath <= 0)
    {
        //printf("missing operand\n"
               //"Try 'mkdir --help' for more information.\n");
        return -1;
    }
    for (int i = 0; i < lenpath; i++)
    {
        if (pathname[i] == '.' || pathname[i] == '/' || pathname[i] == '-')
        {
            //printf("illegal dir name");
            return -1;
        }
    }

    return CreateFile("/",pathname, "",DIRECTORY_TYPE,512);
}

VitualFileSystemOPS *ops = {
    VFS_REG(mkdir,fat12_mkdir),
    VFS_REG(open,NULL),
    VFS_REG(close,NULL),
    
}
