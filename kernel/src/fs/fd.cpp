/*
* SPDX-License-Identifier: GPL-2.0-only
* File: fd.cpp
* Copyright (C) 2026 Yo-yo-ooo
*
* This file is part of SkylineSystem.
*
* SkylineSystem is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
*/
#include <fs/fd.h>
#include <fs/lwext4/ext4.h>

int32_t Ext4Write(fd_t* fsd, const void *buf, size_t count){
    ext4_file f = fsd->f;
    return ext4_fwrite(&f,buf,count,0);
}

int32_t Ext4Read(fd_t* fsd, void *buf, size_t count){
    ext4_file f = fsd->f;
    return ext4_fread(&f,buf,count,0);
}

int32_t Ext4Lseek(fd_t* fsd, int32_t offset, int32_t whence){
    ext4_file f = fsd->f;
    return ext4_fseek(&f,offset,whence);
}

int32_t Ext4Close(fd_t* fsd){
    ext4_file f = fsd->f;
    return ext4_fclose(&f);
}

int32_t Ext4Open(fd_t* fsd, const char* path, int32_t flags){
    ext4_file f = fsd->f;
    return ext4_fopen(&f,path,flags);
}

volatile struct FSOps FileSystemOps[256] = {
    {nullptr,nullptr,nullptr,nullptr,nullptr}, //0:Unused
    {Ext4Write,Ext4Read,Ext4Lseek,Ext4Close,Ext4Open}, //FS_EXT4
};

int32_t ReadFile(fd_t* fsd, void *buf, size_t count){
    if(fsd->IsSpecial == false){
        FileSystemOps[fsd->FsType].read(
        fsd,
        buf,
        count);
    }else{
        if(Dev::DevList_[fsd->dev_idx].classp){
            Dev::DevList_[fsd->dev_idx].ops.ReadBytes(
                Dev::DevList_[fsd->dev_idx].classp,
                fsd->off,
                count,
                buf
            );
        }else{
            Dev::DevList_[fsd->dev_idx].ops.ReadBytes_(
                fsd->off,
                count,
                buf
            );
        }
    }
}
int32_t WriteFile(fd_t* fsd, const void *buf, size_t count){
    if(fsd->IsSpecial == false){
        FileSystemOps[fsd->FsType].write(
        fsd,
        buf,
        count);
    }else{
        if(Dev::DevList_[fsd->dev_idx].classp){
            Dev::DevList_[fsd->dev_idx].ops.WriteBytes(
                Dev::DevList_[fsd->dev_idx].classp,
                fsd->off,
                count,
                buf
            );
        }else{
            Dev::DevList_[fsd->dev_idx].ops.WriteBytes_(
                fsd->off,
                count,
                buf
            );
        }
    }
}
int32_t LseekFile(fd_t* fsd, size_t offset, int32_t whence){
    switch(whence){
            case SEEK_SET:
                fsd->off = offset;
                break;
            case SEEK_CUR:
                fsd->off += offset;
                break;
            case SEEK_END:
                fsd->off = fsd->size + offset;
                break;
            default:
                return -1;
    }
    if(fsd->IsSpecial == false){
        FileSystemOps[fsd->FsType].lseek(
        fsd,
        offset,
        whence);
    }
}
int32_t CloseFile(fd_t* fsd){
    if(fsd->IsSpecial == false){
        FileSystemOps[fsd->FsType].close(
        fsd);
    }
}
int32_t OpenFile(fd_t* fsd, const char* path, int32_t flags){
    
}