/*
 * Copyright (c) 2015 Grzegorz Kostka (kostka.grzegorz@gmail.com)
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * - Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in the
 *   documentation and/or other materials provided with the distribution.
 * - The name of the author may not be used to endorse or promote products
 *   derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */


#include <fs/lwext4/ext4.h>
#include <fs/lwext4/ext4_balloc.h>
#include <fs/lwext4/ext4_bcache.h>
#include <fs/lwext4/ext4_bitmap.h>
#include <fs/lwext4/ext4_block_group.h>
#include <fs/lwext4/ext4_blockdev.h>
#include <fs/lwext4/ext4_config.h>
#include <fs/lwext4/ext4_crc32.h>
#include <fs/lwext4/ext4_debug.h>
#include <fs/lwext4/ext4_dir.h>
#include <fs/lwext4/ext4_dir_idx.h>
#include <fs/lwext4/ext4_errno.h>
#include <fs/lwext4/ext4_extent.h>
#include <fs/lwext4/ext4_fs.h>
#include <fs/lwext4/ext4_hash.h>
#include <fs/lwext4/ext4_ialloc.h>
#include <fs/lwext4/ext4_inode.h>
#include <fs/lwext4/ext4_journal.h>
#include <fs/lwext4/ext4_mbr.h>
#include <fs/lwext4/ext4_misc.h>
#include <fs/lwext4/ext4_mkfs.h>
#include <fs/lwext4/ext4_oflags.h>
#include <fs/lwext4/ext4_super.h>
#include <fs/lwext4/ext4_trans.h>
#include <fs/lwext4/ext4_types.h>
#include <fs/lwext4/ext4_xattr.h>

#include <klib/cstr.h>
#include <klib/klib.h>
#include <mem/heap.h>

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <fs/lwext4/blockdev/blockdev.h>

/*
In SkylineSystem We use dynamic device change, 
so we need to get the device info
from the VsDev "namespace". 
*/


/**********************BLOCKDEV INTERFACE**************************************/

VsDevInfo ThisInfo;

/******************************************************************************/
static uint8_t blockdev_ph_bbuf[(512)]; 
static struct ext4_blockdev_iface blockdev_iface = { 
    .open = blockdev_open, 
    .bread = blockdev_bread, 
    .bwrite = blockdev_bwrite, 
    .close = blockdev_close, 
    .lock = blockdev_lock, 
    .unlock = blockdev_unlock, 
    .ph_bsize = 512, 
    .ph_bcnt = 0, 
    .ph_bbuf = blockdev_ph_bbuf, }; 
static struct ext4_blockdev blockdev = { 
    .bdif = &blockdev_iface, 
    .part_offset = 0, .part_size = (0) * (512), };
/******************************************************************************/
int blockdev_open(struct ext4_blockdev *bdev)
{
	/*blockdev_open: skeleton*/
	return EIO;
}

/******************************************************************************/

int blockdev_bread(struct ext4_blockdev *bdev, void *buf, uint64_t blk_id,
			 uint32_t blk_cnt)
{
	/*blockdev_bread: skeleton*/
    if(ThisInfo.ops->Read(bdev->lg_bcnt, blk_cnt, buf) == VsDev::RW_OK)
        return EOK;
    else
        return EIO;
	return EIO;
}


/******************************************************************************/
int blockdev_bwrite(struct ext4_blockdev *bdev, const void *buf,
			  uint64_t blk_id, uint32_t blk_cnt)
{
	/*blockdev_bwrite: skeleton*/
    if(ThisInfo.ops->Write(bdev->lg_bcnt, blk_cnt, buf) == VsDev::RW_OK)
        return EOK;
    else
        return EIO;
	return EIO;
}
/******************************************************************************/
int blockdev_close(struct ext4_blockdev *bdev)
{
	/*blockdev_close: skeleton*/
	return EIO;
}

int blockdev_lock(struct ext4_blockdev *bdev)
{
	/*blockdev_lock: skeleton*/
    lock(bdev->bdif->p);
    
	return EIO;
}

int blockdev_unlock(struct ext4_blockdev *bdev)
{
	/*blockdev_unlock: skeleton*/
    unlock(bdev->bdif->p);

	return EIO;
}

/******************************************************************************/
struct ext4_blockdev *ext4_blockdev_get(u32 which)
{
    ThisInfo = VsDev::GetSDEV(which);
	return &blockdev;
}
/******************************************************************************/


