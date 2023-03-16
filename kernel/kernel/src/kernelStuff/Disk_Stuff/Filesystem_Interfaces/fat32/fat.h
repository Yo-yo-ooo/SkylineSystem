#pragma once
#include "../generic/genericFileSystemInterface.h"

namespace FilesystemInterface
{
    class Fat32FilesystemInterface : public GenericFilesystemInterface
    {
    public:
        struct Fat32FileSystemInterface{
            char name[12],ext[8];
            unsigned short time;
        }
    }

}