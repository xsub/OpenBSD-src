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
#include <sys/fcntl.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/namei.h>
#include <sys/pool.h>
#include <sys/proc.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/unistd.h>
#include <sys/vnode.h>
#include <sys/zlfs.h>

#include <zlfs/zlfs_var.h>

int	zlfs_lookup(void *);
int	zlfs_create(void *);
int	zlfs_access(void *);
int	zlfs_getattr(void *);
int	zlfs_setattr(void *);
int	zlfs_read(void *);
int	zlfs_write(void *);
int	zlfs_fsync(void *);
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
	struct zlfs_dirent *zd;
	u_int64_t off;
	int error;

	if (namlen > ZLFS_MAXNAMLEN)
		return ENOENT;

	/*
	 * Directory contents are held in core (loaded on demand), so a
	 * lookup sees entries added since the last commit.
	 */
	error = zlfs_node_load(dnp);
	if (error != 0)
		return error;

	for (off = 0; off + ZLFS_DIRENT_SIZE <= dnp->zn_datalen;
	    off += ZLFS_DIRENT_SIZE) {
		zd = (struct zlfs_dirent *)(dnp->zn_data + off);
		if (zd->zd_ino == 0)
			continue;
		if (zd->zd_namlen == namlen &&
		    memcmp(zd->zd_name, name, namlen) == 0) {
			*inop = letoh64(zd->zd_ino);
			return 0;
		}
	}
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
	cnp->cn_flags &= ~PDIRUNLOCK;
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
		/*
		 * Name absent: on a read-write mount let the caller
		 * create it (parent stays locked); otherwise fail.
		 */
		if ((cnp->cn_nameiop == CREATE || cnp->cn_nameiop == RENAME) &&
		    (cnp->cn_flags & ISLASTCN)) {
			if (VTOZ(dvp)->zn_zmp->zm_rdonly)
				return EROFS;
			return EJUSTRETURN;
		}
		return ENOENT;
	}
	if (error != 0)
		return error;

	/* Delete/rename of an existing name is not supported yet. */
	if ((cnp->cn_nameiop == DELETE || cnp->cn_nameiop == RENAME) &&
	    (cnp->cn_flags & ISLASTCN))
		return EROFS;

	error = VFS_VGET(zmp->zm_mountp, ino, ap->a_vpp);
	if (error != 0)
		return error;

	/*
	 * Descending into a child: release the parent directory lock
	 * unless the caller asked to keep it (LOCKPARENT on the last
	 * component).  Every in-tree filesystem does this; omitting it
	 * leaks the parent's recursive vnode lock and wedges the next
	 * access to the cached directory.
	 */
	if (!(cnp->cn_flags & LOCKPARENT) || !(cnp->cn_flags & ISLASTCN)) {
		VOP_UNLOCK(dvp);
		cnp->cn_flags |= PDIRUNLOCK;
	}

	return 0;
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
	vap->va_gen = (u_int32_t)zi->zi_gen;
	vap->va_flags = 0;
	vap->va_rdev = 0;
	vap->va_type = vp->v_type;

	return 0;
}

/*
 * Only file truncation (via zn_data) is supported; other attribute
 * changes are silently accepted so common tools do not fail.
 */
int
zlfs_setattr(void *v)
{
	struct vop_setattr_args *ap = v;
	struct vnode *vp = ap->a_vp;
	struct vattr *vap = ap->a_vap;
	struct zlfs_node *znp = VTOZ(vp);
	struct zlfs_mount *zmp = znp->zn_zmp;
	int error;

	if (zmp->zm_rdonly)
		return EROFS;
	if (vap->va_size != VNOVAL) {
		if (vp->v_type != VREG)
			return EISDIR;
		if (vap->va_size > ZLFS_MAXFILESZ(zmp))
			return EFBIG;
		error = zlfs_node_load(znp);
		if (error != 0)
			return error;
		if (vap->va_size > znp->zn_datalen)
			memset(znp->zn_data + znp->zn_datalen, 0,
			    vap->va_size - znp->zn_datalen);
		znp->zn_datalen = vap->va_size;
		znp->zn_dinode.zi_size = vap->va_size;
		zlfs_node_dirty(znp);
	}
	return 0;
}

