/*
 * Mirage: Inode operations
 * Full R/W support with xattr (SELinux), rename, symlink and legacy compatibility.
 */

static int mirage_vfs_create(IDMAP_ARG, struct inode *dir, struct dentry *dentry, umode_t mode, bool want_excl)
{
	int err;
	struct dentry *lower_dentry, *lower_parent_dentry;
	struct dentry *parent_dentry = dget_parent(dentry);
	struct mirage_dentry_info *parent_info = mirage_dentry(parent_dentry);
	struct mirage_dentry_info *info;
	struct inode *new_inode;
	struct path lower_path;
	
	/* 1. Extract parent information safely */
	if (!parent_info || !parent_info->lower_paths[0].dentry) {
		dput(parent_dentry);
		return -EINVAL;
	}

	/* 2. Lock the physical parent directory */
	lower_parent_dentry = parent_info->lower_paths[0].dentry;
	inode_lock_nested(d_inode(lower_parent_dentry), I_MUTEX_PARENT);

	/* 3. Lookup the new name in the physical layer */
	lower_dentry = lookup_one_len_unlocked(dentry->d_name.name, lower_parent_dentry, dentry->d_name.len);
	if (IS_ERR(lower_dentry)) {
		err = PTR_ERR(lower_dentry);
		goto out;
	}

	lower_path.dentry = lower_dentry;
	lower_path.mnt = parent_info->lower_paths[0].mnt;

	/* 4. Perform the actual creation */
	err = vfs_create(MIRAGE_IDMAP(&lower_path), d_inode(lower_parent_dentry), lower_dentry, mode, want_excl);
	if (err) {
		dput(lower_dentry);
		goto out;
	}

	/* 5. Update the virtual dentry with the newly created physical path */
	info = mirage_dentry(dentry);
	path_put(&info->lower_paths[0]);
	pathcpy(&info->lower_paths[0], &lower_path);
	path_get(&lower_path);
	info->num_lower_paths = 1;

	/* 6. Instantiate the new virtual inode */
	new_inode = mirage_iget(dir->i_sb, d_inode(lower_dentry));
	if (IS_ERR(new_inode)) {
		err = PTR_ERR(new_inode);
		goto out;
	}
	d_instantiate(dentry, new_inode);
	err = 0;

	/* Sync parent metadata */
	fsstack_copy_attr_times(dir, mirage_lower_inode(dir));
	fsstack_copy_inode_size(dir, d_inode(lower_parent_dentry));

out:
	inode_unlock(d_inode(lower_parent_dentry));
	dput(parent_dentry);
	return err;
}

static int mirage_vfs_unlink(struct inode *dir, struct dentry *dentry)
{
	int err;
	struct inode *lower_dir_inode = mirage_lower_inode(dir);
	struct dentry *lower_dir_dentry;
	struct mirage_dentry_info *info = rcu_dereference_raw(dentry->d_fsdata);

	struct dentry *lower_dentry = info->lower_paths[0].dentry;

	lower_dir_dentry = lock_parent(lower_dentry);

	err = vfs_unlink(MIRAGE_IDMAP(&info->lower_paths[0]), lower_dir_inode, lower_dentry, NULL);
	if (err) goto out;

	if (d_inode(dentry))
		set_nlink(d_inode(dentry), mirage_lower_inode(d_inode(dentry))->i_nlink);

	d_drop(dentry);
out:
	unlock_dir(lower_dir_dentry);
	return err;
}

static int mirage_vfs_mkdir(IDMAP_ARG, struct inode *dir, struct dentry *dentry, umode_t mode)
{
	int err;
	struct dentry *lower_dentry, *lower_parent_dentry;
	struct dentry *parent_dentry = dget_parent(dentry);
	struct mirage_dentry_info *parent_info = mirage_dentry(parent_dentry);
	struct mirage_dentry_info *info;
	struct inode *new_inode;
	struct path lower_path;
	
	if (!parent_info || !parent_info->lower_paths[0].dentry) {
		dput(parent_dentry);
		return -EINVAL;
	}

	lower_parent_dentry = parent_info->lower_paths[0].dentry;
	inode_lock_nested(d_inode(lower_parent_dentry), I_MUTEX_PARENT);

	lower_dentry = lookup_one_len_unlocked(dentry->d_name.name, lower_parent_dentry, dentry->d_name.len);
	if (IS_ERR(lower_dentry)) {
		err = PTR_ERR(lower_dentry);
		goto out;
	}

	lower_path.dentry = lower_dentry;
	lower_path.mnt = parent_info->lower_paths[0].mnt;

	err = vfs_mkdir(MIRAGE_IDMAP(&lower_path), d_inode(lower_parent_dentry), lower_dentry, mode);
	if (err) {
		dput(lower_dentry);
		goto out;
	}

	info = mirage_dentry(dentry);
	path_put(&info->lower_paths[0]); 
	pathcpy(&info->lower_paths[0], &lower_path);
	path_get(&lower_path);
	info->num_lower_paths = 1;

	new_inode = mirage_iget(dir->i_sb, d_inode(lower_dentry));
	if (IS_ERR(new_inode)) {
		err = PTR_ERR(new_inode);
		goto out;
	}
	d_instantiate(dentry, new_inode);
	err = 0;
	
	set_nlink(dir, mirage_lower_inode(dir)->i_nlink);

out:
	inode_unlock(d_inode(lower_parent_dentry));
	dput(parent_dentry);
	return err;
}

