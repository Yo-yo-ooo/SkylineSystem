#include <fs/fd.h>
#include <fs/lwext4/ext4.h>

int32_t Ext4Write(fd_t* fsd, const void *buf, size_t count){
    ext4_file f = fsd->f;
    return ext4_fwrite(&f,buf,count,0);
}

int32_t Ext4Read(fd_t* fsd, void *buf, size_t count){
    ext4_file f = fsd->f;
    return ext4_fread(&f,buf,count,0);
}

int32_t Ext4Lseek(fd_t* fsd, int32_t offset, int32_t whence){
    ext4_file f = fsd->f;
    return ext4_fseek(&f,offset,whence);
}

int32_t Ext4Close(fd_t* fsd){
    ext4_file f = fsd->f;
    return ext4_fclose(&f);
}

struct FSOps FileSystemOps[256] = {
    {nullptr,nullptr,nullptr,nullptr}, //0:Unused
    {Ext4Write,Ext4Read,Ext4Lseek,Ext4Close}, //FS_EXT4
};