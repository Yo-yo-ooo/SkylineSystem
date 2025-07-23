#include <fs/lwext4/ctvfs.h>
#include <fs/lwext4/blockdev/blockdev.h>
namespace EXT4_VFS
{

    vfs_fsinfo_t ext4 = {
         "ext4",
        false,
       { 0 },
       nullptr,
        EXT4_VFS::mount,
         nullptr,
        nullptr,
       nullptr,
       nullptr,
         nullptr,
       nullptr,
        nullptr,
         nullptr
    };

    uint8_t mcount = 0;
    vfs_inode_t* mount(vfs_inode_t *at){
        bool first = false;
        uint8_t DriverID;
        uint8_t PartitionID;
        uint8_t k,m;
        vfs_inode_t *ret =
            VFS::AllocInode(VFS_NODE_MOUNTPOINT, 0777, 0, &ext4, NULL);
        char* dev = at->mountpoint->path;
        if(dev == nullptr)
            return ret;
        
        for(uint32_t i = 0;dev[i] != '\0';i++){
            if(dev[i] == '/' && dev[i + 1] == 'd' && dev[i + 2] == 'e' &&
                dev[i + 3] == 'v' && dev[i + 4] == '/'){
                for(uint16_t j = i; dev[j] != '\0';j++){
                    k = j;
                    if(isdigit(dev[j]) && first == false){
                        first = true;
                        DriverID = dev[j] - '0';
                        m = j;
                    }else if(isdigit(dev[j]) && first == false){
                        PartitionID = dev[j] - '0';
                    }else{return ret;}
                }
            }else{return ret;}
        }
        char* p = (char*)kmalloc(m - k + 1);
        for(uint8_t l = k;dev[l] != '\0';l++)
            p[l] = dev[l];
        ext4_mount(p,StrCombine("/",(char*)mcount + '0'),false);
        kfree(p);
        mcount++;
    }


} // namespace EXT4_VFS
