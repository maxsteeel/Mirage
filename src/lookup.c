/*
 * Mirage: Path Resolution and Inode Management
 */

extern struct dentry *lookup_one_len_unlocked(const char *name, struct dentry *base, int len);

/* * Inode Cache: Handles the mapping between virtual and real inodes.
 */
static int mirage_inode_test(struct inode *inode, void *candidate_lower_inode)
{
	struct inode *current_lower_inode = mirage_lower_inode(inode);
	return (current_lower_inode == (struct inode *)candidate_lower_inode);
}

/* Atomic initialization function required by iget5_locked */
static int mirage_inode_set(struct inode *inode, void *opaque)
{
	struct inode *lower_inode = opaque;
	
	/* We assign the inode number here, under the VFS locks */
	inode->i_ino = lower_inode->i_ino;
	/* Required for i_generation and NFS exports */
	inode->i_generation = lower_inode->i_generation;
	return 0;
}

/* * mirage_iget: Retrieves an existing virtual inode or creates a new one.
 */
static struct inode *mirage_iget(struct super_block *sb, struct inode *lower_inode)
{
	struct inode *inode;

	if (unlikely(!igrab(lower_inode)))
		return ERR_PTR(-ESTALE);

	/* Use hash lookup to find if we already have a virtual inode for this real one. */
	inode = iget5_locked(sb, lower_inode->i_ino,
			     mirage_inode_test, mirage_inode_set, lower_inode);

	if (unlikely(!inode)) {
		iput(lower_inode);
		return ERR_PTR(-ENOMEM);
	}

	if (!(inode->i_state & I_NEW)) {
		iput(lower_inode);
		return inode;
	}

	/* Setup the new inode */
	set_lower_inode(inode, lower_inode);

	/* Inherit operations based on type */
	if (S_ISDIR(lower_inode->i_mode)) {
		inode->i_op = &mirage_dir_iops;
		inode->i_fop = &mirage_dir_fops;
	} else if (S_ISLNK(lower_inode->i_mode)) {
		inode->i_op = &mirage_symlink_iops;
		inode->i_fop = &mirage_main_fops;
	} else {
		inode->i_op = &mirage_main_iops;
		inode->i_fop = &mirage_main_fops;
	}

	/* Properly initialize special devices (char, block, fifo) */
	if (unlikely(S_ISBLK(lower_inode->i_mode) || S_ISCHR(lower_inode->i_mode) ||
	    S_ISFIFO(lower_inode->i_mode) || S_ISSOCK(lower_inode->i_mode)))
		init_special_inode(inode, lower_inode->i_mode, lower_inode->i_rdev);

	fsstack_copy_attr_all(inode, lower_inode);
	fsstack_copy_inode_size(inode, lower_inode);
	inode->i_mapping = lower_inode->i_mapping;

	/* Unlock the inode so the rest of the system can use it */
	unlock_new_inode(inode);
	return inode;
}

/* * __mirage_interpose: Links a virtual dentry with its inode.
 */
static struct dentry *__mirage_interpose(struct dentry *dentry,
					 struct super_block *sb,
					 struct path *lower_path)
{
	struct inode *inode;
	struct inode *lower_inode = d_inode(lower_path->dentry);

	inode = mirage_iget(sb, lower_inode);
	if (IS_ERR(inode))
		return ERR_CAST(inode);

	/* Use d_splice_alias to handle existing dentries safely */
	return d_splice_alias(inode, dentry);
}

/* * mirage_vfs_lookup: The main entry point for path resolution.
 */
static struct dentry *mirage_vfs_lookup(struct inode *dir, struct dentry *dentry, unsigned int flags)
{
	struct dentry *ret, *lower_dentry;
	struct mirage_dentry_info *parent_info;
	struct path found_paths[MIRAGE_MAX_BRANCHES];
	int num_found_paths = 0;
	struct mirage_sb_info *sbi;
	struct qstr name = dentry->d_name;
	int err, i;

	/* Allocate private data for the dentry info */
	err = new_dentry_private_data(dentry);
	if (err) {
		return ERR_PTR(err);
	}

	/* Get the superblock info from the dentry's superblock, not allocate new */
	sbi = mirage_sb(dentry->d_sb);

