#include <fs/lwext4/ctvfs.h>
#include <fs/lwext4/blockdev/blockdev.h>
namespace EXT4_VFS
{

    vfs_fsinfo_t ext4 = {
        "ext4",
        EXT4_VFS::Mount,
        EXT4_VFS::Open,
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
    uint8_t fcount = 0;

    vfs_inode_t* Mount(vfs_inode_t *at){
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
                for(uint16_t j = i + 5; dev[j] != '\0';j++){
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
        int32_t r;
        r = ext4_device_register(ext4_blockdev_get(DriverID,PartitionID),p);
        if (r != EOK) {return ret;}
        char *mpname = StrCombine("/",(char*)(mcount + '0'));
        r = ext4_mount(p,mpname,false);
        if (r != EOK) {return ret;}
        r = ext4_recover(mpname);
        if (r != EOK && r != ENOTSUP) {return ret;}
        r = ext4_journal_start(mpname);
        if (r != EOK) {return ret;}
        ext4_cache_write_back(mpname, 1);

        ext4_ident_t *ident = nullptr;
        ident->CurMount = mcount;
        ident->IsMount = true;
        ident->FileDesc = {0};

        kfree(p);
        kfree(mpname);
        mcount++;
        //ret->mountpoint->path = at->mountpoint->path;

        return ret;
    }

    vfs_inode_t __ofile(const char *path){
        uint32_t res = 0;
        //res = ext4_fopen(&ident->FileDesc,path,"wb+");
        if(res != EOK){return (vfs_inode_t){0};}
    }

    vfs_tnode_t Open(vfs_inode_t *this_inode, const char *path){
        vfs_tnode_t tnode;
        /*
        tncount : tree node counter
        ticount : tree inode counter
        */
        uint32_t tncount = 0, ticount = 0;
        int32_t res = 0;
        ext4_ident_t *ident = (ext4_ident_t*)this_inode->ident;
        ext4_dir d;
        ext4_direntry *de;

        ext4_dir_open(&d, path);
        de = ext4_dir_entry_next(&d);
        while (de) {
            if((!strcmp(de->name,".")) && (!strcmp(de->name,".."))){
                de = ext4_dir_entry_next(&d);
                continue;
            }else{

            }
            de = ext4_dir_entry_next(&d);
        }
        ext4_dir_close(&d);

        
        //tnode.path = path;
    }

} // namespace EXT4_VFS
