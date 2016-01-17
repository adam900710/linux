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
#include "ctree.h"
#include "dedup.h"
#include "btrfs_inode.h"
#include "transaction.h"
#include "delayed-ref.h"
#include "disk-io.h"

static int init_dedup_info(struct btrfs_fs_info *fs_info, u16 type, u16 backend,
			   u64 blocksize, u64 limit)
{
	struct btrfs_dedup_info *dedup_info;
	int ret;

	fs_info->dedup_info = kzalloc(sizeof(*dedup_info), GFP_NOFS);
	if (!fs_info->dedup_info)
		return -ENOMEM;

	dedup_info = fs_info->dedup_info;

	dedup_info->hash_type = type;
	dedup_info->backend = backend;
	dedup_info->blocksize = blocksize;
	dedup_info->limit_nr = limit;

	/* Only support SHA256 yet */
	dedup_info->dedup_driver = crypto_alloc_shash("sha256", 0, 0);
	if (IS_ERR(dedup_info->dedup_driver)) {
		btrfs_err(fs_info, "failed to init sha256 driver");
		ret = PTR_ERR(dedup_info->dedup_driver);
		kfree(fs_info->dedup_info);
		fs_info->dedup_info = NULL;
		return ret;
	}

	dedup_info->hash_root = RB_ROOT;
	dedup_info->bytenr_root = RB_ROOT;
	dedup_info->current_nr = 0;
	INIT_LIST_HEAD(&dedup_info->lru_list);
	mutex_init(&dedup_info->lock);
	return 0;
}

struct inmem_hash {
	struct rb_node hash_node;
	struct rb_node bytenr_node;
	struct list_head lru_list;

	u64 bytenr;
	u32 num_bytes;

	u8 hash[];
};

static inline struct inmem_hash *inmem_alloc_hash(u16 type)
{
	if (WARN_ON(type >= ARRAY_SIZE(btrfs_dedup_sizes)))
		return NULL;
	return kzalloc(sizeof(struct inmem_hash) + btrfs_dedup_sizes[type],
			GFP_NOFS);
}

int btrfs_dedup_enable(struct btrfs_fs_info *fs_info, u16 type, u16 backend,
		       u64 blocksize, u64 limit)
{
	printk(KERN_ERR " ##### In %s ##### \n", __func__);
	struct btrfs_dedup_info *dedup_info;
	struct btrfs_root *dedup_root;
	struct btrfs_key key;
	struct btrfs_trans_handle *trans;
	struct btrfs_path *path;
	struct btrfs_dedup_status_item *status;
	int create_tree;
	u64 compat_ro_flag = btrfs_super_compat_ro_flags(fs_info->super_copy);
	int ret = 0;

	/* Sanity check */
	if (blocksize > BTRFS_DEDUP_BLOCKSIZE_MAX ||
	    blocksize < BTRFS_DEDUP_BLOCKSIZE_MIN ||
	    blocksize < fs_info->tree_root->sectorsize ||
	    !is_power_of_2(blocksize))
		return -EINVAL;
	if (type > ARRAY_SIZE(btrfs_dedup_sizes))
		return -EINVAL;
	if (backend >= BTRFS_DEDUP_BACKEND_LAST)
		return -EINVAL;
	if (backend == BTRFS_DEDUP_BACKEND_INMEMORY && limit == 0)
		limit = 4096; /* default value */
	if (backend == BTRFS_DEDUP_BACKEND_ONDISK && limit != 0)
		limit = 0;

	/*
	 * If current fs doesn't support DEDUP feature, don't enable
	 * on-disk dedup.
	 */
	if (!(compat_ro_flag & BTRFS_FEATURE_COMPAT_RO_DEDUP) &&
	    backend == BTRFS_DEDUP_BACKEND_ONDISK)
		return -EINVAL;

	/* Meaningless and unable to enable dedup for RO fs */
	if (fs_info->sb->s_flags & MS_RDONLY)
		return -EINVAL;

	if (fs_info->dedup_info) {
		dedup_info = fs_info->dedup_info;

		/* Check if we are re-enable for different dedup config */
		if (dedup_info->blocksize != blocksize ||
		    dedup_info->hash_type != type ||
		    dedup_info->backend != backend) {
			btrfs_dedup_disable(fs_info);
			goto enable;
		}

		/* On-fly limit change is OK */
		mutex_lock(&dedup_info->lock);
		fs_info->dedup_info->limit_nr = limit;
		mutex_unlock(&dedup_info->lock);
		return 0;
	}

enable:
	create_tree = compat_ro_flag & BTRFS_FEATURE_COMPAT_RO_DEDUP;

	ret = init_dedup_info(fs_info, type, backend, blocksize, limit);
	dedup_info = fs_info->dedup_info;
	if (ret < 0)
		goto out;

	if (!create_tree)
		goto out;

	/* Create dedup tree for status at least */
	path = btrfs_alloc_path();
	if (!path) {
		ret = -ENOMEM;
		goto out;
	}

	trans = btrfs_start_transaction(fs_info->tree_root, 2);
	if (IS_ERR(trans)) {
		ret = PTR_ERR(trans);
		btrfs_free_path(path);
		goto out;
	}

	dedup_root = btrfs_create_tree(trans, fs_info,
				       BTRFS_DEDUP_TREE_OBJECTID);
	if (IS_ERR(dedup_root)) {
		ret = PTR_ERR(dedup_root);
		btrfs_abort_transaction(trans, fs_info->tree_root, ret);
		btrfs_free_path(path);
		goto out;
	}

	dedup_info->dedup_root = dedup_root;

	key.objectid = 0;
	key.type = BTRFS_DEDUP_STATUS_ITEM_KEY;
	key.offset = 0;

	ret = btrfs_insert_empty_item(trans, dedup_root, path, &key,
				      sizeof(*status));
	if (ret < 0) {
		btrfs_abort_transaction(trans, fs_info->tree_root, ret);
		btrfs_free_path(path);
		goto out;
	}
	status = btrfs_item_ptr(path->nodes[0], path->slots[0],
				struct btrfs_dedup_status_item);
	btrfs_set_dedup_status_blocksize(path->nodes[0], status, blocksize);
	btrfs_set_dedup_status_limit(path->nodes[0], status, limit);
	btrfs_set_dedup_status_hash_type(path->nodes[0], status, type);
	btrfs_set_dedup_status_backend(path->nodes[0], status, backend);
	btrfs_mark_buffer_dirty(path->nodes[0]);

	btrfs_free_path(path);
	ret = btrfs_commit_transaction(trans, fs_info->tree_root);

out:
	if (ret < 0) {
		kfree(dedup_info);
		fs_info->dedup_info = NULL;
	}
	return ret;
}

