#include "dev.h"

vfs_node *vfs_dev;

vfs_node *devices[256];
u32 dev_index = 0;
;
namespace Dev{
    vfs_dirent *ReadDir(vfs_node *vnode, u32 index)
    {
        (void)vnode;
        if (devices[index] == NULL)
            return NULL;

        vfs_dirent *dirent = (vfs_dirent *)kmalloc(sizeof(vfs_dirent));
        int len = strlen(devices[index]->name);
        dirent->name = (char *)kmalloc(len);
        __memcpy(dirent->name, devices[index]->name, len);
        dirent->ino = devices[index]->ino;

        return dirent;
    }

    vfs_node *FindDir(vfs_node *vnode, char *path)
    {
        (void)vnode;
        for (u32 i = 0; i < dev_index; i++)
            if (!strcmp(devices[i]->name, path))
                return devices[i];
        return NULL;
    }

    void Add(vfs_node *vnode)
    {
        devices[dev_index] = vnode;
        dev_index++;
    }

    void Init()
    {
        vfs_dev = (vfs_node *)kmalloc(sizeof(vfs_node));
        vfs_dev->parent = vfs_root;
        vfs_dev->open = true;
        vfs_dev->name = (char *)kmalloc(4);
        __memcpy(vfs_dev->name, "dev", 4);
        vfs_dev->ino = 0;
        vfs_dev->write = 0;
        vfs_dev->read = 0;
        vfs_dev->readdir = Dev::ReadDir;
        vfs_dev->finddir = Dev::FindDir;
        vfs_dev->size = 1;
        vfs_dev->type = VFS_DIRECTORY;
    }
}