#include "../klib/klib.h"
#include "vfs.h"
#include "../mem/heap.h"

vfs_node *vfs_root;

void vfs_init()
{
    vfs_root = (vfs_node *)kmalloc(sizeof(vfs_node));
    vfs_root->parent = NULL;
    vfs_root->open = true;
    vfs_root->name = kmalloc(2);
    vfs_root->name[0] = '/';
    vfs_root->name[1] = '\0';
    vfs_root->ino = 2;
    vfs_root->read = NULL;
    vfs_root->readdir = nullptr;
    vfs_root->finddir = nullptr;
    vfs_root->size = NULL;
    vfs_root->type = VFS_DIRECTORY;
}

i32 vfs_write(vfs_node *vnode, u8 *buffer, u32 count)
{
    if (!vnode)
        return -1;
    if (vnode->write)
        return vnode->write(vnode, buffer, count);
    return -1;
}

i32 vfs_read(vfs_node *vnode, u8 *buffer, u32 count)
{
    if (!vnode)
        return -1;
    if (vnode->read)
        return vnode->read(vnode, buffer, count);
    return -1;
}

vfs_dirent *vfs_readdir(vfs_node *vnode, u32 index)
{
    if (!vnode)
        return NULL;
    if (vnode->readdir && vnode->type == VFS_DIRECTORY)
        return vnode->readdir(vnode, index);
    return NULL;
}

vfs_node *vfs_finddir(vfs_node *vnode, char *path)
{
    if (!vnode)
        return NULL;
    if (vnode->finddir && vnode->type == VFS_DIRECTORY)
        return vnode->finddir(vnode, path);
    return NULL;
}

vfs_node *vfs_open(vfs_node *vnode, char *path)
{
    if (path[0] == '.')
    {
        if (path[1] == '.')
        {
            return vnode->parent;
        }
        return vnode;
    }
    bool root = (path[0] == '/');
    int plen = strlen(path);

    bool has_subdir = false;
    for (int i = (root ? 1 : 0); i < plen; i++)
        if (path[i] == '/')
        {
            has_subdir = true;
            break; // it has subdirs
        }

    if (!has_subdir)
        return vfs_finddir((root ? vfs_root : vnode), path + (root ? 1 : 0));

    char *_path = (char *)kmalloc(plen);
    __memcpy(_path, (root ? path + 1 : path), plen);

    char *token = strtok(_path, "/");
    vfs_node *current = (root ? vfs_root : vnode);

    while (token)
    {
        current = vfs_finddir(current, token);
        if (current == NULL)
        {
            kfree(_path);
            return NULL;
        }

        token = strtok(NULL, "/");
    }
    kfree(_path);

    return current;
}

u64 vfs_poll(vfs_node *vnode)
{
    return vnode->poll(vnode);
}

void vfs_destroy(vfs_node *vnode)
{
    if (!(vnode->perms & VFS_DESTROY) || vnode->open)
        return;

    kfree(vnode->name);
    kfree(vnode);
}