/*int btrfs_cbs_enable(struct btrfs_fs_info *fs_info, u16 type,
		       u64 blocksize, u64 limit)
{
	printk(KERN_ERR " ##### In %s ##### \n", __func__);
	struct btrfs_cbs_info *cbs_info;
	struct btrfs_root *cbs_root;
	struct btrfs_key key;
	struct btrfs_trans_handle *trans;
	struct btrfs_path *path;
	struct btrfs_cbs_status_item *status;
	int create_tree;
	u64 compat_ro_flag = btrfs_super_compat_ro_flags(fs_info->super_copy);
	int ret = 0;

	/* Meaningless and unable to enable cbs for RO fs 
	if (fs_info->sb->s_flags & MS_RDONLY)
		return -EINVAL;

	if (fs_info->cbs_info) {
		cbs_info = fs_info->cbs_info;

		/* Check if we are re-enable for different cbs config 
		if (cbs_info->hash_type != type)
		{
			btrfs_cbs_disable(fs_info);
			goto enable;
		}

		/* On-fly limit change is OK 
		mutex_lock(&cbs_info->lock);
		fs_info->cbs_info->limit_nr = limit;
		mutex_unlock(&cbs_info->lock);
		return 0;
	}

enable:
	create_tree = compat_ro_flag & BTRFS_FEATURE_COMPAT_RO_CBS;

	ret = init_cbs_info(fs_info, type, limit);
	cbs_info = fs_info->cbs_info;
	if (ret < 0)
		goto out;

	if (!create_tree)
		goto out;

	/* Create cbs tree for status at least 
	path = btrfs_alloc_path();
	if (!path) {
		ret = -ENOMEM;
		goto out;
	}

	trans = btrfs_start_transaction(fs_info->tree_root, 2);
	if (IS_ERR(trans)) {
		ret = PTR_ERR(trans);
		btrfs_free_path(path);
		goto out;
	}

	cbs_root = btrfs_create_tree(trans, fs_info,
				       BTRFS_CBS_TREE_OBJECTID);
	if (IS_ERR(cbs_root)) {
		ret = PTR_ERR(cbs_root);
		btrfs_abort_transaction(trans, fs_info->tree_root, ret);
		btrfs_free_path(path);
		goto out;
	}

	cbs_info->cbs_root = cbs_root;

	key.objectid = 0;
	key.type = BTRFS_CBS_STATUS_ITEM_KEY;
	key.offset = 0;

	ret = btrfs_insert_empty_item(trans, cbs_root, path, &key,
				      sizeof(*status));
	if (ret < 0) {
		btrfs_abort_transaction(trans, fs_info->tree_root, ret);
		btrfs_free_path(path);
		goto out;
	}
	status = btrfs_item_ptr(path->nodes[0], path->slots[0],
				struct btrfs_cbs_status_item);
	btrfs_set_cbs_status_limit(path->nodes[0], status, limit);
	btrfs_set_cbs_status_hash_type(path->nodes[0], status, type);
	btrfs_mark_buffer_dirty(path->nodes[0]);

	btrfs_free_path(path);
	ret = btrfs_commit_transaction(trans, fs_info->tree_root);

out:
	if (ret < 0) {
		kfree(cbs_info);
		fs_info->cbs_info = NULL;
	}
	return ret;
}
*/
int btrfs_dedup_resume(struct btrfs_fs_info *fs_info,
		       struct btrfs_root *dedup_root)
{
	struct btrfs_dedup_status_item *status;
	struct btrfs_key key;
	struct btrfs_path *path;
	u64 blocksize;
	u64 limit;
	u16 type;
	u16 backend;
	int ret = 0;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	key.objectid = 0;
	key.type = BTRFS_DEDUP_STATUS_ITEM_KEY;
	key.offset = 0;

