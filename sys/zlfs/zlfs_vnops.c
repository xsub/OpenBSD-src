/*	$OpenBSD$	*/

/*
 * ZLFS vnode operations.
 *
 * Read-only bring-up: the only vnode is the synthetic empty root
 * directory created by zlfs_vget().  Directory reads emit "." and
 * ".."; everything that would modify the filesystem fails.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/buf.h>
#include <sys/dirent.h>
#include <sys/endian.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/namei.h>
#include <sys/proc.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/unistd.h>
#include <sys/vnode.h>
#include <sys/zlfs.h>

#include <zlfs/zlfs_var.h>

int	zlfs_lookup(void *);
int	zlfs_access(void *);
int	zlfs_getattr(void *);
int	zlfs_setattr(void *);
int	zlfs_read(void *);
int	zlfs_readdir(void *);
int	zlfs_inactive(void *);
int	zlfs_reclaim(void *);
int	zlfs_lock(void *);
int	zlfs_unlock(void *);
int	zlfs_islocked(void *);
int	zlfs_pathconf(void *);
int	zlfs_print(void *);

/*
 * Search directory dvp for a name, returning the matching inode number
 * in *inop.  Returns 0 on hit, ENOENT on miss, or an I/O error.
 */
static int
zlfs_dir_lookup(struct vnode *dvp, const char *name, int namlen,
    u_int64_t *inop)
{
	struct zlfs_node *dnp = VTOZ(dvp);
	struct zlfs_mount *zmp = dnp->zn_zmp;
	struct zlfs_inode *dzi = &dnp->zn_dinode;
	struct zlfs_dirent *zd;
	struct buf *bp;
	u_int64_t off, blkno, dblk = (u_int64_t)-1;
	u_int32_t bsize = zmp->zm_super.zs_block_size;
	int error, slot;

	if (namlen > ZLFS_MAXNAMLEN)
		return ENOENT;

	bp = NULL;
	for (off = 0; off + ZLFS_DIRENT_SIZE <= dzi->zi_size;
	    off += ZLFS_DIRENT_SIZE) {
		blkno = off / bsize;
		if (blkno >= ZLFS_NDADDR || dzi->zi_db[blkno] == 0)
			break;
		if (blkno != dblk) {
			if (bp != NULL)
				brelse(bp);
			error = zlfs_bread_block(zmp, dzi->zi_db[blkno], &bp);
			if (error != 0)
				return error;
			dblk = blkno;
		}
		slot = (off % bsize) / ZLFS_DIRENT_SIZE;
		zd = (struct zlfs_dirent *)((u_int8_t *)bp->b_data +
		    slot * ZLFS_DIRENT_SIZE);
		if (zd->zd_ino == 0)
			continue;
		if (zd->zd_namlen == namlen &&
		    memcmp(zd->zd_name, name, namlen) == 0) {
			*inop = letoh64(zd->zd_ino);
			brelse(bp);
			return 0;
		}
	}
	if (bp != NULL)
		brelse(bp);
	return ENOENT;
}

