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

#ifndef BLOCKDEV_H_
#define BLOCKDEV_H_

#include "../include/ext4.h"
#include "../include/ext4_balloc.h"
#include "../include/ext4_bcache.h"
#include "../include/ext4_bitmap.h"
#include "../include/ext4_block_group.h"
#include "../include/ext4_blockdev.h"
#include "../include/ext4_config.h"
#include "../include/ext4_crc32.h"
#include "../include/ext4_debug.h"
#include "../include/ext4_dir.h"
#include "../include/ext4_dir_idx.h"
#include "../include/ext4_errno.h"
#include "../include/ext4_extent.h"
#include "../include/ext4_fs.h"
#include "../include/ext4_hash.h"
#include "../include/ext4_ialloc.h"
#include "../include/ext4_inode.h"
#include "../include/ext4_journal.h"
#include "../include/ext4_mbr.h"
#include "../include/ext4_misc.h"
#include "../include/ext4_mkfs.h"
#include "../include/ext4_oflags.h"
#include "../include/ext4_super.h"
#include "../include/ext4_trans.h"
#include "../include/ext4_types.h"
#include "../include/ext4_xattr.h"

#include <stdint.h>
#include <stdbool.h>
#include "../../../drivers/vsdev/vsdev.h"
int blockdev_open(struct ext4_blockdev *bdev);
int blockdev_bread(struct ext4_blockdev *bdev, void *buf, uint64_t blk_id,
			 uint32_t blk_cnt);
int blockdev_bwrite(struct ext4_blockdev *bdev, const void *buf,
			  uint64_t blk_id, uint32_t blk_cnt);
int blockdev_close(struct ext4_blockdev *bdev);
int blockdev_lock(struct ext4_blockdev *bdev);
int blockdev_unlock(struct ext4_blockdev *bdev);

extern VsDevInfo ThisInfo; // Do Not Use It!!!!

/**@brief   File blockdev get.*/
struct ext4_blockdev *ext4_blockdev_get(u32 which);

#endif /* BLOCKDEV_H_ */