	ret = btrfs_search_slot(NULL, dedup_root, &key, path, 0, 0);
	if (ret > 0) {
		ret = -ENOENT;
		goto out;
	} else if (ret < 0) {
		goto out;
	}

	status = btrfs_item_ptr(path->nodes[0], path->slots[0],
				struct btrfs_dedup_status_item);
	blocksize = btrfs_dedup_status_blocksize(path->nodes[0], status);
	limit = btrfs_dedup_status_limit(path->nodes[0], status);
	type = btrfs_dedup_status_hash_type(path->nodes[0], status);
	backend = btrfs_dedup_status_backend(path->nodes[0], status);

	ret = init_dedup_info(fs_info, type, backend, blocksize, limit);
	if (ret < 0)
		goto out;
	fs_info->dedup_info->dedup_root = dedup_root;

out:
	btrfs_free_path(path);
	return ret;
}

static int ondisk_search_hash(struct btrfs_dedup_info *dedup_info, u8 *hash,
			      u64 *bytenr_ret, u32 *num_bytes_ret);
static void inmem_destroy(struct btrfs_fs_info *fs_info);
int btrfs_dedup_cleanup(struct btrfs_fs_info *fs_info)
{
	if (!fs_info->dedup_info)
		return 0;
	if (fs_info->dedup_info->backend == BTRFS_DEDUP_BACKEND_INMEMORY)
		inmem_destroy(fs_info);
	if (fs_info->dedup_info->dedup_root) {
		free_root_extent_buffers(fs_info->dedup_info->dedup_root);
		kfree(fs_info->dedup_info->dedup_root);
	}
	crypto_free_shash(fs_info->dedup_info->dedup_driver);
	kfree(fs_info->dedup_info);
	fs_info->dedup_info = NULL;
	return 0;
}

static int inmem_insert_hash(struct rb_root *root,
			     struct inmem_hash *hash, int hash_len)
{
	printk(KERN_ERR " ##### In %s ##### \n", __func__);

	struct rb_node **p = &root->rb_node;
	struct rb_node *parent = NULL;
	struct inmem_hash *entry = NULL;

	while (*p) {
		parent = *p;
		entry = rb_entry(parent, struct inmem_hash, hash_node);
		if (memcmp(hash->hash, entry->hash, hash_len) < 0)
			p = &(*p)->rb_left;
		else if (memcmp(hash->hash, entry->hash, hash_len) > 0)
			p = &(*p)->rb_right;
		else
			return 1;
	}
	rb_link_node(&hash->hash_node, parent, p);
	rb_insert_color(&hash->hash_node, root);
	return 0;
}

static int inmem_insert_bytenr(struct rb_root *root,
			       struct inmem_hash *hash)
{
	struct rb_node **p = &root->rb_node;
	struct rb_node *parent = NULL;
	struct inmem_hash *entry = NULL;

	while (*p) {
		parent = *p;
		entry = rb_entry(parent, struct inmem_hash, bytenr_node);
		if (hash->bytenr < entry->bytenr)
			p = &(*p)->rb_left;
		else if (hash->bytenr > entry->bytenr)
			p = &(*p)->rb_right;
		else
			return 1;
	}
	rb_link_node(&hash->bytenr_node, parent, p);
	rb_insert_color(&hash->bytenr_node, root);
	return 0;
}

static void __inmem_del(struct btrfs_dedup_info *dedup_info,
			struct inmem_hash *hash)
{
	list_del(&hash->lru_list);
	rb_erase(&hash->hash_node, &dedup_info->hash_root);
	rb_erase(&hash->bytenr_node, &dedup_info->bytenr_root);

