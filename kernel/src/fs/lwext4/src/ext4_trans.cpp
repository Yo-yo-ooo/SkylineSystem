/*
 * Copyright (c) 2015 Grzegorz Kostka (kostka.grzegorz@gmail.com)
 * Copyright (c) 2015 Kaho Ng (ngkaho1234@gmail.com)
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

/** @addtogroup lwext4
 * @{
 */
/**
 * @file  ext4_trans.c
 * @brief Ext4 transaction buffer operations.
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

int32_t ext4_trans_set_block_dirty(struct ext4_buf *buf)
{
	int32_t r = EOK;
#if CONFIG_JOURNALING_ENABLE
	struct ext4_fs *fs = buf->bc->bdev->fs;
	struct ext4_block block;
    block.lb_id = buf->lba;
    block.data = buf->data;
    block.buf = buf;

	if (fs->jbd_journal && fs->curr_trans) {
		struct jbd_trans *trans = fs->curr_trans;
		return jbd_trans_set_block_dirty(trans, &block);
	}
#endif
	ext4_bcache_set_dirty(buf);
	return r;
}

int32_t ext4_trans_block_get_noread(struct ext4_blockdev *bdev,
			  struct ext4_block *b,
			  uint64_t lba)
{
	int32_t r = ext4_block_get_noread(bdev, b, lba);
	if (r != EOK)
		return r;

	return r;
}

int32_t ext4_trans_block_get(struct ext4_blockdev *bdev,
		   struct ext4_block *b,
		   uint64_t lba)
{
	int32_t r = ext4_block_get(bdev, b, lba);
	if (r != EOK)
		return r;

	return r;
}

int32_t ext4_trans_try_revoke_block(struct ext4_blockdev *bdev __unused,
			        uint64_t lba __unused)
{
	int32_t r = EOK;
#if CONFIG_JOURNALING_ENABLE
	struct ext4_fs *fs = bdev->fs;
	if (fs->jbd_journal && fs->curr_trans) {
		struct jbd_trans *trans = fs->curr_trans;
		r = jbd_trans_try_revoke_block(trans, lba);
	} else if (fs->jbd_journal) {
		r = ext4_block_flush_lba(fs->bdev, lba);
	}
#endif
	return r;
}

/**
 * @}
 */
