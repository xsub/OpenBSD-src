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

#include <uvm/uvm_extern.h>

#include <zlfs/zlfs_var.h>

int	zlfs_lookup(void *);
int	zlfs_create(void *);
int	zlfs_mkdir(void *);
int	zlfs_rmdir(void *);
int	zlfs_remove(void *);
int	zlfs_rename(void *);
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

/*
 * Add a directory entry, reusing a freed slot if one exists so
 * directories do not grow without bound.
 */
static int
zlfs_dir_add(struct zlfs_node *dnp, const char *name, int namlen,
    u_int64_t ino, u_int8_t type)
{
	struct zlfs_mount *zmp = dnp->zn_zmp;
	struct zlfs_dirent zd;
	u_int64_t off, slot = (u_int64_t)-1;
	int error;

	error = zlfs_node_load(dnp);
	if (error != 0)
		return error;

	for (off = 0; off + ZLFS_DIRENT_SIZE <= dnp->zn_datalen;
	    off += ZLFS_DIRENT_SIZE) {
		if (((struct zlfs_dirent *)(dnp->zn_data + off))->zd_ino == 0) {
			slot = off;
			break;
		}
	}
	if (slot == (u_int64_t)-1) {
		if (dnp->zn_datalen + ZLFS_DIRENT_SIZE > ZLFS_MAXDIRSZ(zmp))
			return ENOSPC;
		error = zlfs_node_resize(dnp, dnp->zn_datalen + ZLFS_DIRENT_SIZE);
		if (error != 0)
			return error;
		slot = dnp->zn_datalen;
		dnp->zn_datalen += ZLFS_DIRENT_SIZE;
		dnp->zn_dinode.zi_size = dnp->zn_datalen;
	}

	memset(&zd, 0, sizeof(zd));
	zd.zd_ino = htole64(ino);
	zd.zd_type = type;
	zd.zd_namlen = namlen;
	memcpy(zd.zd_name, name, namlen);
	memcpy(dnp->zn_data + slot, &zd, sizeof(zd));
	dnp->zn_dinode.zi_mtime = gettime();
	zlfs_node_dirty(dnp);
	return 0;
}

/*
 * Remove the directory entry that points at inode ino (matched by
 * inode number, so the caller needs no pathname buffer).
 */
static int
zlfs_dir_remove(struct zlfs_node *dnp, u_int64_t ino)
{
	struct zlfs_dirent *zd;
	u_int64_t off;
	int error;

	error = zlfs_node_load(dnp);
	if (error != 0)
		return error;

	for (off = 0; off + ZLFS_DIRENT_SIZE <= dnp->zn_datalen;
	    off += ZLFS_DIRENT_SIZE) {
		zd = (struct zlfs_dirent *)(dnp->zn_data + off);
		if (zd->zd_ino != 0 && letoh64(zd->zd_ino) == ino) {
			zd->zd_ino = 0;
			dnp->zn_dinode.zi_mtime = gettime();
			zlfs_node_dirty(dnp);
			return 0;
		}
	}
	return ENOENT;
}

/* Return 1 if the directory holds nothing but "." and "..". */
static int
zlfs_dir_empty(struct zlfs_node *dnp)
{
	struct zlfs_dirent *zd;
	u_int64_t off;

	if (zlfs_node_load(dnp) != 0)
		return 0;

	for (off = 0; off + ZLFS_DIRENT_SIZE <= dnp->zn_datalen;
	    off += ZLFS_DIRENT_SIZE) {
		zd = (struct zlfs_dirent *)(dnp->zn_data + off);
		if (zd->zd_ino == 0)
			continue;
		if (zd->zd_namlen == 1 && zd->zd_name[0] == '.')
			continue;
		if (zd->zd_namlen == 2 && zd->zd_name[0] == '.' &&
		    zd->zd_name[1] == '.')
			continue;
		return 0;
	}
	return 1;
}

/* Repoint a directory's ".." entry at a new parent inode. */
static int
zlfs_dir_setdotdot(struct zlfs_node *dnp, u_int64_t parent)
{
	struct zlfs_dirent *zd;
	u_int64_t off;
	int error;

	error = zlfs_node_load(dnp);
	if (error != 0)
		return error;

	for (off = 0; off + ZLFS_DIRENT_SIZE <= dnp->zn_datalen;
	    off += ZLFS_DIRENT_SIZE) {
		zd = (struct zlfs_dirent *)(dnp->zn_data + off);
		if (zd->zd_ino != 0 && zd->zd_namlen == 2 &&
		    zd->zd_name[0] == '.' && zd->zd_name[1] == '.') {
			zd->zd_ino = htole64(parent);
			zlfs_node_dirty(dnp);
			return 0;
		}
	}
	return ENOENT;		/* a directory always has ".." */
}