	if (!WARN_ON(dedup_info->current_nr == 0))
		dedup_info->current_nr--;

	kfree(hash);
}

/*
 * Insert a hash into in-memory dedup tree
 * Will remove exceeding last recent use hash.
 *
 * If the hash mathced with existing one, we won't insert it, to
 * save memory
 */
static int inmem_add(struct btrfs_dedup_info *dedup_info,
		     struct btrfs_dedup_hash *hash)
{
	printk(KERN_ERR " ##### In %s ##### \n", __func__);

	int ret = 0;
	u16 type = dedup_info->hash_type;
	struct inmem_hash *ihash;

	ihash = inmem_alloc_hash(type);

	if (!ihash)
		return -ENOMEM;

	/* Copy the data out */
	ihash->bytenr = hash->bytenr;
	ihash->num_bytes = hash->num_bytes;
	memcpy(ihash->hash, hash->hash, btrfs_dedup_sizes[type]);

	mutex_lock(&dedup_info->lock);

	ret = inmem_insert_bytenr(&dedup_info->bytenr_root, ihash);
	if (ret > 0) {
		kfree(ihash);
		ret = 0;
		goto out;
	}

	ret = inmem_insert_hash(&dedup_info->hash_root, ihash,
				btrfs_dedup_sizes[type]);
	if (ret > 0) {
		/*
		 * We only keep one hash in tree to save memory, so if
		 * hash conflicts, free the one to insert.
		 */
		rb_erase(&ihash->bytenr_node, &dedup_info->bytenr_root);
		kfree(ihash);
		ret = 0;
		goto out;
	}

	list_add(&ihash->lru_list, &dedup_info->lru_list);
	dedup_info->current_nr++;

	/* Remove the last dedup hash if we exceed limit */
	while (dedup_info->current_nr > dedup_info->limit_nr) {
		struct inmem_hash *last;

		last = list_entry(dedup_info->lru_list.prev,
				  struct inmem_hash, lru_list);
		__inmem_del(dedup_info, last);
	}
out:
	mutex_unlock(&dedup_info->lock);
	return 0;
}

static int ondisk_search_bytenr(struct btrfs_trans_handle *trans,
				struct btrfs_dedup_info *dedup_info,
				struct btrfs_path *path, u64 bytenr,
				int prepare_del);
static int ondisk_add(struct btrfs_trans_handle *trans,
		      struct btrfs_dedup_info *dedup_info,
		      struct btrfs_dedup_hash *hash)
{
	printk(KERN_ERR " ##### In %s ##### \n", __func__);

	struct btrfs_path *path;
	struct btrfs_root *dedup_root = dedup_info->dedup_root;
	struct btrfs_key key;
	struct btrfs_dedup_hash_item *hash_item;
	u64 bytenr;
	u32 num_bytes;
	int hash_len = btrfs_dedup_sizes[dedup_info->hash_type];
	int ret;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	mutex_lock(&dedup_info->lock);

	ret = ondisk_search_bytenr(NULL, dedup_info, path, hash->bytenr, 0);
	if (ret < 0)
		goto out;
	if (ret > 0) {
		ret = 0;
		goto out;
	}
	btrfs_release_path(path);

	ret = ondisk_search_hash(dedup_info, hash->hash, &bytenr, &num_bytes);
	if (ret < 0)
		goto out;
	/* Same hash found, don't re-add to save dedup tree space */
	if (ret > 0) {
		ret = 0;
		goto out;
	}

	/* Insert hash->bytenr item */
	memcpy(&key.objectid, hash->hash + hash_len - 8, 8);
	key.type = BTRFS_DEDUP_HASH_ITEM_KEY;
	key.offset = hash->bytenr;

	ret = btrfs_insert_empty_item(trans, dedup_root, path, &key,
			sizeof(*hash_item) + hash_len);
	WARN_ON(ret == -EEXIST);
	if (ret < 0)
		goto out;
	hash_item = btrfs_item_ptr(path->nodes[0], path->slots[0],
				   struct btrfs_dedup_hash_item);
	btrfs_set_dedup_hash_len(path->nodes[0], hash_item, hash->num_bytes);
	write_extent_buffer(path->nodes[0], hash->hash,
			    (unsigned long)(hash_item + 1), hash_len);
	btrfs_mark_buffer_dirty(path->nodes[0]);
	btrfs_release_path(path);

	/* Then bytenr->hash item */
	key.objectid = hash->bytenr;
	key.type = BTRFS_DEDUP_BYTENR_ITEM_KEY;
	memcpy(&key.offset, hash->hash + hash_len - 8, 8);