int
zlfs_lookup(void *v)
{
	struct vop_lookup_args *ap = v;
	struct componentname *cnp = ap->a_cnp;
	struct vnode *dvp = ap->a_dvp;
	struct zlfs_mount *zmp = VTOZ(dvp)->zn_zmp;
	u_int64_t ino;
	int error;

	*ap->a_vpp = NULL;
	if (dvp->v_type != VDIR)
		return ENOTDIR;

	/* VOP_LOOKUP must enforce search (execute) permission itself. */
	error = VOP_ACCESS(dvp, VEXEC, cnp->cn_cred, cnp->cn_proc);
	if (error != 0)
		return error;

	if (cnp->cn_namelen == 1 && cnp->cn_nameptr[0] == '.') {
		vref(dvp);
		*ap->a_vpp = dvp;
		return 0;
	}
	if (cnp->cn_flags & ISDOTDOT) {
		/*
		 * The only directory is the root, whose parent is itself;
		 * the VFS layer intercepts ".." at the mount root anyway.
		 */
		vref(dvp);
		*ap->a_vpp = dvp;
		return 0;
	}

	error = zlfs_dir_lookup(dvp, cnp->cn_nameptr, cnp->cn_namelen, &ino);
	if (error == ENOENT) {
		/* A create/rename of a missing name fails read-only. */
		if ((cnp->cn_nameiop == CREATE || cnp->cn_nameiop == RENAME) &&
		    (cnp->cn_flags & ISLASTCN))
			return EROFS;
		return ENOENT;
	}
	if (error != 0)
		return error;

	if ((cnp->cn_nameiop == DELETE || cnp->cn_nameiop == RENAME) &&
	    (cnp->cn_flags & ISLASTCN))
		return EROFS;

	return VFS_VGET(zmp->zm_mountp, ino, ap->a_vpp);
}

int
zlfs_access(void *v)
{
	struct vop_access_args *ap = v;
	struct zlfs_node *znp = VTOZ(ap->a_vp);

	return vaccess(ap->a_vp->v_type, znp->zn_dinode.zi_mode & ALLPERMS,
	    znp->zn_dinode.zi_uid, znp->zn_dinode.zi_gid, ap->a_mode,
	    ap->a_cred);
}

int
zlfs_getattr(void *v)
{
	struct vop_getattr_args *ap = v;
	struct vnode *vp = ap->a_vp;
	struct vattr *vap = ap->a_vap;
	struct zlfs_node *znp = VTOZ(vp);
	struct zlfs_inode *zi = &znp->zn_dinode;

	vattr_null(vap);
	vap->va_fsid = znp->zn_zmp->zm_dev;
	vap->va_fileid = zi->zi_ino;
	vap->va_mode = zi->zi_mode & ALLPERMS;
	vap->va_nlink = zi->zi_nlink;
	vap->va_uid = zi->zi_uid;
	vap->va_gid = zi->zi_gid;
	vap->va_size = zi->zi_size;
	vap->va_blocksize = znp->zn_zmp->zm_super.zs_block_size;
	vap->va_bytes = zi->zi_blocks <<
	    znp->zn_zmp->zm_bshift;
	vap->va_atime.tv_sec = zi->zi_atime;
	vap->va_atime.tv_nsec = zi->zi_atimensec;
	vap->va_mtime.tv_sec = zi->zi_mtime;
	vap->va_mtime.tv_nsec = zi->zi_mtimensec;
	vap->va_ctime.tv_sec = zi->zi_ctime;
	vap->va_ctime.tv_nsec = zi->zi_ctimensec;
	vap->va_gen = zi->zi_gen;
	vap->va_flags = 0;
	vap->va_rdev = 0;
	vap->va_type = vp->v_type;

	return 0;
}

int
zlfs_setattr(void *v)
{
	return EROFS;
}

/*
 * Read regular-file data through the direct block pointers.  Only the
 * first ZLFS_NDADDR blocks are reachable; larger files need the
 * indirect blocks, which the read-only bring-up does not yet parse.
 */