static int mirage_vfs_rmdir(struct inode *dir, struct dentry *dentry)
{
	int err;
	struct dentry *lower_dir_dentry;
	struct mirage_dentry_info *info = rcu_dereference_raw(dentry->d_fsdata);
	
	struct dentry *lower_dentry = info->lower_paths[0].dentry;

	lower_dir_dentry = lock_parent(lower_dentry);

	err = vfs_rmdir(MIRAGE_IDMAP(&info->lower_paths[0]), d_inode(lower_dir_dentry), lower_dentry);
	if (err) goto out;

	d_drop(dentry);
	if (d_inode(dentry))
		clear_nlink(d_inode(dentry));

	set_nlink(dir, d_inode(lower_dir_dentry)->i_nlink);

out:
	unlock_dir(lower_dir_dentry);
	return err;
}

static int mirage_vfs_symlink(IDMAP_ARG, struct inode *dir, struct dentry *dentry, const char *symname)
{
	int err;
	struct dentry *lower_dentry, *lower_parent_dentry;
	struct dentry *parent_dentry = dget_parent(dentry);
	struct mirage_dentry_info *parent_info = mirage_dentry(parent_dentry);
	struct mirage_dentry_info *info;
	struct inode *new_inode;
	struct path lower_path;

	if (!parent_info || !parent_info->lower_paths[0].dentry) {
		dput(parent_dentry);
		return -EINVAL;
	}
	
	lower_parent_dentry = parent_info->lower_paths[0].dentry;
	inode_lock_nested(d_inode(lower_parent_dentry), I_MUTEX_PARENT);

	lower_dentry = lookup_one_len_unlocked(dentry->d_name.name, lower_parent_dentry, dentry->d_name.len);
	if (IS_ERR(lower_dentry)) {
		err = PTR_ERR(lower_dentry);
		goto out;
	}

	lower_path.dentry = lower_dentry;
	lower_path.mnt = parent_info->lower_paths[0].mnt;

	err = vfs_symlink(MIRAGE_IDMAP(&lower_path), d_inode(lower_parent_dentry), lower_dentry, symname);
	if (err) {
		dput(lower_dentry);
		goto out;
	}

	info = mirage_dentry(dentry);
	path_put(&info->lower_paths[0]); 
	pathcpy(&info->lower_paths[0], &lower_path);
	path_get(&lower_path);
	info->num_lower_paths = 1;

	new_inode = mirage_iget(dir->i_sb, d_inode(lower_dentry));
	if (IS_ERR(new_inode)) {
		err = PTR_ERR(new_inode);
		goto out;
	}
	d_instantiate(dentry, new_inode);
	err = 0;

out:
	inode_unlock(d_inode(lower_parent_dentry));
	dput(parent_dentry);
	return err;
}

#ifdef RENAME_HAS_FLAGS
static int mirage_vfs_rename(IDMAP_ARG, struct inode *old_dir, struct dentry *old_dentry,
			  struct inode *new_dir, struct dentry *new_dentry,
			  unsigned int flags)
#else
static int mirage_vfs_rename(IDMAP_ARG, struct inode *old_dir, struct dentry *old_dentry,
			  struct inode *new_dir, struct dentry *new_dentry)