	ret = btrfs_insert_empty_item(trans, dedup_root, path, &key, hash_len);
	WARN_ON(ret == -EEXIST);
	if (ret < 0)
		goto out;
	write_extent_buffer(path->nodes[0], hash->hash,
			btrfs_item_ptr_offset(path->nodes[0], path->slots[0]),
			hash_len);
	btrfs_mark_buffer_dirty(path->nodes[0]);

out:
	mutex_unlock(&dedup_info->lock);
	btrfs_free_path(path);
	return ret;
}

int btrfs_dedup_add(struct btrfs_trans_handle *trans, struct btrfs_root *root,
		    struct btrfs_dedup_hash *hash)
{
	printk(KERN_INFO " ##### In %s ##### \n", __func__);

	struct btrfs_fs_info *fs_info = root->fs_info;
	struct btrfs_dedup_info *dedup_info = fs_info->dedup_info;

	if (!dedup_info || !hash)
		return 0;

	if (WARN_ON(hash->bytenr == 0))
		return -EINVAL;

	if (dedup_info->backend == BTRFS_DEDUP_BACKEND_INMEMORY)
		return inmem_add(dedup_info, hash);
	if (dedup_info->backend == BTRFS_DEDUP_BACKEND_ONDISK)
		return ondisk_add(trans, dedup_info, hash);
	return -EINVAL;
}

static struct inmem_hash *
inmem_search_bytenr(struct btrfs_dedup_info *dedup_info, u64 bytenr)
{
	struct rb_node **p = &dedup_info->bytenr_root.rb_node;
	struct rb_node *parent = NULL;
	struct inmem_hash *entry = NULL;

	while (*p) {
		parent = *p;
		entry = rb_entry(parent, struct inmem_hash, bytenr_node);

		if (bytenr < entry->bytenr)
			p = &(*p)->rb_left;
		else if (bytenr > entry->bytenr)
			p = &(*p)->rb_right;
		else
			return entry;
	}

	return NULL;
}

/* Delete a hash from in-memory dedup tree */
static int inmem_del(struct btrfs_dedup_info *dedup_info, u64 bytenr)
{
	struct inmem_hash *hash;

	mutex_lock(&dedup_info->lock);
	hash = inmem_search_bytenr(dedup_info, bytenr);
	if (!hash) {
		mutex_unlock(&dedup_info->lock);
		return 0;
	}

	__inmem_del(dedup_info, hash);
	mutex_unlock(&dedup_info->lock);
	return 0;
}

/*
 * If prepare_del is given, this will setup search_slot() for delete.
 * Caller needs to do proper locking.
 *
 * Return > 0 for found.
 * Return 0 for not found.
 * Return < 0 for error.
 */
static int ondisk_search_bytenr(struct btrfs_trans_handle *trans,
				struct btrfs_dedup_info *dedup_info,
				struct btrfs_path *path, u64 bytenr,
				int prepare_del)
{
	struct btrfs_key key;
	struct btrfs_root *dedup_root = dedup_info->dedup_root;
	int ret;
	int ins_len = 0;
	int cow = 0;

	if (prepare_del) {
		if (WARN_ON(trans == NULL))
			return -EINVAL;
		cow = 1;
		ins_len = -1;
	}

	key.objectid = bytenr;
	key.type = BTRFS_DEDUP_BYTENR_ITEM_KEY;
	key.offset = (u64)-1;

	ret = btrfs_search_slot(trans, dedup_root, &key, path,
				ins_len, cow);
	if (ret < 0)
		return ret;

	WARN_ON(ret == 0);
	ret = btrfs_previous_item(dedup_root, path, bytenr,
				  BTRFS_DEDUP_BYTENR_ITEM_KEY);
	if (ret < 0)
		return ret;
	if (ret > 0)
		return 0;
	return 1;
}

static int ondisk_del(struct btrfs_trans_handle *trans,
		      struct btrfs_dedup_info *dedup_info, u64 bytenr)
{
	struct btrfs_root *dedup_root = dedup_info->dedup_root;
	struct btrfs_path *path;
	struct btrfs_key key;
	int ret;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	key.objectid = bytenr;
	key.type = BTRFS_DEDUP_BYTENR_ITEM_KEY;
	key.offset = 0;

	mutex_lock(&dedup_info->lock);

	ret = ondisk_search_bytenr(trans, dedup_info, path, bytenr, 1);
	if (ret <= 0)
		goto out;

	btrfs_item_key_to_cpu(path->nodes[0], &key, path->slots[0]);
	btrfs_del_item(trans, dedup_root, path);
	btrfs_release_path(path);

	/* Search for hash item and delete it */
	key.objectid = key.offset;
	key.type = BTRFS_DEDUP_HASH_ITEM_KEY;
	key.offset = bytenr;