/*
 * Reject moving directory fnp into its own subtree: walk up from tdvp
 * through the ".." chain to the root and fail with EINVAL if fnp is an
 * ancestor of (or equal to) the destination directory.  Safe under the
 * mount's single-threaded assumption; it briefly vgets each ancestor to
 * read its "..".
 */
static int
zlfs_rename_ancestor(struct zlfs_mount *zmp, struct zlfs_node *fnp,
    struct vnode *tdvp)
{
	struct vnode *vp;
	u_int64_t ino, parent, steps;
	int error;

	error = zlfs_dir_lookup(tdvp, "..", 2, &ino);
	if (error != 0)
		return error;

	/*
	 * A valid tree is at most zm_ninodes deep; bound the walk by that
	 * so a corrupt on-disk ".." cycle fails rather than spins forever.
	 */
	for (steps = 0; ino != ZLFS_ROOT_INO; steps++) {
		if (steps > zmp->zm_ninodes)
			return ELOOP;
		if (ino == fnp->zn_ino)
			return EINVAL;		/* fnp is an ancestor */
		error = VFS_VGET(zmp->zm_mountp, ino, &vp);
		if (error != 0)
			return error;
		error = zlfs_dir_lookup(vp, "..", 2, &parent);
		vput(vp);
		if (error != 0)
			return error;
		if (parent == ino)		/* self-parent: stop */
			break;
		ino = parent;
	}
	return 0;
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
		 * Resolve ".." to the parent named by the directory's own
		 * ".." entry.  Follow the ufs lock dance: drop the child
		 * lock to fetch the parent (parent-before-child ordering),
		 * then relock if the caller wants the parent locked.  (The
		 * VFS layer intercepts ".." at the mount root before us.)
		 */
		error = zlfs_dir_lookup(dvp, "..", 2, &ino);
		if (error != 0)
			return error;
		VOP_UNLOCK(dvp);
		cnp->cn_flags |= PDIRUNLOCK;
		error = VFS_VGET(zmp->zm_mountp, ino, ap->a_vpp);
		if (error != 0) {
			if (vn_lock(dvp, LK_EXCLUSIVE | LK_RETRY) == 0)
				cnp->cn_flags &= ~PDIRUNLOCK;
			return error;
		}
		if ((cnp->cn_flags & LOCKPARENT) && (cnp->cn_flags & ISLASTCN)) {
			error = vn_lock(dvp, LK_EXCLUSIVE);
			if (error != 0) {
				vput(*ap->a_vpp);
				*ap->a_vpp = NULL;
				return error;
			}
			cnp->cn_flags &= ~PDIRUNLOCK;
		}
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
			/*
			 * Keep the pathname buffer for VOP_CREATE; without
			 * SAVENAME namei frees it and the VOP double-frees.
			 */
			cnp->cn_flags |= SAVENAME;
			return EJUSTRETURN;
		}
		return ENOENT;
	}
	if (error != 0)
		return error;

	/* Rename of an existing name is not supported yet. */
	if (cnp->cn_nameiop == RENAME && (cnp->cn_flags & ISLASTCN))
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
	u_int32_t bsize = zmp->zm_super.zs_block_size;
	u_int64_t b, newsize, oldsize;
	int error;

	if (zmp->zm_rdonly)
		return EROFS;
	if (vap->va_size != VNOVAL) {
		if (vp->v_type != VREG)
			return EISDIR;
		if (vap->va_size > ZLFS_MAXFILESZ(zmp))
			return EFBIG;
		newsize = vap->va_size;
		oldsize = znp->zn_dinode.zi_size;

		if (newsize > oldsize) {
			/* Materialise the new blocks as zeroes (no holes).
			 * The old EOF block's tail is zero by invariant. */
			for (b = howmany(oldsize, bsize);
			    b < howmany(newsize, bsize); b++) {
				error = zlfs_dblk_prepare(znp, b, 0);
				if (error != 0)
					return error;
			}
		} else if (newsize < oldsize) {
			/* Drop overlay buffers beyond the new end. */
			for (b = howmany(newsize, bsize);
			    b < znp->zn_ndblk; b++) {
				if (znp->zn_dblk[b] != NULL) {
					free(znp->zn_dblk[b], M_ZLFS, bsize);
					znp->zn_dblk[b] = NULL;
				}
			}
			/*
			 * Zero the tail of the new EOF block so a later
			 * grow cannot re-expose the truncated bytes.
			 */
			if (newsize % bsize != 0) {
				b = newsize / bsize;
				error = zlfs_dblk_prepare(znp, b, 1);
				if (error != 0)
					return error;
				memset(znp->zn_dblk[b] + newsize % bsize, 0,
				    bsize - newsize % bsize);
			}
		}
		znp->zn_dinode.zi_size = newsize;
		/* Keep the UVM object's size and pages coherent (a shrink
		 * frees the pages beyond the new end). */
		uvm_vnp_setsize(vp, newsize);
		(void)uvm_vnp_uncache(vp);
		zlfs_node_dirty(znp);
	}
	return 0;
}

