#include <fs/lwext4/ext4.h>
#include <fs/vfs.h>

namespace EXT4_VFS
{
    size_t read(vnode_t *node, uint8_t *buffer, size_t off, size_t len);
    size_t write(struct vnode_t *Node, uint8_t *buffer, size_t off, size_t len);
    vnode_t *lookup(vnode_t *node, const char *name);
    void populate(vnode_t *node);

    void Init(const char* DEVN,const char* MPN,u8 wpart = 0);
}