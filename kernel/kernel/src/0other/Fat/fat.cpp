#include "fat.h"
#include "../../devices/rtc/rtc.h"

static BiosParameterBlock32 bpb;

inline void* ReadSector(uint64_t addr,int NDisks){
    void *tmp;
    if(osData.diskInterfaces[NDisks]->ReadBytes(addr,512,tmp) != true) return (void*)0;
    return tmp;
}

inline bool WriteSector(uint64_t addr,int NDisks,void* buffer){
    return osData.diskInterfaces[NDisks]->WriteBytes(addr,512,buffer);
}

int isspace(char c)
{
    if(c =='\t'|| c =='\n'|| c ==' ')
        return 1;
    else
        return 0;
}

int atoi(char* pstr)
{
	int Ret_Integer = 0;
	int Integer_sign = 1;
	
	/*
	* 判断指针是否为空
	*/
	if(pstr == NULL)
	{
		//printf("Pointer is NULL\n");
		return 0;
	}
	
	/*
	* 跳过前面的空格字符
	*/
	while(isspace(*pstr) == 0)
	{
		pstr++;
	}
	
	/*
	* 判断正负号
	* 如果是正号，指针指向下一个字符
	* 如果是符号，把符号标记为Integer_sign置-1，然后再把指针指向下一个字符
	*/
	if(*pstr == '-')
	{
		Integer_sign = -1;
	}
	if(*pstr == '-' || *pstr == '+')
	{
		pstr++;
	}
	
	/*
	* 把数字字符串逐个转换成整数，并把最后转换好的整数赋给Ret_Integer
	*/
	while(*pstr >= '0' && *pstr <= '9')
	{
		Ret_Integer = Ret_Integer * 10 + *pstr - '0';
		pstr++;
	}
	Ret_Integer = Integer_sign * Ret_Integer;
	
	return Ret_Integer;
}

uint64_t GetFileAddress(uint64_t StartClusterNumber){
    uint64_t FAT1_OFFSET = 512 * bpb.reservedSectors;
    uint64_t FAT2_OFFSET = FAT1_OFFSET + 512 * sizeof(PartitionTableEntry);
    return (FAT2_OFFSET + 512 * sizeof(PartitionTableEntry)) + (StartClusterNumber - bpb.rootCluster) * 32
    * 512;
}

bool Write(int NDisk,char* name,int type){
    if(type == 1){
        DirectoryEntry de;
        char *sname = name;
        for(int i = 0;i < 8;i++){
            de.name[i] = sname[i];
        }
        for(int i = 0;i < 3;i++){
            de.extension[i] = sname[8 + i];
        }
        de.creationDate |= (RTC::Year  & 0x7f) << 9;
        de.creationDate |= (RTC::Month & 0x0f) << 5;
        de.creationDate |= (RTC::Day & 0x1f);
        de.creationTime |= (RTC::Hour & 0x1f) << 11;
        de.creationTime |= (RTC::Minute & 0x3f) << 5;
        de.reserved = 0;
        de.creationTimeTenth = de.creationTime;
        de.attributes = type;
        de.lastWriteDate = de.creationDate;
        de.lastWriteTime = de.creationTime;
        osData.diskInterfaces[NDisk]->WriteBytes(GetFileAddress(0x0C),sizeof(DirectoryEntry),(uint8_t*)&de);
    }
}