	ret = btrfs_search_slot(trans, dedup_root, &key, path, -1, 1);
	if (WARN_ON(ret > 0)) {
		ret = -ENOENT;
		goto out;
	}
	if (ret < 0)
		goto out;
	btrfs_del_item(trans, dedup_root, path);

out:
	btrfs_free_path(path);
	mutex_unlock(&dedup_info->lock);
	return ret;
}

/* Remove a dedup hash from dedup tree */
int btrfs_dedup_del(struct btrfs_trans_handle *trans, struct btrfs_root *root,
		    u64 bytenr)
{
	struct btrfs_fs_info *fs_info = root->fs_info;
	struct btrfs_dedup_info *dedup_info = fs_info->dedup_info;

	if (!dedup_info)
		return 0;

	if (dedup_info->backend == BTRFS_DEDUP_BACKEND_INMEMORY)
		return inmem_del(dedup_info, bytenr);
	if (dedup_info->backend == BTRFS_DEDUP_BACKEND_ONDISK)
		return ondisk_del(trans, dedup_info, bytenr);
	return -EINVAL;
}

static void inmem_destroy(struct btrfs_fs_info *fs_info)
{
	struct inmem_hash *entry, *tmp;
	struct btrfs_dedup_info *dedup_info = fs_info->dedup_info;

	mutex_lock(&dedup_info->lock);
	list_for_each_entry_safe(entry, tmp, &dedup_info->lru_list, lru_list)
		__inmem_del(dedup_info, entry);
	mutex_unlock(&dedup_info->lock);
}

int btrfs_dedup_disable(struct btrfs_fs_info *fs_info)
{
	printk(KERN_ERR " ##### In %s ##### \n", __func__);

	struct btrfs_dedup_info *dedup_info = fs_info->dedup_info;
	int ret = 0;

	if (!dedup_info)
		return 0;

	if (dedup_info->backend == BTRFS_DEDUP_BACKEND_INMEMORY)
		inmem_destroy(fs_info);
	if (dedup_info->dedup_root)
		ret = btrfs_drop_snapshot(dedup_info->dedup_root, NULL, 1, 0);
	crypto_free_shash(fs_info->dedup_info->dedup_driver);
	kfree(fs_info->dedup_info);
	fs_info->dedup_info = NULL;
	return ret;
}

/*
 * Return 0 for not found
 * Return >0 for found and set bytenr_ret
 * Return <0 for error
 */
static int ondisk_search_hash(struct btrfs_dedup_info *dedup_info, u8 *hash,
			      u64 *bytenr_ret, u32 *num_bytes_ret)
{
	printk(KERN_ERR " ##### In %s ##### \n", __func__);

	struct btrfs_path *path;
	struct btrfs_key key;
	struct btrfs_root *dedup_root = dedup_info->dedup_root;
	u8 *buf = NULL;
	u64 hash_key;
	int hash_len = btrfs_dedup_sizes[dedup_info->hash_type];
	int ret;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	buf = kmalloc(hash_len, GFP_NOFS);
	if (!buf) {
		ret = -ENOMEM;
		goto out;
	}

	memcpy(&hash_key, hash + hash_len - 8, 8);
	key.objectid = hash_key;
	key.type = BTRFS_DEDUP_HASH_ITEM_KEY;
	key.offset = (u64)-1;

	ret = btrfs_search_slot(NULL, dedup_root, &key, path, 0, 0);
	if (ret < 0)
		goto out;
	WARN_ON(ret == 0);
	while (1) {
		struct extent_buffer *node;
		struct btrfs_dedup_hash_item *hash_item;
		int slot;

		ret = btrfs_previous_item(dedup_root, path, hash_key,
					  BTRFS_DEDUP_HASH_ITEM_KEY);
		if (ret < 0)
			goto out;
		if (ret > 0) {
			ret = 0;
			goto out;
		}

		node = path->nodes[0];
		slot = path->slots[0];
		btrfs_item_key_to_cpu(node, &key, slot);

		if (key.type != BTRFS_DEDUP_HASH_ITEM_KEY ||
		    memcmp(&key.objectid, hash + hash_len - 8, 8))
			break;
		hash_item = btrfs_item_ptr(node, slot,
				struct btrfs_dedup_hash_item);
		read_extent_buffer(node, buf, (unsigned long)(hash_item + 1),
				   hash_len);
		if (!memcmp(buf, hash, hash_len)) {
			ret = 1;
			*bytenr_ret = key.offset;
			*num_bytes_ret = btrfs_dedup_hash_len(node, hash_item);
			break;
		}
	}
out:
	kfree(buf);
	btrfs_free_path(path);
	return ret;
}

/*
 * Caller must ensure the corresponding ref head is not being run.
 */
