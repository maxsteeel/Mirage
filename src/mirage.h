/*
 * Mirage: A stackable virtual file system for Android content redirection.
 *   Based on WrapFS by Erez Zadok.
 *   This file contains the private declarations for mirage.
 */

#ifndef _MIRAGE_H_
#define _MIRAGE_H_

#include <linux/version.h>
#include <linux/dcache.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/mount.h>
#include <linux/namei.h>
#include <linux/seq_file.h>
#include <linux/statfs.h>
#include <linux/fs_stack.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/xattr.h>
#include <linux/path.h>
#include <linux/limits.h>
#include <linux/exportfs.h>
#include <linux/rcupdate.h>
#include <linux/module.h>
#include <linux/security.h>

#include "compat.h"
#ifdef MIRAGE_KERNEL_UMOUNT
/* Include kernel umount support */
#include "kernel_umount.h"
#endif

/* The file system name for 'mount -t mirage' */
#define MIRAGE_NAME "mirage"

/* Mirage magic number */
#define MIRAGE_FS_MAGIC 0xF18F

/* Max number of stacked directories */
#define MIRAGE_MAX_BRANCHES 5

/* Operations vectors defined in specific files */
extern struct file_system_type mirage_fs_type;
extern const struct file_operations mirage_main_fops;
extern const struct file_operations mirage_dir_fops;
extern const struct inode_operations mirage_main_iops;
extern const struct inode_operations mirage_dir_iops;
extern const struct inode_operations mirage_symlink_iops;
extern const struct export_operations mirage_export_ops;

#ifdef MIRAGE_KERNEL_UMOUNT
/* Tracepoint hook functions */
extern int mirage_init_tp_hooks(void);
extern void mirage_exit_tp_hooks(void);
#endif

/* File private data: link to the real underlying file(s) */
struct mirage_file_info {
	struct file *lower_files[MIRAGE_MAX_BRANCHES];
	int num_lower_files;
};

/* Inode data in memory */
struct mirage_inode_info {
	/* vfs_inode MUST be at Offset 0. */
	struct inode vfs_inode;
	struct inode *lower_inode;
};

/* Dentry data in memory */
struct mirage_dentry_info {
	int num_lower_paths;
	struct path lower_paths[MIRAGE_MAX_BRANCHES];
};

/* super-block data in memory */
struct mirage_sb_info {
	struct super_block *lower_sb;
	struct path lower_paths[MIRAGE_MAX_BRANCHES];
	int num_lower_paths;
	bool has_inject;
	bool has_upperdir;
	char *inject_name; /* Name of the file to intercept */
	size_t inject_name_len;     /* Precomputed length of inject_name */
	u32 inject_name_hash;       /* Precomputed hash of inject_name */
	struct path inject_path;    /* Actual path of the modified file */
	/* Original path strings saved for /proc/mounts show_options.
	 * d_path() on private vfsmount clones returns "/" — unusable.
	 * Store the strings from the mount options directly instead. */
	char *lower_path_strs[MIRAGE_MAX_BRANCHES];
	char *inject_path_str;
};

/* Structure to safely pass assembly data to fill_super */
struct mirage_mount_data {
	const char *dev_name;
	void *raw_data;
};

/*
 * Inode to private data conversion.
 * Since struct inode is embedded, this will always return a valid pointer.
 */
static inline struct mirage_inode_info *mirage_inode(const struct inode *inode)
{
	return container_of(inode, struct mirage_inode_info, vfs_inode);
}

/* Helper macros for accessing private data */
#define mirage_dentry(dent) ((struct mirage_dentry_info *)(dent)->d_fsdata)
#define mirage_sb(super) ((struct mirage_sb_info *)(super)->s_fs_info)
#define mirage_file(file) ((struct mirage_file_info *)((file)->private_data))

/* Helper functions to get/set lower (real) objects */
static inline struct file *mirage_lower_file(const struct file *file)
{
	return mirage_file(file)->lower_files[0]; /* Return topmost file for general I/O */
}

static inline struct inode *mirage_lower_inode(const struct inode *inode)
{
	return mirage_inode(inode)->lower_inode;
}

/* Copying and handling lower paths safely */
static inline void pathcpy(struct path *dst, const struct path *src)
{
	/* Struct assignment allows the compiler to emit LDP/STP
	 * instructions, doing the copy in a single clock cycle
	 * instead of member-by-member.
	 */
	*dst = *src;
}

/* Safely retrieve the lower path. 
 * Lockless design: lower_paths[] is immutable after setup. */
static inline void get_lower_path(const struct dentry *dent, struct path *lower_path)
{
	struct mirage_dentry_info *info = rcu_dereference_raw(dent->d_fsdata);

	if (likely(info && info->num_lower_paths > 0)) {
		pathcpy(lower_path, &info->lower_paths[0]);
		path_get(lower_path);
	} else {
		lower_path->dentry = NULL;
		lower_path->mnt = NULL;
	}
}

/* Lockless retrieval of all merged lower paths */
static inline int get_all_lower_paths(const struct dentry *dent, struct path *lower_paths)
{
	struct mirage_dentry_info *info = rcu_dereference_raw(dent->d_fsdata);
	int i, num;

	if (unlikely(!info)) return 0;

	num = info->num_lower_paths;
	for (i = 0; i < num; i++) {
		pathcpy(&lower_paths[i], &info->lower_paths[i]);
		path_get(&lower_paths[i]);
	}
	return num;
}

static inline void put_lower_path(const struct dentry *dent, struct path *lower_path)
{
	if (lower_path->dentry)
		path_put(lower_path);
}

static inline void put_all_lower_paths(const struct dentry *dent, struct path *lower_paths, int num)
{
	int i;
	for (i = 0; i < num; i++) {
		if (lower_paths[i].dentry)
			path_put(&lower_paths[i]);
	}
}

/* Helpers to set private data - MUST only be called before dentry is visible */
static inline void set_lower_inode(struct inode *inode, struct inode *lowernode)
{
	mirage_inode(inode)->lower_inode = lowernode;
}

/*
 * Set lower path(s) for a dentry.
 *
 * SAFETY: Must only be called during dentry creation (vfs_lookup/fill_super)
 * BEFORE the dentry is made visible via d_add() or d_splice_alias().
 * At that point, no RCU readers can exist, so no locking is needed.
 *
 * Called from:
 * - mirage_vfs_lookup(): after new_dentry_private_data(), before d_add/d_splice_alias
 * - mirage_fill_super(): for root dentry, before sb->s_root assignment
 */
static inline void set_lower_paths(struct dentry *dent, struct path *lower_paths, int num_paths)
{
	int i;
	for (i = 0; i < num_paths; i++) {
		pathcpy(&mirage_dentry(dent)->lower_paths[i], &lower_paths[i]);
	}
	mirage_dentry(dent)->num_lower_paths = num_paths;
}

/* Locking helpers for directory operations */
static inline struct dentry *lock_parent(struct dentry *dentry)
{
	struct dentry *dir = dget_parent(dentry);
	inode_lock(d_inode(dir));
	return dir;
}

static inline void unlock_dir(struct dentry *dir)
{
	inode_unlock(d_inode(dir));
	dput(dir);
}

#endif	/* _MIRAGE_H_ */