int
zlfs_read(void *v)
{
	struct vop_read_args *ap = v;
	struct vnode *vp = ap->a_vp;
	struct uio *uio = ap->a_uio;
	struct zlfs_node *znp = VTOZ(vp);
	struct zlfs_mount *zmp = znp->zn_zmp;
	struct zlfs_inode *zi = &znp->zn_dinode;
	struct buf *bp;
	u_int64_t blkno, boff, size = zi->zi_size;
	u_int32_t bsize = zmp->zm_super.zs_block_size;
	size_t n;
	int error = 0;

	if (vp->v_type == VDIR)
		return EISDIR;
	if (vp->v_type != VREG)
		return EOPNOTSUPP;
	if (uio->uio_offset < 0)
		return EINVAL;

	while (uio->uio_resid > 0 && (u_int64_t)uio->uio_offset < size) {
		blkno = uio->uio_offset / bsize;
		boff = uio->uio_offset % bsize;
		n = bsize - boff;
		if (n > size - (u_int64_t)uio->uio_offset)
			n = size - uio->uio_offset;
		if (n > uio->uio_resid)
			n = uio->uio_resid;

		if (blkno >= ZLFS_NDADDR || zi->zi_db[blkno] == 0)
			return EIO;
		error = zlfs_bread_block(zmp, zi->zi_db[blkno], &bp);
		if (error != 0)
			return error;
		error = uiomove((u_int8_t *)bp->b_data + boff, n, uio);
		brelse(bp);
		if (error != 0)
			break;
	}

	return error;
}

/*
 * Emit directory entries from the on-disk directory blocks.
 * uio_offset is a byte offset into the directory (a multiple of
 * ZLFS_DIRENT_SIZE); unused slots (zd_ino == 0) are skipped.
 */
int
zlfs_readdir(void *v)
{
	struct vop_readdir_args *ap = v;
	struct uio *uio = ap->a_uio;
	struct vnode *vp = ap->a_vp;
	struct zlfs_node *znp = VTOZ(vp);
	struct zlfs_mount *zmp = znp->zn_zmp;
	struct zlfs_inode *zi = &znp->zn_dinode;
	struct zlfs_dirent *zd;
	struct dirent d;
	struct buf *bp = NULL;
	u_int64_t off, blkno, dblk = (u_int64_t)-1;
	u_int32_t bsize = zmp->zm_super.zs_block_size;
	int error = 0, slot;

	if (vp->v_type != VDIR)
		return ENOTDIR;
	if (uio->uio_offset < 0 || (uio->uio_offset % ZLFS_DIRENT_SIZE) != 0)
		return EINVAL;

	*ap->a_eofflag = 0;

	for (off = uio->uio_offset; off + ZLFS_DIRENT_SIZE <= zi->zi_size;
	    off += ZLFS_DIRENT_SIZE) {
		blkno = off / bsize;
		if (blkno >= ZLFS_NDADDR || zi->zi_db[blkno] == 0)
			break;
		if (blkno != dblk) {
			if (bp != NULL)
				brelse(bp);
			error = zlfs_bread_block(zmp, zi->zi_db[blkno], &bp);
			if (error != 0)
				return error;
			dblk = blkno;
		}
		slot = (off % bsize) / ZLFS_DIRENT_SIZE;
		zd = (struct zlfs_dirent *)((u_int8_t *)bp->b_data +
		    slot * ZLFS_DIRENT_SIZE);
		if (zd->zd_ino == 0)
			continue;

		memset(&d, 0, sizeof(d));
		d.d_fileno = letoh64(zd->zd_ino);
		d.d_type = zd->zd_type;
		d.d_namlen = zd->zd_namlen;
		if (d.d_namlen > ZLFS_MAXNAMLEN)
			d.d_namlen = ZLFS_MAXNAMLEN;
		memcpy(d.d_name, zd->zd_name, d.d_namlen);
		d.d_name[d.d_namlen] = '\0';
		d.d_reclen = DIRENT_SIZE(&d);
		d.d_off = off + ZLFS_DIRENT_SIZE;

		if (uio->uio_resid < d.d_reclen)
			goto done;
		error = uiomove(&d, d.d_reclen, uio);
		if (error != 0)
			goto done;
		uio->uio_offset = off + ZLFS_DIRENT_SIZE;
	}

	uio->uio_offset = off;
	*ap->a_eofflag = 1;

done:
	if (bp != NULL)
		brelse(bp);
	return error;
}

int
zlfs_inactive(void *v)
{
	struct vop_inactive_args *ap = v;

	VOP_UNLOCK(ap->a_vp);
	return 0;
}