/*
 * Read regular-file data block by block: a block with a dirty overlay
 * buffer is served from it, everything else from disk through
 * zlfs_bmap_read (direct, single- and double-indirect pointers).
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

	size = zi->zi_size;
	while (uio->uio_resid > 0 && (u_int64_t)uio->uio_offset < size) {
		u_int64_t lba;

		blkno = uio->uio_offset / bsize;
		boff = uio->uio_offset % bsize;
		n = bsize - boff;
		if (n > size - (u_int64_t)uio->uio_offset)
			n = size - uio->uio_offset;
		if (n > uio->uio_resid)
			n = uio->uio_resid;

		if (znp->zn_dblk != NULL && blkno < znp->zn_ndblk &&
		    znp->zn_dblk[blkno] != NULL) {
			error = uiomove(znp->zn_dblk[blkno] + boff, n, uio);
			if (error != 0)
				break;
			continue;
		}

		error = zlfs_bmap_read(znp, blkno, &lba);
		if (error == 0 && lba == 0)
			error = EIO;
		if (error != 0)
			break;
		error = zlfs_bread_block(zmp, lba, &bp);
		if (error != 0)
			break;
		error = uiomove((u_int8_t *)bp->b_data + boff, n, uio);
		brelse(bp);
		if (error != 0)
			break;
	}

	return error;
}

/*
 * Write regular-file data into the per-block dirty overlay; it reaches
 * disk at the next commit, which rewrites only the dirty blocks.  A
 * partially covered block is read-modify-written; blocks in a gap left
 * by writing past end of file materialise as zeroes (no holes on disk).
 */
