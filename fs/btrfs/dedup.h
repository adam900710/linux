/*
 * Copyright (C) 2015 Fujitsu.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License v2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 021110-1307, USA.
 */

#ifndef __BTRFS_DEDUP__
#define __BTRFS_DEDUP__

#include <linux/btrfs.h>
#include <crypto/hash.h>

/*
 * Dedup storage backend
 * On disk is persist storage but overhead is large
 * In memory is fast but will lose all its hash on umount
 */
#define BTRFS_DEDUP_BACKEND_INMEMORY		0
#define BTRFS_DEDUP_BACKEND_ONDISK		1
#define BTRFS_DEDUP_BACKEND_LAST		2

/* Dedup block size limit and default value */
#define BTRFS_DEDUP_BLOCKSIZE_MAX	(8 * 1024 * 1024)
#define BTRFS_DEDUP_BLOCKSIZE_MIN	(16 * 1024)
#define BTRFS_DEDUP_BLOCKSIZE_DEFAULT	(32 * 1024)

/* Hash algorithm, only support SHA256 yet */
#define BTRFS_DEDUP_HASH_SHA256		0

static int btrfs_dedup_sizes[] = { 32 };

/*
 * For caller outside of dedup.c
 *
 * Different dedup backends should have their own hash structure
 */
struct btrfs_dedup_hash {
	u64 bytenr;
	u32 num_bytes;

	/* last field is a variable length array of dedup hash */
	u8 hash[];
};

struct btrfs_root;

struct btrfs_dedup_info {
	/* dedup blocksize */
	u64 blocksize;
	u16 backend;
	u16 hash_type;

	struct crypto_shash *dedup_driver;
	struct mutex lock;

	/* following members are only used in in-memory dedup mode */
	struct rb_root hash_root;
	struct rb_root bytenr_root;
	struct list_head lru_list;
	u64 limit_nr;
	u64 current_nr;

	/* for persist data like dedup-hash and dedup status */
	struct btrfs_root *dedup_root;
};

struct btrfs_trans_handle;

static inline int btrfs_dedup_hash_size(u16 type)
{
	if (WARN_ON(type >= ARRAY_SIZE(btrfs_dedup_sizes)))
		return -EINVAL;
	return sizeof(struct btrfs_dedup_hash) + btrfs_dedup_sizes[type];
}

static inline struct btrfs_dedup_hash *btrfs_dedup_alloc_hash(u16 type)
{
	return kzalloc(btrfs_dedup_hash_size(type), GFP_NOFS);
}


/*
 * Initial inband dedup info
 * Called at dedup enable time.
 */
int btrfs_dedup_enable(struct btrfs_fs_info *fs_info, u16 type, u16 backend,
		       u64 blocksize, u64 limit);

/*
 * Disable dedup and invalidate all its dedup data.
 * Called at dedup disable time.
 */
int btrfs_dedup_disable(struct btrfs_fs_info *fs_info);

/*
 * Restore previous dedup setup from disk
 * Called at mount time
 */
int btrfs_dedup_resume(struct btrfs_fs_info *fs_info,
		       struct btrfs_root *dedup_root);

/*
 * Free current btrfs_dedup_info
 * Called at umount(close_ctree) time
 */
int btrfs_dedup_cleanup(struct btrfs_fs_info *fs_info);

/*
 * Calculate hash for dedup.
 * Caller must ensure [start, start + dedup_bs) has valid data.
 */
int btrfs_dedup_calc_hash(struct btrfs_root *root, struct inode *inode,
			  u64 start, struct btrfs_dedup_hash *hash);

/*
 * Calculate hash for cbs (hash of the complete file). 
 */
int btrfs_cbs_calc_hash(struct btrfs_root *root, struct inode *inode,
			  u64 start, u64 end, struct btrfs_dedup_hash *hash);

/*
 * Search for duplicated extents by calculated hash
 * Caller must call btrfs_dedup_calc_hash() first to get the hash.
 *
 * @inode: the inode for we are writing
 * @file_pos: offset inside the inode
 * As we will increase extent ref immediately after a hash match,
 * we need @file_pos and @inode in this case.
 *
 * Return > 0 for a hash match, and the extent ref will be
 * *INCREASED*, and hash->bytenr/num_bytes will record the existing
 * extent data.
 * Return 0 for a hash miss. Nothing is done
 * Return <0 for error.
 *
 * Only on-disk backedn may return error though.
 */
int btrfs_dedup_search(struct inode *inode, u64 file_pos,
		       struct btrfs_dedup_hash *hash);

/* Add a dedup hash into dedup info */
int btrfs_dedup_add(struct btrfs_trans_handle *trans, struct btrfs_root *root,
		    struct btrfs_dedup_hash *hash);

/* Remove a dedup hash from dedup info */
int btrfs_dedup_del(struct btrfs_trans_handle *trans, struct btrfs_root *root,
		    u64 bytenr);
#endif