	if (unlikely(sbi && sbi->has_inject && name.hash == sbi->inject_name_hash && 
	      name.len == sbi->inject_name_len && memcmp(name.name, sbi->inject_name, name.len) == 0)) {
		pathcpy(&found_paths[0], &sbi->inject_path);
		path_get(&found_paths[0]);
		lower_dentry = found_paths[0].dentry;
		num_found_paths = 1;
	} else {
	        /*
		 * The VFS already locked the parent directory (dir->i_rwsem) before 
		 * calling lookup. This guarantees dentry->d_parent and its d_fsdata 
		 * are 100% stable. We read the raw pointer directly, bypassing 
		 * dget_parent() and atomic path_get() loops entirely.
		 */
		parent_info = rcu_dereference_raw(dentry->d_parent->d_fsdata);
		struct path first_negative_path = { .dentry = NULL, .mnt = NULL };

		for (i = 0; i < parent_info->num_lower_paths; i++) {
			struct path lower_path;

			lower_dentry = lookup_one_len_unlocked(name.name, parent_info->lower_paths[i].dentry, name.len);

			if (IS_ERR(lower_dentry)) {
				continue;
			}

			/*
			 * If the child dentry is itself a mount point, follow_down
			 * traverses into the mounted filesystem. Without this, a
			 * union mount where lowerdir == mount point would recurse
			 * back into mirage_vfs_lookup infinitely, crashing the kernel.
			 *
			 * We build a full path so follow_down can update both the
			 * dentry and the vfsmount atomically.
			 */
			lower_path.dentry = lower_dentry;
			/* We still mntget because follow_down modifies the mount reference */
			lower_path.mnt = mntget(parent_info->lower_paths[i].mnt);
			err = follow_down(&lower_path, flags);
			if (err < 0) {
				/* follow_down failed — clean up and continue to next layer
				 * path_put releases BOTH mnt and dentry */
				path_put(&lower_path);
				continue;
			}
			lower_dentry = lower_path.dentry;
			/* lower_path.mnt may have changed — use it for found_paths */

			if (d_really_is_positive(lower_dentry)) {
				found_paths[num_found_paths].dentry = lower_dentry;
				found_paths[num_found_paths].mnt = lower_path.mnt;
				num_found_paths++;

				if (!S_ISDIR(d_inode(lower_dentry)->i_mode)) {
					break; /* Shadowing: top file hides lower files/dirs */
				}
			} else {
				/* It's negative. Save the first one (from layer 0) for creations */
				if (!first_negative_path.dentry) {
					first_negative_path.dentry = lower_dentry;
					first_negative_path.mnt = lower_path.mnt;
				} else {
					path_put(&lower_path);
				}
			}
		}

		/* If we found nothing positive, but we found a negative dentry, use it */
		if (num_found_paths == 0 && first_negative_path.dentry) {
			found_paths[0] = first_negative_path;
			num_found_paths = 1;
		} else if (first_negative_path.dentry) {
			/* Found positive later, release the cached negative dentry */
			path_put(&first_negative_path);
		}
	}

	if (num_found_paths == 0) {
		/* Negative dentry: file doesn't exist AND no negative dentry available (rare error). */
		set_lower_paths(dentry, found_paths, 0); 
		d_add(dentry, NULL);
		ret = NULL;
	} else if (d_really_is_negative(found_paths[0].dentry)) {
		/* Proper negative dentry cached, ready for creation */
		set_lower_paths(dentry, found_paths, 1);
		d_add(dentry, NULL);
		ret = NULL;
	} else {
		set_lower_paths(dentry, found_paths, num_found_paths);
		/* Positive dentry: Interpose virtual dentry with real inode (from topmost branch) */
		ret = __mirage_interpose(dentry, dentry->d_sb, &found_paths[0]);
		if (IS_ERR(ret)) {
			/*
			 * Interpose failed. free_dentry_private_data releases all paths
			 * that were stored via set_lower_paths — do NOT put them
			 * again. Jump directly to parent cleanup.
			 */
			free_dentry_private_data(dentry);
			return ret; /* VFS inherently handles parent state */
		}
	}

	/*
	 * We DO NOT sync atime/mtime for the dentry or the parent here.
	 * Doing so forces cache invalidations multiple times per file open.
	 * getattr() fetches accurate times directly from disk when stat is called.
	 */

	return ret;
}
