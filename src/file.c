/*
 * Mirage: File operations
 * Handling read, write and open.
 */

struct kmem_cache *mirage_file_cachep;

/* * mirage_vfs_open: called when a file is being opened.
 * We must open the lower file and store its reference.
 */
static int mirage_vfs_open(struct inode *inode, struct file *file)
{
	int err = 0;
	struct file *lower_file = NULL;
	struct path lower_paths[MIRAGE_MAX_BRANCHES];
	int num_lower_paths;
	struct mirage_file_info *info;
	int i;

	/* Allocate private storage for lower file reference */
	info = kmem_cache_alloc(mirage_file_cachep, GFP_KERNEL);
	if (unlikely(!info))
		return -ENOMEM;

	info->num_lower_files = 0;

	if (!S_ISDIR(inode->i_mode)) {
		get_lower_path(file->f_path.dentry, &lower_paths[0]);
		num_lower_paths = lower_paths[0].dentry ? 1 : 0;
	} else {
		num_lower_paths = get_all_lower_paths(file->f_path.dentry, lower_paths);
	}
	
	for (i = 0; i < num_lower_paths; i++) {
		lower_file = dentry_open(&lower_paths[i], file->f_flags, current_cred());
		if (IS_ERR(lower_file)) {
			err = PTR_ERR(lower_file);
			break; /* If one fails, stop and cleanup */
		}
		info->lower_files[info->num_lower_files++] = lower_file;
		
		/* If it's not a directory, we only open the topmost one */
		if (!S_ISDIR(inode->i_mode))
			break;
	}

	if (!S_ISDIR(inode->i_mode)) {
		if (lower_paths[0].dentry)
			put_lower_path(file->f_path.dentry, &lower_paths[0]);
	} else {
		put_all_lower_paths(file->f_path.dentry, lower_paths, num_lower_paths);
	}

	if (err && info->num_lower_files == 0) {
		kmem_cache_free(mirage_file_cachep, info);
		return err;
	} else if (err) {
		/* Failed partway through opening directories; close what we opened */
		for (i = 0; i < info->num_lower_files; i++) {
			fput(info->lower_files[i]);
		}
		kmem_cache_free(mirage_file_cachep, info);
		return err;
	} else if (info->num_lower_files == 0) {
		/* No files were opened — this is an error */
		kmem_cache_free(mirage_file_cachep, info);
		return -ENOENT;
	} else {
		file->private_data = info;
	}
	return 0;
}

/* * mirage_vfs_release: close the lower file when the virtual one is closed.
 */
static int mirage_vfs_release(struct inode *inode, struct file *file)
{
	struct mirage_file_info *info = mirage_file(file);
	int i;

	if (info) {
		for (i = 0; i < info->num_lower_files; i++) {
			if (info->lower_files[i]) {
				fput(info->lower_files[i]);
			}
		}
		
		kmem_cache_free(mirage_file_cachep, info);
		file->private_data = NULL;
	}
	return 0;
}

/*
 * mirage_vfs_mmap: Delegate memory mapping directly to the physical disk.
 *
 * Instead of intercepting page faults (which requires dangerous VMA 
 * cloning/memcpy and breaks on modern kernels), we completely hand over 
 * the VMA ownership to the physical underlying file. 
 */
static int mirage_vfs_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct file *lower_file = mirage_lower_file(file);

	if (!lower_file->f_op->mmap)
		return -ENODEV;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 1, 0)
	vma_set_file(vma, lower_file);
#else
	if (vma->vm_file)
		fput(vma->vm_file);
	vma->vm_file = get_file(lower_file);
#endif

	/* Call the lower filesystem's mmap natively */
	return lower_file->f_op->mmap(lower_file, vma);
}

/* * IOCTL: Critical for hardware-specific storage commands in Android.
 */
static long mirage_unlocked_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct file *lower_file = mirage_lower_file(file);
	long err = -ENOTTY;

	if (lower_file->f_op && lower_file->f_op->unlocked_ioctl)
		err = lower_file->f_op->unlocked_ioctl(lower_file, cmd, arg);

	return err;
}

#ifdef CONFIG_COMPAT
/* * compat_ioctl: Handles 32-bit ioctl calls on 64-bit kernels.
 * Essential for Android compatibility with older apps/libraries.
 */
static long mirage_compat_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	long err = -ENOTTY;
	struct file *lower_file = mirage_lower_file(file);

	/* Redirect the call to the lower filesystem's compat_ioctl */
	if (lower_file->f_op && lower_file->f_op->compat_ioctl)
		err = lower_file->f_op->compat_ioctl(lower_file, cmd, arg);

	return err;
}
#endif

static int mirage_vfs_fsync(struct file *file, loff_t start, loff_t end, int datasync)
{
	return vfs_fsync_range(mirage_lower_file(file), start, end, datasync);
}

static int mirage_vfs_flush(struct file *file, fl_owner_t id)
{
	struct file *lower_file = mirage_lower_file(file);
	if (lower_file && lower_file->f_op && lower_file->f_op->flush)
		return lower_file->f_op->flush(lower_file, id);
	return 0;
}