static struct inmem_hash *
inmem_search_hash(struct btrfs_dedup_info *dedup_info, u8 *hash)
{
	printk(KERN_ERR " ##### In %s ##### \n", __func__);

	struct rb_node **p = &dedup_info->hash_root.rb_node;
	struct rb_node *parent = NULL;
	struct inmem_hash *entry = NULL;
	u16 hash_type = dedup_info->hash_type;
	int hash_len = btrfs_dedup_sizes[hash_type];

	while (*p) {
		parent = *p;
		entry = rb_entry(parent, struct inmem_hash, hash_node);

		if (memcmp(hash, entry->hash, hash_len) < 0) {
			p = &(*p)->rb_left;
		} else if (memcmp(hash, entry->hash, hash_len) > 0) {
			p = &(*p)->rb_right;
		} else {
			/* Found, need to re-add it to LRU list head */
			list_del(&entry->lru_list);
			list_add(&entry->lru_list, &dedup_info->lru_list);
			return entry;
		}
	}
	return NULL;
}

/* Wrapper for different backends, caller needs to hold dedup_info->lock */
static inline int generic_search_hash(struct btrfs_dedup_info *dedup_info,
				      u8 *hash, u64 *bytenr_ret,
				      u32 *num_bytes_ret)
{
	printk(KERN_ERR " ##### In %s ##### \n", __func__);

	if (dedup_info->backend == BTRFS_DEDUP_BACKEND_INMEMORY) {
		struct inmem_hash *found_hash;
		int ret;

		found_hash = inmem_search_hash(dedup_info, hash);
		if (found_hash) {
			ret = 1;
			*bytenr_ret = found_hash->bytenr;
			*num_bytes_ret = found_hash->num_bytes;
		} else {
			ret = 0;
			*bytenr_ret = 0;
			*num_bytes_ret = 0;
		}
		return ret;
	} else if (dedup_info->backend == BTRFS_DEDUP_BACKEND_ONDISK) {
		return ondisk_search_hash(dedup_info, hash, bytenr_ret,
					  num_bytes_ret);
	}
	return -EINVAL;
}

static int generic_search(struct inode *inode, u64 file_pos,
			struct btrfs_dedup_hash *hash)
{
	printk(KERN_ERR " ##### In %s ##### \n", __func__);

	int ret;
	struct btrfs_root *root = BTRFS_I(inode)->root;
	struct btrfs_fs_info *fs_info = root->fs_info;
	struct btrfs_trans_handle *trans;
	struct btrfs_delayed_ref_root *delayed_refs;
	struct btrfs_delayed_ref_head *head;
	struct btrfs_dedup_info *dedup_info = fs_info->dedup_info;
	u64 bytenr;
	u64 tmp_bytenr;
	u32 num_bytes;

	trans = btrfs_join_transaction(root);
	if (IS_ERR(trans))
		return PTR_ERR(trans);

again:
	mutex_lock(&dedup_info->lock);
	ret = generic_search_hash(dedup_info, hash->hash, &bytenr, &num_bytes);
	if (ret <= 0)
		goto out;

	delayed_refs = &trans->transaction->delayed_refs;

	spin_lock(&delayed_refs->lock);
	head = btrfs_find_delayed_ref_head(trans, bytenr);
	if (!head) {
		/*
		 * We can safely insert a new delayed_ref as long as we
		 * hold delayed_refs->lock.
		 * Only need to use atomic inc_extent_ref()
		 */
		ret = btrfs_inc_extent_ref_atomic(trans, root, bytenr,
				num_bytes, 0, root->root_key.objectid,
				btrfs_ino(inode), file_pos);
		spin_unlock(&delayed_refs->lock);

		if (ret == 0) {
			hash->bytenr = bytenr;
			hash->num_bytes = num_bytes;
			ret = 1;
		}
		goto out;
	}

	/*
	 * We can't lock ref head with dedup_info->lock hold or we will cause
	 * ABBA dead lock.
	 */
	mutex_unlock(&dedup_info->lock);
	ret = btrfs_delayed_ref_lock(trans, head);
	spin_unlock(&delayed_refs->lock);
	if (ret == -EAGAIN)
		goto again;

	mutex_lock(&dedup_info->lock);
	/*
	 * Search again to ensure the hash is still here and bytenr didn't
	 * change
	 */
	ret = generic_search_hash(dedup_info, hash->hash, &tmp_bytenr,
				  &num_bytes);
	if (ret <= 0) {
		mutex_unlock(&head->mutex);
		goto out;
	}
	if (tmp_bytenr != bytenr) {
		mutex_unlock(&head->mutex);
		mutex_unlock(&dedup_info->lock);
		goto again;
	}
	hash->bytenr = bytenr;
	hash->num_bytes = num_bytes;