int
zlfs_write(void *v)
{
	struct vop_write_args *ap = v;
	struct vnode *vp = ap->a_vp;
	struct uio *uio = ap->a_uio;
	struct zlfs_node *znp = VTOZ(vp);
	struct zlfs_mount *zmp = znp->zn_zmp;
	u_int32_t bsize = zmp->zm_super.zs_block_size;
	u_int64_t off, end, oldsize, b, first, last, boff;
	u_int64_t maxsz = ZLFS_MAXFILESZ(zmp);
	size_t n, seg;
	int error, rmw;

	if (zmp->zm_rdonly)
		return EROFS;
	if (vp->v_type != VREG)
		return EOPNOTSUPP;
	if (ap->a_ioflag & IO_APPEND)
		uio->uio_offset = znp->zn_dinode.zi_size;
	if (uio->uio_offset < 0)
		return EINVAL;
	off = uio->uio_offset;
	n = uio->uio_resid;
	if (n == 0)
		return 0;
	if (off > maxsz || n > maxsz - off)
		return EFBIG;
	end = off + n;
	oldsize = znp->zn_dinode.zi_size;

	/* Materialise any gap between the old end and the write start. */
	if (off > oldsize) {
		for (b = oldsize / bsize; b < off / bsize; b++) {
			/* Only the old EOF block holds live data. */
			error = zlfs_dblk_prepare(znp, b,
			    b * (u_int64_t)bsize < oldsize);
			if (error != 0)
				return error;
		}
	}

	first = off / bsize;
	last = (end - 1) / bsize;
	for (b = first; b <= last; b++) {
		/*
		 * Read the block's current contents whenever it overlaps
		 * live data ([0, zi_size)): a faulting uiomove then leaves
		 * old bytes rather than zeroes, and a block at or beyond
		 * the current end never resurrects stale on-disk bytes --
		 * an uncommitted shrink leaves its old pointers in the
		 * inode until the next commit.
		 */
		rmw = b * (u_int64_t)bsize < oldsize;
		error = zlfs_dblk_prepare(znp, b, rmw);
		if (error != 0)
			goto out;
		/* The overlay must reach a commit even on a later fault. */
		zlfs_node_dirty(znp);
		boff = (b == first) ? off % bsize : 0;
		seg = MIN((u_int64_t)bsize - boff, end - (b * bsize + boff));
		error = uiomove(znp->zn_dblk[b] + boff, seg, uio);
		if (error != 0)
			goto out;
	}

	if (end > oldsize) {
		znp->zn_dinode.zi_size = end;
		uvm_vnp_setsize(vp, end);
	}
	znp->zn_dinode.zi_mtime = gettime();
out:
	/*
	 * Discard resident UVM pages so the next mmap fault re-reads
	 * through VOP_READ and sees the new contents (mirrors ffs_write).
	 * Runs on the error exits too: earlier blocks of this write may
	 * already have changed.
	 */
	(void)uvm_vnp_uncache(vp);
	return error;
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

	error = zlfs_dir_add(dnp, cnp->cn_nameptr, cnp->cn_namelen,
	    VTOZ(vp)->zn_ino, DT_REG);
	if (error != 0) {
		zlfs_idrop(vp);
		goto bad;
	}

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
zlfs_mkdir(void *v)
{
	struct vop_mkdir_args *ap = v;
	struct vnode *dvp = ap->a_dvp;
	struct componentname *cnp = ap->a_cnp;
	struct vattr *vap = ap->a_vap;
	struct zlfs_node *dnp = VTOZ(dvp);
	struct zlfs_mount *zmp = dnp->zn_zmp;
	struct zlfs_node *np;
	struct zlfs_dirent zd;
	struct vnode *vp;
	u_int64_t ino;
	int error;

	if (zmp->zm_rdonly) {
		error = EROFS;
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

	error = zlfs_ialloc(zmp, S_IFDIR | (vap->va_mode & 07777), &vp);
	if (error != 0)
		goto bad;
	np = VTOZ(vp);
	np->zn_dinode.zi_nlink = 2;	/* "." and the parent's entry */

	/* Lay down the new directory's "." and ".." entries. */
	error = zlfs_node_resize(np, 2 * ZLFS_DIRENT_SIZE);
	if (error != 0) {
		zlfs_idrop(vp);
		goto bad;
	}
	memset(&zd, 0, sizeof(zd));
	zd.zd_ino = htole64(np->zn_ino);
	zd.zd_type = DT_DIR;
	zd.zd_namlen = 1;
	zd.zd_name[0] = '.';
	memcpy(np->zn_data + 0 * ZLFS_DIRENT_SIZE, &zd, sizeof(zd));
	memset(&zd, 0, sizeof(zd));
	zd.zd_ino = htole64(dnp->zn_ino);
	zd.zd_type = DT_DIR;
	zd.zd_namlen = 2;
	zd.zd_name[0] = '.';
	zd.zd_name[1] = '.';
	memcpy(np->zn_data + 1 * ZLFS_DIRENT_SIZE, &zd, sizeof(zd));
	np->zn_datalen = 2 * ZLFS_DIRENT_SIZE;
	np->zn_dinode.zi_size = np->zn_datalen;
	zlfs_node_dirty(np);

	error = zlfs_dir_add(dnp, cnp->cn_nameptr, cnp->cn_namelen,
	    np->zn_ino, DT_DIR);
	if (error != 0) {
		zlfs_idrop(vp);
		goto bad;
	}
	dnp->zn_dinode.zi_nlink++;	/* the child's ".." links back */
	zlfs_node_dirty(dnp);

	if ((cnp->cn_flags & SAVESTART) == 0)
		pool_put(&namei_pool, cnp->cn_pnbuf);
	/*
	 * Unlike VOP_CREATE, the VOP_MKDIR callers do not release the
	 * parent directory, so this VOP owns the vput(dvp) on every path.
	 */
	vput(dvp);
	*ap->a_vpp = vp;
	return 0;

bad:
	pool_put(&namei_pool, cnp->cn_pnbuf);
	vput(dvp);
	*ap->a_vpp = NULL;
	return error;
}

/*
 * Remove a file.  The VOP_REMOVE wrapper releases dvp and vp, and
 * namei owns the pathname buffer, so this only edits the directory and
 * the inode.
 */
int
zlfs_remove(void *v)
{
	struct vop_remove_args *ap = v;
	struct zlfs_node *dnp = VTOZ(ap->a_dvp);
	struct zlfs_node *np = VTOZ(ap->a_vp);
	struct zlfs_mount *zmp = dnp->zn_zmp;
	int error;

	if (zmp->zm_rdonly)
		return EROFS;
	if (ap->a_vp->v_type == VDIR)
		return EPERM;

	error = zlfs_dir_remove(dnp, np->zn_ino);
	if (error != 0)
		return error;

	if (np->zn_dinode.zi_nlink > 0)
		np->zn_dinode.zi_nlink--;
	if (np->zn_dinode.zi_nlink == 0)
		zmp->zm_imap[np->zn_ino] = 0;
	zlfs_node_dirty(np);
	return 0;
}

/*
 * Remove an empty directory.  Unlike VOP_REMOVE, the VOP_RMDIR wrapper
 * does not release dvp/vp, so this must vput them itself.
 */
int
zlfs_rmdir(void *v)
{
	struct vop_rmdir_args *ap = v;
	struct vnode *dvp = ap->a_dvp;
	struct vnode *vp = ap->a_vp;
	struct zlfs_node *dnp = VTOZ(dvp);
	struct zlfs_node *np = VTOZ(vp);
	struct zlfs_mount *zmp = dnp->zn_zmp;
	int error;

	if (zmp->zm_rdonly) {
		error = EROFS;
		goto out;
	}
	if (vp->v_type != VDIR) {
		error = ENOTDIR;
		goto out;
	}
	if (!zlfs_dir_empty(np)) {
		error = ENOTEMPTY;
		goto out;
	}

	error = zlfs_dir_remove(dnp, np->zn_ino);
	if (error != 0)
		goto out;
	if (dnp->zn_dinode.zi_nlink > 0)
		dnp->zn_dinode.zi_nlink--;	/* drop the child's ".." link */
	zlfs_node_dirty(dnp);
	np->zn_dinode.zi_nlink = 0;
	np->zn_datalen = 0;
	zmp->zm_imap[np->zn_ino] = 0;
	zlfs_node_dirty(np);

out:
	vput(vp);
	vput(dvp);
	return error;
}

/*
 * Rename.  The VOP_RENAME wrapper does no vnode disposal, so this owns
 * all four: fdvp/fvp arrive referenced and unlocked, tdvp/tvp (tvp may
 * be NULL) referenced and exclusively locked; every path must release
 * the two refs, plus unlock and release tdvp/tvp.  The pathname buffers
 * belong to the caller (dorenameat frees them), so they are not touched
 * here.
 *
 * This is a bring-up rename: it works on the in-core directory buffers
 * without the tmpfs-style relock dance, consistent with the mount's
 * documented single-threaded-commit assumption.  Moving a directory to a
 * different parent reparents its ".." and moves the child link between
 * the two parents, rejecting a move into the directory's own subtree.
 */
int
zlfs_rename(void *v)
{
	struct vop_rename_args *ap = v;
	struct vnode *fdvp = ap->a_fdvp, *fvp = ap->a_fvp;
	struct vnode *tdvp = ap->a_tdvp, *tvp = ap->a_tvp;
	struct componentname *fcnp = ap->a_fcnp, *tcnp = ap->a_tcnp;
	struct zlfs_node *fdnp = VTOZ(fdvp), *fnp = VTOZ(fvp);
	struct zlfs_node *tdnp = VTOZ(tdvp);
	struct zlfs_node *tnp = (tvp != NULL) ? VTOZ(tvp) : NULL;
	struct zlfs_mount *zmp = fdnp->zn_zmp;
	u_int8_t ftype;
	int dirmove, error = 0;

	/* Same filesystem only. */
	if (fvp->v_mount != tdvp->v_mount ||
	    (tvp != NULL && fvp->v_mount != tvp->v_mount)) {
		error = EXDEV;
		goto out;
	}
	if (zmp->zm_rdonly) {
		error = EROFS;
		goto out;
	}
	/* Reject "." and ".." as the source name. */
	if ((fcnp->cn_namelen == 1 && fcnp->cn_nameptr[0] == '.') ||
	    (fcnp->cn_namelen == 2 && fcnp->cn_nameptr[0] == '.' &&
	    fcnp->cn_nameptr[1] == '.')) {
		error = EINVAL;
		goto out;
	}
	/* The source may not be the target directory itself. */
	if (fvp == tdvp) {
		error = EINVAL;
		goto out;
	}
	if (tcnp->cn_namelen > ZLFS_MAXNAMLEN) {
		error = ENAMETOOLONG;
		goto out;
	}

	/* A move of a directory to a different parent reparents its "..". */
	dirmove = (fvp->v_type == VDIR && fdvp != tdvp);
	ftype = (fvp->v_type == VDIR) ? DT_DIR : DT_REG;

	/*
	 * Do everything that can fail up front, before any destructive
	 * change, so the rename is all-or-nothing: POSIX requires that a
	 * failed rename leave both names in place.  Load both directories,
	 * check the target, and -- when a new entry must be appended to a
	 * different target directory -- reserve its slot now.  After this
	 * point the source detach, target teardown, and reattach below
	 * cannot fail: the reattach always finds a freed slot (the detached
	 * source's, or the removed target's) or the reserved space.
	 */
	error = zlfs_node_load(fdnp);
	if (error != 0)
		goto out;
	error = zlfs_node_load(tdnp);
	if (error != 0)
		goto out;
	if (tnp != NULL && tvp->v_type == VDIR && !zlfs_dir_empty(tnp)) {
		error = ENOTEMPTY;
		goto out;
	}
	if (dirmove) {
		/* Load fnp now so the ".." repoint below cannot fail, and
		 * reject moving the directory into its own subtree. */
		error = zlfs_node_load(fnp);
		if (error != 0)
			goto out;
		error = zlfs_rename_ancestor(zmp, fnp, tdvp);
		if (error != 0)
			goto out;
	}
	if (tnp == NULL && fdvp != tdvp) {
		if (tdnp->zn_datalen + ZLFS_DIRENT_SIZE > ZLFS_MAXDIRSZ(zmp)) {
			error = ENOSPC;
			goto out;
		}
		error = zlfs_node_resize(tdnp,
		    tdnp->zn_datalen + ZLFS_DIRENT_SIZE);
		if (error != 0)
			goto out;
	}

	/*
	 * Detach the source.  This is the first mutation, so an unexpected
	 * ENOENT (the entry named by fcnp is gone) leaves everything else
	 * untouched.  When the rename stays in one directory this frees the
	 * slot the reattach reuses.
	 */
	error = zlfs_dir_remove(fdnp, fnp->zn_ino);
	if (error != 0)
		goto out;

	/* Drop an existing target, freeing its slot in the target dir. */
	if (tnp != NULL) {
		(void)zlfs_dir_remove(tdnp, tnp->zn_ino);
		if (tvp->v_type == VDIR && tdnp->zn_dinode.zi_nlink > 0)
			tdnp->zn_dinode.zi_nlink--;	/* lost target's ".." */
		tnp->zn_dinode.zi_nlink = 0;
		tnp->zn_datalen = 0;
		zmp->zm_imap[tnp->zn_ino] = 0;
		zlfs_node_dirty(tnp);
	}

	/* Reattach the source under the new name; cannot fail now. */
	error = zlfs_dir_add(tdnp, tcnp->cn_nameptr, tcnp->cn_namelen,
	    fnp->zn_ino, ftype);
	KASSERT(error == 0);

	/*
	 * A directory moved to a new parent takes its ".." with it: repoint
	 * it, and move the "child's .." link from the old parent to the new
	 * one.  fnp was loaded up front, so the repoint cannot fail here.
	 */
	if (dirmove) {
		(void)zlfs_dir_setdotdot(fnp, tdnp->zn_ino);
		if (fdnp->zn_dinode.zi_nlink > 0)
			fdnp->zn_dinode.zi_nlink--;
		tdnp->zn_dinode.zi_nlink++;
		zlfs_node_dirty(fdnp);
		zlfs_node_dirty(tdnp);
	}

	fnp->zn_dinode.zi_ctime = gettime();
	zlfs_node_dirty(fnp);

out:
	VOP_UNLOCK(tdvp);
	if (tvp != NULL && tvp != tdvp)
		VOP_UNLOCK(tvp);
	vrele(fvp);
	if (tvp != NULL)
		vrele(tvp);
	vrele(fdvp);
	vrele(tdvp);
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
			free(znp->zn_data, M_ZLFS, znp->zn_dataalloc);
		zlfs_dblk_free(znp);
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
	.vop_remove	= zlfs_remove,
	.vop_link	= eopnotsupp,
	.vop_rename	= zlfs_rename,
	.vop_mkdir	= zlfs_mkdir,
	.vop_rmdir	= zlfs_rmdir,
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
