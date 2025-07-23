#include <klib/klib.h>
#include <fs/vfs.h>
#include <mem/heap.h>


namespace VFS{
    vfs_inode_t *AllocInode(vfs_node_type_t type, uint32_t perms,
                             uint32_t uid, vfs_fsinfo_t * fs,
                             vfs_tnode_t * mountpoint)
    {
        vfs_inode_t *inode = (vfs_inode_t *) kmalloc(sizeof(vfs_inode_t));
        _memset(inode, 0, sizeof(vfs_inode_t));
        //*inode = {
        ///* .type =  */type,/* .perms = */ perms,/* .uid =  */uid,/* .fs = */ fs,/* .ident = */
        //        NULL,/* .mountpoint = */ mountpoint,/* .refcount = */ 0,/* .size =*/ 0};
        (*inode).type = type;(*inode).perms = perms;(*inode).uid = uid;(*inode).fs = fs;
        (*inode).ident = nullptr;(*inode).mountpoint = mountpoint;(*inode).refcount = 0;(*inode).size = 0;
        return inode;
    }

    void FreeNodes(vfs_tnode_t * tnode){
        vfs_inode_t *inode = tnode->inode;
        if (inode->refcount <= 0)
            kfree(inode);
        kfree(tnode);
    }
}