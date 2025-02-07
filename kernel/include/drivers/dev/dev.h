#pragma once

#ifndef _DEV_H
#define _DEV_H

#include <fs/vfs.h>
#include <klib/klib.h>

extern vfs_node* vfs_dev;

namespace Dev{
    void Init();
    void Add(vfs_node* dev);
}

#endif