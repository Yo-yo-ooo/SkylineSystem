#include <klib/klib.h>
#include <fs/vfs.h>
#include <mem/heap.h>
#include <klib/errno.h>

namespace VFS{
    vnode_t *root_node = NULL;

   void Init() {
        root_node = (vnode_t*)kmalloc(sizeof(vnode_t));
        _memset(root_node, 0, sizeof(vnode_t));
        root_node->Name[0] = '/';
        //int err = ext2_mount(root_node, ahci_ports[0]); // TODO: Actually find a valid EXT2 port.
        /* if (!err)
            kpok("Mounted disk0 to /.\n");
        else
            kerror("Something went wrong when mounting disk0 to /.\n"); */
        //devfs_init();
        kpok("Mounted dev to /dev/.\n");
    }

    void AddNode(vnode_t *Parent, vnode_t *Child) {
        Child->Parent = Parent;
        if (!Parent->Child) {
            Parent->Child = Child;
            return;
        }
        vnode_t *it = Parent->Child;
        while (it->Next)
            it = it->Next;
        it->Next = Child;
        return;
    }

    vnode_t *Open(vnode_t *Root,const char* Path) {
        vnode_t *current = Root;
        if (Path[0] == '/')
            current = root_node;
        char *new_path = kmalloc(strlen(Path) + 1);
        __memcpy(new_path, Path, strlen(Path) + 1);
        char *token = strtok(new_path + (Path[0] == '/' ? 1 : 0), "/");
        while (token != NULL) {
            if (!strcmp(token, ".")) {
                token = strtok(NULL, "/");
                continue;
            } else if (!strcmp(token, "..")) {
                current = current->Parent;
                token = strtok(NULL, "/");
                continue;
            }
            current = VFS::lookup(current, token);
            if (!current) {
                kfree(new_path);
                return NULL;
            }
            token = strtok(NULL, "/");
        }
        current->RefCount++;
        kfree(new_path);
        return current;
    }

    void Close(vnode_t *Node) {
        Node->RefCount--;
        if (Node->RefCount == 0) {
            kfree(Node->Mutex->queue);
            kfree(Node->Mutex);
            kfree(Node);
        }
    }

    size_t Read(vnode_t *Node, uint8_t *Buffer, size_t Offset, size_t Length) {
        if (Node->read) {
            size_t res = Node->read(Node, Buffer, Offset, Length);
            return res;
        }
        return 0;
    }

    size_t Write(vnode_t *Node, uint8_t *Buffer, size_t Offset, size_t Length) {
        if (Node->write) {
            size_t res = Node->write(Node, Buffer, Offset, Length);
            return res;
        }
        return 0;
    }

    vnode_t *lookup(vnode_t *Node, const char *name) {
        if (Node->lookup)
            return Node->lookup(Node, name);
        return NULL;
    }

    int32_t ioctl(vnode_t *Node, uint64_t Request, void *Arg) {
        if (Node->ioctl)
            return Node->ioctl(Node, Request, Arg);
        return -ENOTTY;
    }

    int32_t poll(vnode_t *Node, int32_t Events) {
        if (Node->poll)
            return Node->poll(Node, Events);
        return 0;
    }

    void populate(vnode_t *Node) {
        if (Node->populate)
            Node->populate(Node);
    }
}