static int mirage_vfs_fasync(int fd, struct file *file, int flag)
{
	struct file *lower_file = mirage_lower_file(file);
	if (lower_file->f_op && lower_file->f_op->fasync)
		return lower_file->f_op->fasync(fd, lower_file, flag);
	return 0;
}

#ifdef HAVE_LEGACY_IO
/* * mirage_vfs_read: The old read for Kernel < 3.16.
 */
static ssize_t mirage_vfs_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
	return vfs_read(mirage_lower_file(file), buf, count, ppos);
}

static ssize_t mirage_vfs_write(struct file *file, const char __user *buf, size_t count, loff_t *ppos)
{
	/* Forward write to the lower filesystem */
	return vfs_write(mirage_lower_file(file), buf, count, ppos);
}
#else
/* * mirage_vfs_read_iter: modern read interface.
 */
static ssize_t mirage_vfs_read_iter(struct kiocb *iocb, struct iov_iter *iter)
{
	ssize_t err;
	struct file *file = iocb->ki_filp;
	struct file *lower_file = mirage_lower_file(file);
	struct file *saved_filp;

	if (unlikely(!lower_file->f_op->read_iter))
		return -EINVAL;

	/* Temporarily switch the control block's file context */
	saved_filp = iocb->ki_filp;
	iocb->ki_filp = lower_file;
	
	err = lower_file->f_op->read_iter(iocb, iter);
	
	/* Restore the original file context */
	iocb->ki_filp = saved_filp;

	/* Metadata sync removed */
	return err;
}

static ssize_t mirage_vfs_write_iter(struct kiocb *iocb, struct iov_iter *iter)
{
	ssize_t err;
	struct file *file = iocb->ki_filp;
	struct file *lower_file = mirage_lower_file(file);
	struct file *saved_filp;

	if (unlikely(!lower_file->f_op->write_iter))
		return -EINVAL;

	/* Temporarily switch the control block's file context */
	saved_filp = iocb->ki_filp;
	iocb->ki_filp = lower_file;

	err = lower_file->f_op->write_iter(iocb, iter);
	
	/* Restore the original file context */
	iocb->ki_filp = saved_filp;

	/* * The Linux VFS naturally handles the i_size update of the upper 
	 * virtual inode during write calls. Manual fsstack_copy is redundant.
	 */
	return err;
}
#endif

/* Embedded context proxy to bypass memory footprint during readdir */
struct mirage_proxy_ctx {
    struct dir_context ctx;          /* Fake context passed to lower layers */
    struct dir_context *orig_ctx;    /* Original user space context */
    struct mirage_sb_info *sbi;      /* Global superblock info for injections */
    struct mirage_file_info *mfi;    /* File context to access sibling layer file structures */
    int current_branch;              /* Index of the lower layer being currently read */
    bool ghost_found;                /* Tracks if the ghost file was shadowed by a real one */
};

/* The on-the-fly filter callback. Executes for EVERY entry discovered on disk */
static MIR_ACTOR_RET mirage_proxy_actor(struct dir_context *ctx, const char *name, int namelen,
                                        loff_t offset, u64 ino, unsigned int d_type)
{
    struct mirage_proxy_ctx *proxy = container_of(ctx, struct mirage_proxy_ctx, ctx);
    struct mirage_sb_info *sbi = proxy->sbi;
    struct mirage_file_info *mfi = proxy->mfi;
    int i;

    /* 1. Explicit file injection/spoofing handler */
    if (unlikely(sbi->has_inject && 
                 namelen == sbi->inject_name_len && 
                 memcmp(name, sbi->inject_name, namelen) == 0)) {
        
        ino = d_inode(sbi->inject_path.dentry)->i_ino;
        d_type = DT_REG;
        proxy->ghost_found = true;
    }

    /* 2. Union Mount Deduplication 
     * Scan all upper branches (from 0 up to current_branch - 1).
     * If the filename already exists in a higher layer, it has been emitted.
     */
    for (i = 0; i < proxy->current_branch; i++) {
        struct dentry *upper_dir = mfi->lower_files[i]->f_path.dentry;
        struct dentry *child;

        /* Query the kernel dcache directly without hitting physical disk */
        child = lookup_one_len_unlocked(name, upper_dir, namelen);
        if (!IS_ERR(child)) {
            if (d_really_is_positive(child)) {
                /* File exists in a higher layer; discard lower duplicate entry */
                dput(child);
                return MIR_ACTOR_CONTINUE;
            }
            dput(child);
        }
    }

    /* 3. Safe pass-through directly to the real user space buffer */
    return proxy->orig_ctx->actor(proxy->orig_ctx, name, namelen, offset, ino, d_type);
}

