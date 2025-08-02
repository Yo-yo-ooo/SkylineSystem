#include <fs/lwext4/ext4.h>
#include <fs/vfs.h>

namespace EXT4_VFS
{
    extern vfs_fsinfo_t ext4;
    extern uint8_t mcount;
    extern uint8_t fcount;

    typedef struct ext4_ident_t{
        uint8_t CurMount;
        //uint8_t CurFileIdx; //NULL refer to empty file
        bool IsMount;
        ext4_file FileDesc;
    }ext4_ident_t;

    vfs_inode_t* Mount(vfs_inode_t *at);
    vfs_tnode_t Open(vfs_inode_t *this_inode, const char *path);
}