#endif
{
	int err = 0;
	struct dentry *lower_old_dir_dentry, *lower_new_dir_dentry;
	struct dentry *trap;

	struct mirage_dentry_info *old_info = rcu_dereference_raw(old_dentry->d_fsdata);
	struct mirage_dentry_info *new_info = rcu_dereference_raw(new_dentry->d_fsdata);

	struct dentry *lower_old_dentry = old_info->lower_paths[0].dentry;
	struct dentry *lower_new_dentry = new_info->lower_paths[0].dentry;

#ifdef RENAME_HAS_FLAGS
	/* * Allow RENAME_NOREPLACE. This is crucial for Android SQLite 
	 * databases (like WhatsApp or Contacts) to ensure atomic writes.
	 * We only reject exotic flags like RENAME_EXCHANGE which
	 * require complex VFS swapping logic not needed here.
	 */
	if (flags & ~(RENAME_NOREPLACE)) 
		return -EINVAL; 
#endif

	lower_old_dir_dentry = dget_parent(lower_old_dentry);
	lower_new_dir_dentry = dget_parent(lower_new_dentry);

	/* Strict locking to prevent deadlocks when moving folders */
	trap = lock_rename(lower_old_dir_dentry, lower_new_dir_dentry);
	if (trap == lower_old_dentry) { err = -EINVAL; goto out; }
	if (trap == lower_new_dentry) { err = -ENOTEMPTY; goto out; }

#ifdef RENAME_HAS_FLAGS

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 12, 0)
    struct renamedata rd = {
        .old_mnt_idmap = MIRAGE_IDMAP(&old_info->lower_paths[0]),
        .old_dir       = d_inode(lower_old_dir_dentry),
        .old_dentry    = lower_old_dentry,
        .new_mnt_idmap = MIRAGE_IDMAP(&new_info->lower_paths[0]),
        .new_dir       = d_inode(lower_new_dir_dentry),
        .new_dentry    = lower_new_dentry,
        .delegated_inode = NULL,
        .flags         = flags,
    };
    err = vfs_rename(&rd);
#else
	/* Pass the flags down to the physical filesystem layer */
	err = vfs_rename(d_inode(lower_old_dir_dentry), lower_old_dentry,
			 d_inode(lower_new_dir_dentry), lower_new_dentry, NULL, flags);
#endif // LINUX_VERSION_CODE >= KERNEL_VERSION(5, 12, 0)

#else
	err = vfs_rename(d_inode(lower_old_dir_dentry), lower_old_dentry,
			 d_inode(lower_new_dir_dentry), lower_new_dentry);
#endif // RENAME_HAS_FLAGS

	if (err) goto out;

out:
	unlock_rename(lower_old_dir_dentry, lower_new_dir_dentry);
	dput(lower_old_dir_dentry);
	dput(lower_new_dir_dentry);
	return err;
}

static int mirage_vfs_getattr(IDMAP_ARG, const struct path *path, struct kstat *stat, u32 request_mask, unsigned int query_flags)
{
	struct mirage_dentry_info *info = rcu_dereference_raw(path->dentry->d_fsdata);
	struct path *lower_path = &info->lower_paths[0];
	struct inode *lower_inode = d_inode(lower_path->dentry);
	int err;

	if (likely(lower_inode->i_op->getattr)) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
		err = lower_inode->i_op->getattr(MIRAGE_IDMAP(lower_path), lower_path, stat, request_mask, query_flags);
#else
		err = lower_inode->i_op->getattr(MIRAGE_IDMAP(lower_path), lower_path, stat, request_mask, query_flags);
#endif
	} else {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
		generic_fillattr(MIRAGE_IDMAP(lower_path), request_mask, lower_inode, stat);
#else
		generic_fillattr(MIRAGE_IDMAP(lower_path), lower_inode, stat);
#endif
		err = 0;
	}

	if (likely(!err))
		stat->dev = path->dentry->d_sb->s_dev;

	return err;
}

static int mirage_vfs_setattr(IDMAP_ARG, struct dentry *dentry, struct iattr *ia)
{
	int err;
	struct inode *inode = d_inode(dentry);
	struct inode *lower_inode = mirage_lower_inode(inode);
	struct mirage_dentry_info *info = rcu_dereference_raw(dentry->d_fsdata);
	struct dentry *lower_dentry = info->lower_paths[0].dentry;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
    err = setattr_prepare(IDMAP_CALL, dentry, ia);
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(5, 12, 0)
    err = setattr_prepare(&init_user_ns, dentry, ia);
#else
    err = setattr_prepare(dentry, ia);
#endif
	if (err) return err;

	inode_lock(lower_inode);
	err = notify_change(MIRAGE_IDMAP(&info->lower_paths[0]), lower_dentry, ia, NULL);
	inode_unlock(lower_inode);

	if (!err && (ia->ia_valid & ATTR_SIZE))
		truncate_setsize(inode, ia->ia_size);

	return err;
}

/* --- XATTR Handling --- */
static ssize_t mirage_vfs_listxattr(struct dentry *dentry, char *buffer, size_t buffer_size)
{
	struct mirage_dentry_info *info;
	struct dentry *ld;
	ssize_t err = -EOPNOTSUPP;

	info = rcu_dereference_raw(dentry->d_fsdata);
	if (unlikely(!info || !info->lower_paths[0].dentry))
		return err;

	ld = info->lower_paths[0].dentry;

	if (likely(d_is_positive(ld) && (d_inode(ld)->i_opflags & IOP_XATTR)))
		err = vfs_listxattr(ld, buffer, buffer_size);

	return err;
}