/*
 * Read regular-file data.  Modified files are served from their in-core
 * copy; clean files are read from the direct block pointers on disk.
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
	u_int64_t blkno, boff, size;
	u_int32_t bsize = zmp->zm_super.zs_block_size;
	size_t n;
	int error = 0;

	if (vp->v_type == VDIR)
		return EISDIR;
	if (vp->v_type != VREG)
		return EOPNOTSUPP;
	if (uio->uio_offset < 0)
		return EINVAL;

	if (znp->zn_data != NULL) {
		size = znp->zn_datalen;
		while (uio->uio_resid > 0 &&
		    (u_int64_t)uio->uio_offset < size) {
			n = size - (u_int64_t)uio->uio_offset;
			if (n > uio->uio_resid)
				n = uio->uio_resid;
			error = uiomove(znp->zn_data + uio->uio_offset, n, uio);
			if (error != 0)
				break;
		}
		return error;
	}

	size = zi->zi_size;
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
 * Write regular-file data into the in-core copy; it reaches disk at the
 * next commit.  Only direct-block-sized files are supported.
 */
int
zlfs_write(void *v)
{
	struct vop_write_args *ap = v;
	struct vnode *vp = ap->a_vp;
	struct uio *uio = ap->a_uio;
	struct zlfs_node *znp = VTOZ(vp);
	struct zlfs_mount *zmp = znp->zn_zmp;
	u_int64_t off, end, maxsz = ZLFS_MAXFILESZ(zmp);
	size_t n;
	int error;

	if (zmp->zm_rdonly)
		return EROFS;
	if (vp->v_type != VREG)
		return EOPNOTSUPP;
	if (ap->a_ioflag & IO_APPEND)
		uio->uio_offset = znp->zn_datalen;
	if (uio->uio_offset < 0)
		return EINVAL;
	off = uio->uio_offset;
	n = uio->uio_resid;
	if (off > maxsz || n > maxsz - off)
		return EFBIG;

	error = zlfs_node_load(znp);
	if (error != 0)
		return error;

	/* Zero any gap created by writing past the current end. */
	if (off > znp->zn_datalen)
		memset(znp->zn_data + znp->zn_datalen, 0,
		    off - znp->zn_datalen);

	end = off + n;
	error = uiomove(znp->zn_data + off, n, uio);
	if (error != 0)
		return error;

	if (end > znp->zn_datalen)
		znp->zn_datalen = end;
	znp->zn_dinode.zi_size = znp->zn_datalen;
	znp->zn_dinode.zi_mtime = gettime();
	zlfs_node_dirty(znp);
	return 0;
}

/*
 * Dispose of a just-allocated inode that could not be linked into a
 * directory: mark it unlinked and undirty so inactive recycles it
 * instead of leaving an orphan on the node list.
 */
static void
zlfs_idrop(struct vnode *vp)
{
	struct zlfs_node *znp = VTOZ(vp);

	znp->zn_dinode.zi_nlink = 0;
	znp->zn_dirty = 0;
	vput(vp);
}

