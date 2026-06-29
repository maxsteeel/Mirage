/*
 * Mirage: Compatibility layer for different Linux Kernel versions.
 * This ensures that Mirage can be compiled on Android kernels from 3.4 to 6.x.
 */

#ifndef _MIRAGE_COMPAT_H_
#define _MIRAGE_COMPAT_H_

#ifndef TWA_RESUME
#define TWA_RESUME true
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0)
    #define MIR_ACTOR_RET bool
    #define MIR_ACTOR_CONTINUE true
#else
    #define MIR_ACTOR_RET int
    #define MIR_ACTOR_CONTINUE 0
#endif

/*
 * Inode locking: i_mutex (old) vs i_rwsem (new).
 * Kernel 4.5 renamed i_mutex and introduced inode_lock() helper.
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 5, 0)
    #define inode_lock(inode) mutex_lock(&(inode)->i_mutex)
    #define inode_unlock(inode) mutex_unlock(&(inode)->i_mutex)
    #define inode_lock_nested(inode, subclass) mutex_lock_nested(&(inode)->i_mutex, (subclass))
#endif

/*
 * Dentry alias list handling.
 * Kernel 3.11 moved d_alias into the d_u union.
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 11, 0)
    #define d_alias_list d_alias
#else
    #define d_alias_list d_u.d_alias
#endif

/*
 * Directory iteration: iterate_shared (new) vs readdir (old).
 * Introduced in Kernel 4.7.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 7, 0)
    #define HAVE_ITERATE_SHARED
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(3, 11, 0)
    #define HAVE_ITERATE
#endif

/*
 * Symlink handling: get_link (new) vs follow_link (old).
 * The prototype changed significantly in Kernel 4.5.
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 5, 0)
    #define LEGACY_FOLLOW_LINK
#endif

/*
 * File I/O: read_iter (new) vs aio_read/read (old).
 * read_iter was standardized around 3.16/3.19.
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 16, 0)
    #define HAVE_LEGACY_IO
#endif

/*
 * Rename signature handling.
 * Kernel 4.9+ unified rename2 into rename and added flags.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 9, 0)
    #define RENAME_HAS_FLAGS
#endif

/* ID Map handling for Modern Kernels (6.3+) */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
    #define MIRAGE_IDMAP(path) mnt_idmap((path)->mnt)
    #define IDMAP_ARG struct mnt_idmap *idmap
    #define IDMAP_CALL idmap
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(5, 12, 0)
    #define MIRAGE_IDMAP(path) mnt_user_ns((path)->mnt)
    #define IDMAP_ARG struct user_namespace *mnt_userns
    #define IDMAP_CALL mnt_userns
#else
    #define MIRAGE_IDMAP(path) /* Nothing */
    #define IDMAP_ARG /* Nothing */
    #define IDMAP_CALL /* Nothing */
#endif

/* d_real signature changed in Kernel 6.5 from passing an inode to passing a type enum */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 5, 0)
    #define D_REAL_ARGS enum d_real_type type
    #define D_REAL_PASS type
#else
    #define D_REAL_ARGS const struct inode *inode
    #define D_REAL_PASS inode
#endif

/*
 * String copying safe fallback.
 * strscpy was introduced in 4.3. Older kernels must use strlcpy.
 * Modern kernels (6.8+) removed strlcpy entirely.
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 3, 0)
    #ifndef strscpy
        #define strscpy strlcpy
    #endif
#endif

/*
 * d_really_is_positive/negative helpers.
 * These were introduced to handle RCU-walks in newer kernels.
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 18, 0)
    static inline bool d_really_is_positive(const struct dentry *dentry)
    {
        return dentry->d_inode != NULL;
    }
    static inline bool d_really_is_negative(const struct dentry *dentry)
    {
        return dentry->d_inode == NULL;
    }
#endif

/*
 * mirage_clone_private_mount: clone a vfsmount privately so it is
 * detached from the mount namespace.
 *
 * A private clone is not visible to follow_mount/lookup_mnt in the
 * public namespace, so dentry_open through it always reaches the real
 * underlying filesystem directly — even when Mirage is mounted over
 * the same path as one of its lower layers.
 *
 * clone_private_mount() was added in 3.18 but is not always exported
 * to out-of-tree modules on Android kernels.
 *
 * Returns ERR_PTR on error, never NULL.
 */
static inline struct vfsmount *mirage_clone_private_mount(struct path *path)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 18, 0) && !defined(NO_CLONE_PRIVATE_MOUNT)
	struct vfsmount *mnt = clone_private_mount(path);
#else
	struct vfsmount *mnt = mntget(path->mnt);
#endif
	return mnt ? mnt : ERR_PTR(-ENOMEM);
}

#endif /* _MIRAGE_COMPAT_H_ */