/* --- Handlers XATTRs Directos --- */
static int mirage_xattr_get(const struct xattr_handler *handler,
			    struct dentry *dentry, struct inode *inode,
			    const char *name, void *buffer, size_t size)
{
	struct mirage_dentry_info *info = rcu_dereference_raw(dentry->d_fsdata);
	struct dentry *ld = info->lower_paths[0].dentry;

	if (likely(d_is_positive(ld) && (d_inode(ld)->i_opflags & IOP_XATTR)))
		return vfs_getxattr(MIRAGE_IDMAP(&info->lower_paths[0]), ld, name, buffer, size);
	
	return -EOPNOTSUPP;
}

static int mirage_xattr_set(const struct xattr_handler *handler,
			    IDMAP_ARG, struct dentry *dentry, struct inode *inode,
			    const char *name, const void *value, size_t size, int flags)
{
	struct mirage_dentry_info *info = rcu_dereference_raw(dentry->d_fsdata);
	struct dentry *ld = info->lower_paths[0].dentry;

	if (unlikely(!d_is_positive(ld) || !(d_inode(ld)->i_opflags & IOP_XATTR)))
		return -EOPNOTSUPP;

	if (value)
		return vfs_setxattr(MIRAGE_IDMAP(&info->lower_paths[0]), ld, name, value, size, flags);

	BUG_ON(flags != XATTR_REPLACE);
	return vfs_removexattr(MIRAGE_IDMAP(&info->lower_paths[0]), ld, name);
}

const struct xattr_handler mirage_xattr_handler = {
	.prefix = "",		/* Match any attribute string */
	.get = mirage_xattr_get,
	.set = mirage_xattr_set,
};

const struct xattr_handler *mirage_xattr_handlers[] = {
	&mirage_xattr_handler,
	NULL
};

/* --- Symlink Management --- */

#ifdef LEGACY_FOLLOW_LINK
static void *mirage_vfs_follow_link(struct dentry *dentry, struct nameidata *nd)
{
	struct inode *lower_inode = mirage_lower_inode(d_inode(dentry));
	if (lower_inode->i_op->follow_link)
		return lower_inode->i_op->follow_link(dentry, nd);
	return ERR_PTR(-EINVAL);
}
#else
static const char *mirage_vfs_get_link(struct dentry *dentry,
				   struct inode *inode,
				   struct delayed_call *done)
{
	struct mirage_dentry_info *info;

	/* RCU-walk mode (dentry is NULL) - Delegate directly to lower inode */
	if (unlikely(!dentry)) {
		struct inode *lower_inode = mirage_lower_inode(inode);
		if (!lower_inode->i_op->get_link)
			return ERR_PTR(-ECHILD);
		return lower_inode->i_op->get_link(NULL, lower_inode, done);
	}

	/* Standard Ref-walk mode */
	info = rcu_dereference_raw(dentry->d_fsdata);
	if (unlikely(!info || !info->lower_paths[0].dentry))
		return ERR_PTR(-ECHILD);

	return vfs_get_link(info->lower_paths[0].dentry, done);
}
#endif

static int mirage_permission(IDMAP_ARG, struct inode *inode, int mask)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
	return inode_permission(&nop_mnt_idmap, mirage_lower_inode(inode), mask);
#else
	return inode_permission(IDMAP_CALL, mirage_lower_inode(inode), mask);
#endif
}

/* --- Operation Vectors --- */

const struct inode_operations mirage_dir_iops = {
	.getattr    = mirage_vfs_getattr,
	.create		= mirage_vfs_create,
	.lookup		= mirage_vfs_lookup,
	.unlink		= mirage_vfs_unlink,
	.mkdir		= mirage_vfs_mkdir,
	.rmdir		= mirage_vfs_rmdir,
	.symlink    = mirage_vfs_symlink,
	.rename		= mirage_vfs_rename,
	.permission	= mirage_permission,
	.setattr    = mirage_vfs_setattr,
	.listxattr	= mirage_vfs_listxattr,
};

const struct inode_operations mirage_main_iops = {
	.getattr    = mirage_vfs_getattr,
	.permission	= mirage_permission,
	.setattr    = mirage_vfs_setattr,
	.listxattr	= mirage_vfs_listxattr,
};

const struct inode_operations mirage_symlink_iops = {
	.getattr     = mirage_vfs_getattr,
#ifdef LEGACY_FOLLOW_LINK
	.follow_link = mirage_vfs_follow_link,
#else
	.get_link	 = mirage_vfs_get_link,
#endif
	.permission	 = mirage_permission,
	.setattr     = mirage_vfs_setattr,
	.listxattr	 = mirage_vfs_listxattr,
};
