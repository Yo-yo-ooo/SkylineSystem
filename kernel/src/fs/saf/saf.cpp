#include <fs/saf/saf.h>
#include <mem/pmm.h>
#include <klib/klib.h>

int32_t StrIndexOf(const char* str, char chr)
{
    for (int i = 0; str[i] != 0; i++)
        if (str[i] == chr)
            return i;
    return -1;
}

namespace SAF
{
    saf_node_hdr_t* initrd_find(const char* path, void* base, saf_node_hdr_t* current) 
    {
        //Serial::Writelnf("initrd_find(%s, %X, %X, %s)", path, base, current, current->name);

        while (*path == '/') 
            path++;
        
        if (*path == 0) 
            return current;

        char buffer[128] = { 0 };
        int index = StrIndexOf(path, '/');
        if (index == -1) 
            index = strlen(path);
        const char* next = path + index;//copy_until('/', path, buffer);
        _memcpy((char*)path, buffer, index);
        
        //assert(current->flags == FLAG_ISFOLDER);
        saf_node_folder_t* folder_node = (saf_node_folder_t*) current;

        for (int i = 0; i < folder_node->num_children; i++) {
            saf_node_hdr_t* child = (saf_node_hdr_t*) ((uint64_t) base + (uint64_t) folder_node->children[i]);
            if (strcmp(buffer, child->name)) 
            {
                return initrd_find(next, base, child);
            }
        }

        return NULL;
    }


    file_t* initrd_open(initrdMount* mount, const char* path, int flags) 
    {
        //Serial::Writelnf("open: %s", path);

        saf_node_hdr_t* file = initrd_find(path, mount->driver_specific_data, (saf_node_hdr_t*) mount->driver_specific_data);
        if (file == NULL) {
            Serial::TWritelnf("File %s not found", path);
            return NULL;
        }

        if (file->flags == FLAG_ISFOLDER) {
            Serial::TWritelnf("File %s is a folder", path);
            return NULL;
        }

        saf_node_file_t* file_node = (saf_node_file_t*) file;

        file_t* f = (file_t*)PMM::Request();
        f->mount = mount;
        f->size = file_node->size;
        f->driver_specific_data = (void*) ((uint64_t) mount->driver_specific_data + (uint64_t) file_node->addr);
        f->isFolder = file->flags == FLAG_ISFOLDER;

        return f;
    }

    file_t* LoadFileFromNode(initrdMount* mount, saf_node_file_t* file)
    {
        saf_node_file_t* file_node = file;

        file_t* f = (file_t*)PMM::Request();
        f->mount = mount;
        f->size = file_node->size;
        f->isFolder = ((saf_node_hdr_t*)file_node)->flags == FLAG_ISFOLDER;
        //f->name = file_node->hdr.name;
        _memcpy(file_node->hdr.name, f->name, strlen(file_node->hdr.name) + 1);
        f->driver_specific_data = (void*) ((uint64_t) mount->driver_specific_data + (uint64_t) file_node->addr);

        return f;
    }

    saf_node_hdr_t* GetFolderFromFileNode(initrdMount* mount, saf_node_file_t* file)
    {
        saf_node_hdr_t* folder = (saf_node_hdr_t*)file;

        return folder;
    }

    void initrd_close(initrdMount* mount, file_t* f) 
    {
        PMM::Free((void*)f);
    }

    void initrd_read(initrdMount* mount, file_t* f, void* buffer, size_t size, size_t offset) 
    {
        //assert(f->size >= offset + size);

        _memcpy((void*) ((uint64_t) f->driver_specific_data + offset), buffer, size);
    }

    dir_t initrd_dir_at(initrdMount* mount, int idx, const char* path)
    {
        // debugf("dir_at: %s (%d)", path, idx);
        char path_cpy[strlen(path) + 1];
        _memcpy((char*)path, path_cpy, strlen(path) + 1);
        path_cpy[strlen(path)] = 0;;

        saf_node_hdr_t* folder = initrd_find(path, mount->driver_specific_data, (saf_node_hdr_t*) mount->driver_specific_data);

        if (folder == NULL) {
            dir_t dir = {
                .is_none = true,
            };
            return dir;
        }

        if (folder->flags != FLAG_ISFOLDER) {
            dir_t dir = {
                .is_none = true,
            };
            return dir;
        }

        saf_node_folder_t* folder_node = (saf_node_folder_t*) folder;

        if (idx > folder_node->num_children - 1) {
            dir_t dir = {
                .is_none = true,
            };
            return dir;
        } else {
            saf_node_hdr_t* child = (saf_node_hdr_t*) ((uint64_t) mount->driver_specific_data + (uint64_t) folder_node->children[idx]);

            dir_t dir;
            _memset(&dir, 0, sizeof(dir));

            _memcpy(child->name, dir.name, strlen(child->name) + 1);
            dir.idx = idx;
            dir.is_none = false;

            if (child->flags == FLAG_ISFOLDER) {
                dir.type = ENTRY_DIR;
            } else {
                dir.type = ENTRY_FILE;
            }

            return dir;
        }
    }

    initrdMount* initrd_mount(void* saf_image) 
    {
        initrdMount* mount = (initrdMount*) PMM::Request();
        _memset(mount, 0, sizeof(initrdMount));

        mount->driver_specific_data = saf_image;

        return mount;
    }
}