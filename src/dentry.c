/*
 * Mirage: Dentry operations
 * Managing path name cache. Lockless and Immutable design.
 */

struct kmem_cache *mirage_dentry_cachep;

/* Helper to allocate private dentry data */
static int new_dentry_private_data(struct dentry *dentry)
{
	struct mirage_dentry_info *info;

	info = kmem_cache_zalloc(mirage_dentry_cachep, GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	dentry->d_fsdata = info;
	return 0;
}

static void free_dentry_private_data(struct dentry *dentry)
{
  struct mirage_dentry_info *info = mirage_dentry(dentry);
	int i;

    if (likely(info)) {
		for (i = 0; i < info->num_lower_paths; i++) {
			if (info->lower_paths[i].dentry) {
				path_put(&info->lower_paths[i]);
				info->lower_paths[i].dentry = NULL;
			}
		}

		spin_lock(&dentry->d_lock);
		dentry->d_fsdata = NULL;
		spin_unlock(&dentry->d_lock);
		kmem_cache_free(mirage_dentry_cachep, info);
    }
}

/* mirage_vfs_d_revalidate: checks if our cached dentry is still valid 
 * compared to the real one on disk.
 */
static int mirage_vfs_d_revalidate(struct dentry *dentry, unsigned int flags)
{
	struct dentry *lower_dentry;
	struct mirage_dentry_info *info;
	int valid = 1;

	info = mirage_dentry(dentry);
	if (unlikely(!info || !info->lower_paths[0].dentry)) {
		valid = -ECHILD;
		goto out_drop;
	}

	lower_dentry = info->lower_paths[0].dentry;
	if (lower_dentry->d_op && lower_dentry->d_op->d_revalidate) {
		valid = lower_dentry->d_op->d_revalidate(lower_dentry, flags);
		if (unlikely(valid <= 0))
			goto out_drop;
	}

	if (likely(d_really_is_positive(dentry))) {
		struct inode *inode = d_inode_rcu(dentry);
		struct inode *lower_inode = d_inode_rcu(lower_dentry);
		if (unlikely(!inode || !lower_inode || inode->i_ino != lower_inode->i_ino))
			valid = 0;
	}

out_drop:
	if (valid == 0)
		d_drop(dentry);

	return valid;
}

/* mirage_vfs_d_release: cleanup when a dentry is destroyed.
 */
static void mirage_vfs_d_release(struct dentry *dentry)
{
	free_dentry_private_data(dentry);
}

static struct dentry *mirage_vfs_d_real(struct dentry *dentry, D_REAL_ARGS)
{
	struct dentry *lower_dentry;
	struct mirage_dentry_info *info;

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 5, 0)
    if (likely(inode && d_inode(dentry) == inode))
        return dentry;
#endif

	info = mirage_dentry(dentry);
	if (unlikely(!info || !info->lower_paths[0].dentry))
		return dentry;

	lower_dentry = info->lower_paths[0].dentry;
	if (lower_dentry->d_op && lower_dentry->d_op->d_real)
		return lower_dentry->d_op->d_real(lower_dentry, D_REAL_PASS);

	return lower_dentry;
}

/* mirage_vfs_d_delete: Decides if a dentry should be cached when its refcount hits 0.
 */
static int mirage_vfs_d_delete(const struct dentry *dentry)
{
	struct mirage_dentry_info *info;
	struct dentry *lower_dentry;
	int err = 0;

	info = mirage_dentry(dentry);
	if (unlikely(!info))
		return 1;

	lower_dentry = info->lower_paths[0].dentry;

	if (likely(lower_dentry)) {
		if (unlikely(d_unhashed(lower_dentry)))
			err = 1;
		else if (lower_dentry->d_op && lower_dentry->d_op->d_delete)
			err = lower_dentry->d_op->d_delete(lower_dentry);
	} else {
		err = 1;
	}

	return err;
}

/* Dentry operations vector */
const struct dentry_operations mirage_dops = {
	.d_revalidate	= mirage_vfs_d_revalidate,
	.d_release		= mirage_vfs_d_release,
	.d_real			= mirage_vfs_d_real,
	.d_delete	    = mirage_vfs_d_delete, 
};

/* --- Dentry Cache Initialization --- */

static int mirage_init_dentry_cache(void)
{
	mirage_dentry_cachep = kmem_cache_create("mirage_dentry_cache",
											  sizeof(struct mirage_dentry_info),
											  0, SLAB_RECLAIM_ACCOUNT | SLAB_HWCACHE_ALIGN, NULL);
	return mirage_dentry_cachep ? 0 : -ENOMEM;
}

static void mirage_destroy_dentry_cache(void)
{
	if (mirage_dentry_cachep)
		kmem_cache_destroy(mirage_dentry_cachep);
}
