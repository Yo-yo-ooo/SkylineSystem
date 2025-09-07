#include <fs/lwext4/ctvfs.h>
#include <fs/lwext4/blockdev/blockdev.h>

static vnode_t *EXT4_VFS_CreateVnode(ext4_file *f){
    vnode_t *node = (vnode_t*)kmalloc(sizeof(vnode_t));
    _memset(node, 0, sizeof(vnode_t));
    node->read = EXT4_VFS::read;
    node->lookup = EXT4_VFS::lookup;
    node->populate = EXT4_VFS::populate;
    node->mnt_info = f;
    node->Mutex = Mutex::Create();
    return node;
}
namespace EXT4_VFS
{
    size_t read(vnode_t *node, uint8_t *buffer, size_t off, size_t len) {
        ext4_file f = *(ext4_file*)node->mnt_info;
        ext4_fopen(&f,(const char*)&f.PATH_ADDR,O_RDONLY);
        if(ext4_fseek(&f,off,SEEK_SET) == EINVAL){return EINVAL;}
        return ext4_fread(&f,buffer,len,NULL);
    }
    
    size_t write(struct vnode_t *Node, uint8_t *buffer, size_t off, size_t len){
        ext4_file f = *(ext4_file*)Node->mnt_info;
        ext4_fopen(&f,(const char*)&f.PATH_ADDR,O_RDONLY);
        if(ext4_fseek(&f,off,SEEK_SET) == EINVAL){return EINVAL;}
        return ext4_fwrite(&f,buffer,len,NULL);
    }

    vnode_t *lookup(vnode_t *node, const char *name) {
        vnode_t *current = node->Child;
        while (current) {
            if (!strcmp(current->Name, name))
                break;
            current = current->Next;
            if (!current)
                return NULL;
        }
        if (current->Type == 2 && !current->Child)
            VFS::populate(current);
        return current;
    }


    void populate(vnode_t *node){
        ext4_dir d;
        ext4_direntry *de;
        const char* path = (const char*)(*(ext4_file*)node->mnt_info).PATH_ADDR;

        ext4_dir_open(&d, path);
        de = ext4_dir_entry_next(&d);

        vnode_t *current = EXT4_VFS_CreateVnode(&d.f);
        node->Child = current;

        while (de) {
            __memcpy(current->Name, de->name, de->name_length);
            current->Inode = de->inode;
            current->Type = de->inode_type;
            current->Parent = node;
            de = ext4_dir_entry_next(&d);
            current->Size = d.f.fsize;
            vnode_t *next = EXT4_VFS_CreateVnode(&d.f);
            current->Next = next;
            current = next;
        }
        ext4_dir_close(&d);
    }

    void Init(const char* DEVN,const char* MPN,u8 wpart = 0){
        uint32_t r = ext4_device_register(ext4_blockdev_get(DEVN,wpart), DEVN);
        if (r != EOK) {
            kerror("ext4_device_register: rc = %d\n", r);
        }

        r = ext4_mount(DEVN, MPN, false);
        if (r != EOK){kerror("ext4_mount: rc = %d\n", r);hcf();}
        kpok("EXT4 MOUNTED!\n");
        

        r = ext4_recover(MPN);
        if (r != EOK && r != ENOTSUP) {
            kerror("ext4_recover: rc = %d\n", r);
            return false;
        }
        kpok("ext4_recover !\n");

        r = ext4_journal_start(MPN);
        if (r != EOK) {
            kerror("ext4_journal_start: rc = %d\n", r);
            return false;
        }
        kpok("ext4_journal_start !\n");

        ext4_cache_write_back(MPN, 1);

        ext4_file f;
        ext4_fopen(&f,MPN,O_RDONLY);
        VFS::root_node->mnt_info = &f;
        EXT4_VFS::populate(VFS::root_node);
        VFS::root_node->read = EXT4_VFS::read;
        VFS::root_node->write = EXT4_VFS::write;
        VFS::root_node->lookup = EXT4_VFS::lookup;
        VFS::root_node->populate = EXT4_VFS::populate;
    }
} // namespace EXT4_VFS