int
zlfs_reclaim(void *v)
{
	struct vop_reclaim_args *ap = v;
	struct vnode *vp = ap->a_vp;
	struct zlfs_node *znp = VTOZ(vp);
	struct zlfs_mount *zmp;

	if (znp != NULL) {
		zmp = znp->zn_zmp;
		if (znp->zn_onlist) {
			rw_enter_write(&zmp->zm_lock);
			LIST_REMOVE(znp, zn_entry);
			rw_exit_write(&zmp->zm_lock);
		}
		cache_purge(vp);
		free(znp, M_ZLFS, sizeof(*znp));
	}
	vp->v_data = NULL;
	return 0;
}

int
zlfs_lock(void *v)
{
	struct vop_lock_args *ap = v;
	struct vnode *vp = ap->a_vp;

	return rrw_enter(&VTOZ(vp)->zn_lock, ap->a_flags & LK_RWFLAGS);
}

int
zlfs_unlock(void *v)
{
	struct vop_unlock_args *ap = v;
	struct vnode *vp = ap->a_vp;

	rrw_exit(&VTOZ(vp)->zn_lock);
	return 0;
}

int
zlfs_islocked(void *v)
{
	struct vop_islocked_args *ap = v;

	return rrw_status(&VTOZ(ap->a_vp)->zn_lock);
}

int
zlfs_pathconf(void *v)
{
	struct vop_pathconf_args *ap = v;
	int error = 0;

	switch (ap->a_name) {
	case _PC_LINK_MAX:
		*ap->a_retval = 1;
		break;
	case _PC_NAME_MAX:
		*ap->a_retval = NAME_MAX;
		break;
	case _PC_CHOWN_RESTRICTED:
		*ap->a_retval = 1;
		break;
	case _PC_NO_TRUNC:
		*ap->a_retval = 1;
		break;
	case _PC_TIMESTAMP_RESOLUTION:
		*ap->a_retval = 1;
		break;
	default:
		error = EINVAL;
		break;
	}

	return error;
}

int
zlfs_print(void *v)
{
#ifdef DEBUG
	struct vop_print_args *ap = v;
	struct zlfs_node *znp = VTOZ(ap->a_vp);

	printf("zlfs inode %llu\n", (unsigned long long)znp->zn_ino);
#endif
	return 0;
}

const struct vops zlfs_vops = {
	.vop_lookup	= zlfs_lookup,
	.vop_create	= eopnotsupp,
	.vop_mknod	= eopnotsupp,
	.vop_open	= nullop,
	.vop_close	= nullop,
	.vop_access	= zlfs_access,
	.vop_getattr	= zlfs_getattr,
	.vop_setattr	= zlfs_setattr,
	.vop_read	= zlfs_read,
	.vop_write	= eopnotsupp,
	.vop_ioctl	= eopnotsupp,
	.vop_kqfilter	= eopnotsupp,
	.vop_revoke	= vop_generic_revoke,
	.vop_fsync	= nullop,
	.vop_remove	= eopnotsupp,
	.vop_link	= eopnotsupp,
	.vop_rename	= eopnotsupp,
	.vop_mkdir	= eopnotsupp,
	.vop_rmdir	= eopnotsupp,
	.vop_symlink	= eopnotsupp,
	.vop_readdir	= zlfs_readdir,
	.vop_readlink	= eopnotsupp,
	.vop_abortop	= vop_generic_abortop,
	.vop_inactive	= zlfs_inactive,
	.vop_reclaim	= zlfs_reclaim,
	.vop_lock	= zlfs_lock,
	.vop_unlock	= zlfs_unlock,
	.vop_bmap	= eopnotsupp,
	.vop_strategy	= eopnotsupp,
	.vop_print	= zlfs_print,
	.vop_islocked	= zlfs_islocked,
	.vop_pathconf	= zlfs_pathconf,
	.vop_advlock	= eopnotsupp,
	.vop_bwrite	= vop_generic_bwrite,
};