#if defined(HAVE_ITERATE_SHARED) || defined(HAVE_ITERATE)
static int mirage_vfs_iterate(struct file *file, struct dir_context *ctx)
{
    struct mirage_sb_info *sbi = mirage_sb(file->f_inode->i_sb);
    struct mirage_file_info *mfi = mirage_file(file);
    bool is_root = (file->f_path.dentry == file->f_inode->i_sb->s_root);
    loff_t magic_pos = 0x7000000000000000ULL;
    int i, err = 0;

    /* Allocate the proxy context directly inside the CPU stack */
    struct mirage_proxy_ctx proxy_ctx = {
        .ctx.actor = mirage_proxy_actor,
        .ctx.pos = ctx->pos,
        .orig_ctx = ctx,
        .sbi = sbi,
        .mfi = mfi,
        .current_branch = 0,
        .ghost_found = false
    };

    /* Phase 1: Stream and merge entries from all active filesystem layers */
    if (ctx->pos < magic_pos) {
        for (i = 0; i < mfi->num_lower_files; i++) {
            struct file *lower_file = mfi->lower_files[i];

            proxy_ctx.current_branch = i;
            lower_file->f_pos = proxy_ctx.ctx.pos;

#ifdef HAVE_ITERATE_SHARED
            err = iterate_dir(lower_file, &proxy_ctx.ctx);
#else
            if (!lower_file->f_op->iterate) return -ENOTDIR;
            err = lower_file->f_op->iterate(lower_file, &proxy_ctx.ctx);
#endif
            proxy_ctx.ctx.pos = lower_file->f_pos;
            if (err < 0) break;
        }
        
        ctx->pos = proxy_ctx.ctx.pos;
    }

    /* Phase 2: Synthetic ghost file append (Only if it wasn't shadowed) */
    if (err >= 0 && is_root && sbi->has_inject) {
        if (!proxy_ctx.ghost_found && ctx->pos <= magic_pos) {
            u64 fake_ino = d_inode(sbi->inject_path.dentry)->i_ino;
            ctx->pos = magic_pos;
            if (dir_emit(ctx, sbi->inject_name, sbi->inject_name_len, fake_ino, DT_REG)) {
                ctx->pos = magic_pos + 1;
            }
        }
    }

    file->f_pos = ctx->pos;
    return err;
}
#else
/* * Ultra-Legacy kernels ( < 3.11) use the old readdir.
 */
static int mirage_vfs_readdir(struct file *file, void *dirent, filldir_t filldir)
{
		struct file *lower_file = mirage_lower_file(file);
		struct mirage_sb_info *sbi = mirage_sb(file->f_inode->i_sb);
		loff_t magic_pos = 0x7000000000000000ULL;
		int err;

		if (!lower_file->f_op->readdir)
				return -ENOTDIR;

		if (file->f_pos < magic_pos) {
				err = lower_file->f_op->readdir(lower_file, dirent, filldir);
				file->f_pos = lower_file->f_pos;
		} else {
				err = 0;
		}

		if (err >= 0 && (file->f_path.dentry == file->f_inode->i_sb->s_root) &&
				 sbi->has_inject && file->f_pos <= magic_pos) {

				u64 fake_ino = d_inode(sbi->inject_path.dentry)->i_ino;
				file->f_pos = magic_pos;
				if (filldir(dirent, sbi->inject_name, sbi->inject_name_len, file->f_pos, fake_ino, DT_REG) >= 0)
						file->f_pos = magic_pos + 1;
		}
		return err;
}
#endif

/* File operations for regular files */
const struct file_operations mirage_main_fops = {
    .open           = mirage_vfs_open,
    .release        = mirage_vfs_release,
    .llseek         = generic_file_llseek,
    .mmap           = mirage_vfs_mmap,
    .unlocked_ioctl = mirage_unlocked_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl   = mirage_compat_ioctl,
#endif
    .fsync          = mirage_vfs_fsync,
    .flush          = mirage_vfs_flush,
    .fasync         = mirage_vfs_fasync,
#ifdef HAVE_LEGACY_IO
    .read           = mirage_vfs_read,
    .write          = mirage_vfs_write,
#else
    .read_iter      = mirage_vfs_read_iter,
    .write_iter     = mirage_vfs_write_iter,
#endif
};

const struct file_operations mirage_dir_fops = {
    .open           = mirage_vfs_open,
    .release        = mirage_vfs_release,
    .llseek         = generic_file_llseek,
    .unlocked_ioctl = mirage_unlocked_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl   = mirage_compat_ioctl,
#endif
    .fsync          = mirage_vfs_fsync,
    .flush          = mirage_vfs_flush,
    .fasync         = mirage_vfs_fasync,
#ifdef HAVE_ITERATE_SHARED
    .iterate_shared = mirage_vfs_iterate,
#elif defined(HAVE_ITERATE)
    .iterate        = mirage_vfs_iterate,
#else
	.readdir	    = mirage_vfs_readdir,
#endif
};

static int mirage_init_file_cache(void)
{
    mirage_file_cachep = kmem_cache_create("mirage_file_cache",
                                           sizeof(struct mirage_file_info), 0,
                                           SLAB_RECLAIM_ACCOUNT | SLAB_HWCACHE_ALIGN, NULL);
    return mirage_file_cachep ? 0 : -ENOMEM;
}

static void mirage_destroy_file_cache(void)
{
    if (mirage_file_cachep)
        kmem_cache_destroy(mirage_file_cachep);
}