int
zlfs_create(void *v)
{
	struct vop_create_args *ap = v;
	struct vnode *dvp = ap->a_dvp;
	struct componentname *cnp = ap->a_cnp;
	struct vattr *vap = ap->a_vap;
	struct zlfs_node *dnp = VTOZ(dvp);
	struct zlfs_mount *zmp = dnp->zn_zmp;
	struct zlfs_dirent zd;
	struct vnode *vp;
	u_int64_t ino;
	int error;

	if (zmp->zm_rdonly) {
		error = EROFS;
		goto bad;
	}
	if (vap->va_type != VREG) {
		error = EOPNOTSUPP;
		goto bad;
	}
	if (cnp->cn_namelen > ZLFS_MAXNAMLEN) {
		error = ENAMETOOLONG;
		goto bad;
	}
	if (zlfs_dir_lookup(dvp, cnp->cn_nameptr, cnp->cn_namelen, &ino) == 0) {
		error = EEXIST;
		goto bad;
	}

	error = zlfs_ialloc(zmp, S_IFREG | (vap->va_mode & 07777), &vp);
	if (error != 0)
		goto bad;

	/* Append the directory entry to the (in-core) parent. */
	error = zlfs_node_load(dnp);
	if (error != 0) {
		zlfs_idrop(vp);
		goto bad;
	}
	if (dnp->zn_datalen + ZLFS_DIRENT_SIZE > ZLFS_MAXFILESZ(zmp)) {
		zlfs_idrop(vp);
		error = ENOSPC;
		goto bad;
	}
	memset(&zd, 0, sizeof(zd));
	zd.zd_ino = htole64(VTOZ(vp)->zn_ino);
	zd.zd_type = DT_REG;
	zd.zd_namlen = cnp->cn_namelen;
	memcpy(zd.zd_name, cnp->cn_nameptr, cnp->cn_namelen);
	memcpy(dnp->zn_data + dnp->zn_datalen, &zd, sizeof(zd));
	dnp->zn_datalen += ZLFS_DIRENT_SIZE;
	dnp->zn_dinode.zi_size = dnp->zn_datalen;
	dnp->zn_dinode.zi_mtime = gettime();
	zlfs_node_dirty(dnp);

	if ((cnp->cn_flags & SAVESTART) == 0)
		pool_put(&namei_pool, cnp->cn_pnbuf);
	*ap->a_vpp = vp;
	return 0;

bad:
	pool_put(&namei_pool, cnp->cn_pnbuf);
	*ap->a_vpp = NULL;
	return error;
}

int
zlfs_fsync(void *v)
{
	struct vop_fsync_args *ap = v;
	struct zlfs_node *znp = VTOZ(ap->a_vp);

	if (znp->zn_zmp->zm_rdonly)
		return 0;
	return zlfs_commit(znp->zn_zmp);
}

/*
 * Emit directory entries from the in-core directory contents.
 * uio_offset is a byte offset (a multiple of ZLFS_DIRENT_SIZE); unused
 * slots (zd_ino == 0) are skipped.
 */
int
zlfs_readdir(void *v)
{
	struct vop_readdir_args *ap = v;
	struct uio *uio = ap->a_uio;
	struct vnode *vp = ap->a_vp;
	struct zlfs_node *znp = VTOZ(vp);
	struct zlfs_dirent *zd;
	struct dirent d;
	u_int64_t off;
	int error;

	if (vp->v_type != VDIR)
		return ENOTDIR;
	if (uio->uio_offset < 0 || (uio->uio_offset % ZLFS_DIRENT_SIZE) != 0)
		return EINVAL;
	error = zlfs_node_load(znp);
	if (error != 0)
		return error;

	*ap->a_eofflag = 0;

	for (off = uio->uio_offset; off + ZLFS_DIRENT_SIZE <= znp->zn_datalen;
	    off += ZLFS_DIRENT_SIZE) {
		zd = (struct zlfs_dirent *)(znp->zn_data + off);
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
			return 0;
		error = uiomove(&d, d.d_reclen, uio);
		if (error != 0)
			return error;
		uio->uio_offset = off + ZLFS_DIRENT_SIZE;
	}

	uio->uio_offset = off;
	*ap->a_eofflag = 1;
	return 0;
}

int
zlfs_inactive(void *v)
{
	struct vop_inactive_args *ap = v;
	struct vnode *vp = ap->a_vp;
	struct zlfs_node *znp = VTOZ(vp);

	VOP_UNLOCK(vp);
	/* An unlinked inode (e.g. a failed create) is not kept cached. */
	if (znp != NULL && znp->zn_dinode.zi_nlink == 0)
		vrecycle(vp, ap->a_p);
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
		if (znp->zn_data != NULL)
			free(znp->zn_data, M_ZLFS, ZLFS_MAXFILESZ(zmp));
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
	.vop_create	= zlfs_create,
	.vop_mknod	= eopnotsupp,
	.vop_open	= nullop,
	.vop_close	= nullop,
	.vop_access	= zlfs_access,
	.vop_getattr	= zlfs_getattr,
	.vop_setattr	= zlfs_setattr,
	.vop_read	= zlfs_read,
	.vop_write	= zlfs_write,
	.vop_ioctl	= eopnotsupp,
	.vop_kqfilter	= eopnotsupp,
	.vop_revoke	= vop_generic_revoke,
	.vop_fsync	= zlfs_fsync,
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