	/*
	 * Increase the extent ref right now, to avoid delayed ref run
	 * Or we may increase ref on non-exist extent.
	 */
	btrfs_inc_extent_ref(trans, root, bytenr, num_bytes, 0,
			     root->root_key.objectid,
			     btrfs_ino(inode), file_pos);
	mutex_unlock(&head->mutex);
out:
	mutex_unlock(&dedup_info->lock);
	btrfs_end_transaction(trans, root);

	return ret;
}

int btrfs_dedup_search(struct inode *inode, u64 file_pos,
		       struct btrfs_dedup_hash *hash)
{
	printk(KERN_ERR " ##### In %s ##### \n", __func__);

	struct btrfs_fs_info *fs_info = BTRFS_I(inode)->root->fs_info;
	struct btrfs_dedup_info *dedup_info = fs_info->dedup_info;
	int ret = 0;

	if (WARN_ON(!dedup_info || !hash))
		return 0;

	if (dedup_info->backend < BTRFS_DEDUP_BACKEND_LAST) {
		ret = generic_search(inode, file_pos, hash);
		if (ret == 0) {
			hash->num_bytes = 0;
			hash->bytenr = 0;
		}
		return ret;
	}
	return -EINVAL;
}

static int hash_data(struct btrfs_dedup_info *dedup_info, const char *data,
		     u64 length, struct btrfs_dedup_hash *hash)
{
	printk(KERN_ERR " ##### In %s ##### \n", __func__);
	
	struct crypto_shash *tfm = dedup_info->dedup_driver;
	struct {
		struct shash_desc desc;
		char ctx[crypto_shash_descsize(tfm)];
	} sdesc;
	int ret;

	sdesc.desc.tfm = tfm;
	sdesc.desc.flags = 0;

	ret = crypto_shash_digest(&sdesc.desc, data, length,
				  (char *)(hash->hash));

	printk(KERN_INFO "%s : crypto_shash_digest returned %d #####_______ \n", __func__, ret);
	return ret;
}

int btrfs_dedup_calc_hash(struct btrfs_root *root, struct inode *inode,
			  u64 start, struct btrfs_dedup_hash *hash)
{
	struct page *p;
	struct btrfs_dedup_info *dedup_info = root->fs_info->dedup_info;
	char *data;
	int i;
	int ret;
	u64 dedup_bs;
	u64 sectorsize = root->sectorsize;

	if (!dedup_info || !hash)
		return 0;

	WARN_ON(!IS_ALIGNED(start, sectorsize));

	dedup_bs = dedup_info->blocksize;
	sectorsize = root->sectorsize;

	data = kmalloc(dedup_bs, GFP_NOFS);
	if (!data)
		return -ENOMEM;
	for (i = 0; sectorsize * i < dedup_bs; i++) {
		char *d;

		/* TODO: Add support for subpage size case */
		p = find_get_page(inode->i_mapping,
				  (start >> PAGE_CACHE_SHIFT) + i);
		if (WARN_ON(!p)) {
			ret = -ENOENT;
			goto out;
		}
		d = kmap_atomic(p);
		memcpy((data + sectorsize * i), d, sectorsize);
		kunmap_atomic(d);
		page_cache_release(p);
	}
	ret = hash_data(dedup_info, data, dedup_bs, hash);
out:
	kfree(data);
	return ret;
}


int btrfs_cbs_calc_hash(struct btrfs_root *root, struct inode *inode,
			  u64 start, u64 end, struct btrfs_dedup_hash *hash)
{
	printk(KERN_ERR " ##### In %s ##### \n", __func__);

	struct page *p;
	struct btrfs_dedup_info *dedup_info = root->fs_info->dedup_info;
	char *data;
	int i;
	int ret;
	u64 len;
	u64 sectorsize = root->sectorsize;

	if (!dedup_info || !hash)
		return 0;

	WARN_ON(!IS_ALIGNED(start, sectorsize));

	len = end-start+1;
	data = kmalloc(len, GFP_NOFS);
	if (!data) {
		printk(KERN_INFO "%s : kmalloc failed #####_______ \n", __func__);
		return -ENOMEM;
	}
	for (i = 0; sectorsize * i < len; i++) {
		char *d;

		/* TODO: Add support for subpage size case */
		p = find_get_page(inode->i_mapping,
				  (start >> PAGE_CACHE_SHIFT) + i);
		WARN_ON(!p);
		d = kmap_atomic(p);
		memcpy((data + sectorsize * i), d, sectorsize);
		kunmap_atomic(d);
		page_cache_release(p);
	}
	ret = hash_data(dedup_info, data, len, hash);
	printk(KERN_INFO "%s : hash_data returned %d #####_______ \n", __func__, ret);
	kfree(data);
	return ret;
}
