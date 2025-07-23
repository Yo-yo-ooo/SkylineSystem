#include <fs/lwext4/ext4.h>
#include <fs/vfs.h>

namespace EXT4_VFS
{
    extern vfs_fsinfo_t ext4;

    vfs_inode_t* mount(vfs_inode_t *at);
}