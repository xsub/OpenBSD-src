/*	$OpenBSD$	*/

/*
 * ZLFS (Zoned Log-Structured File System) VFS operations.
 *
 * Read-only bring-up: mount discovers the newest superblock in the
 * superblock-log zones (see <sys/zlfs.h>), loads per-zone state via
 * the in-kernel zone report API, and exposes a synthetic empty root
 * directory until the checkpoint and inode map formats land.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/namei.h>
#include <sys/proc.h>
#include <sys/vnode.h>
#include <sys/lock.h>
#include <sys/mount.h>
#include <sys/buf.h>
#include <sys/fcntl.h>
#include <sys/disklabel.h>
#include <sys/dkio.h>
#include <sys/malloc.h>
#include <sys/specdev.h>
#include <sys/stat.h>
#include <sys/zlfs.h>

#include <zlfs/zlfs_var.h>

int	zlfs_mount(struct mount *, const char *, void *,
	    struct nameidata *, struct proc *);
int	zlfs_mountfs(struct vnode *, struct mount *, struct proc *,
	    struct zlfs_args *);
int	zlfs_start(struct mount *, int, struct proc *);
int	zlfs_unmount(struct mount *, int, struct proc *);
int	zlfs_root(struct mount *, struct vnode **);
int	zlfs_quotactl(struct mount *, int, uid_t, caddr_t, struct proc *);
int	zlfs_statfs(struct mount *, struct statfs *, struct proc *);
int	zlfs_sync(struct mount *, int, int, struct ucred *, struct proc *);
int	zlfs_vget(struct mount *, ino_t, struct vnode **);
int	zlfs_fhtovp(struct mount *, struct fid *, struct vnode **);
int	zlfs_vptofh(struct vnode *, struct fid *);
int	zlfs_init(struct vfsconf *);
int	zlfs_sysctl(int *, u_int, void *, size_t *, void *, size_t,
	    struct proc *);
int	zlfs_checkexp(struct mount *, struct mbuf *, int *,
	    struct ucred **);

int
zlfs_mount(struct mount *mp, const char *path, void *data,
    struct nameidata *ndp, struct proc *p)
{
	struct zlfs_args *args = data;
	struct zlfs_mount *zmp;
	struct vnode *devvp;
	char fspec[MNAMELEN];
	int error;

	if (mp->mnt_flag & MNT_UPDATE) {
		zmp = VFSTOZLFS(mp);
		if (args && args->fspec == NULL)
			return vfs_export(mp, &zmp->zm_export,
			    &args->export_info);
		return 0;
	}

	error = copyinstr(args->fspec, fspec, sizeof(fspec), NULL);
	if (error != 0)
		return error;
	NDINIT(ndp, LOOKUP, FOLLOW, UIO_SYSSPACE, fspec, p);
	if ((error = namei(ndp)) != 0)
		return error;
	devvp = ndp->ni_vp;

	if (devvp->v_type != VBLK) {
		vrele(devvp);
		return ENOTBLK;
	}
	if (major(devvp->v_rdev) >= nblkdev) {
		vrele(devvp);
		return ENXIO;
	}

	error = zlfs_mountfs(devvp, mp, p, args);
	if (error != 0) {
		vrele(devvp);
		return error;
	}

	bzero(mp->mnt_stat.f_mntonname, MNAMELEN);
	strlcpy(mp->mnt_stat.f_mntonname, path, MNAMELEN);
	bzero(mp->mnt_stat.f_mntfromname, MNAMELEN);
	strlcpy(mp->mnt_stat.f_mntfromname, fspec, MNAMELEN);
	bzero(mp->mnt_stat.f_mntfromspec, MNAMELEN);
	strlcpy(mp->mnt_stat.f_mntfromspec, fspec, MNAMELEN);
	bcopy(args, &mp->mnt_stat.mount_info.zlfs_args, sizeof(*args));

	zlfs_statfs(mp, &mp->mnt_stat, p);

	return 0;
}

int
zlfs_mountfs(struct vnode *devvp, struct mount *mp, struct proc *p,
    struct zlfs_args *args)
{
	struct zlfs_mount *zmp = NULL;
	struct dk_zone sbz[ZLFS_SB_ZONES];
	struct disklabel dl;
	u_int32_t filled;
	dev_t dev = devvp->v_rdev;
	int error;

	if ((error = vfs_mountedon(devvp)) != 0)
		return error;
	if (vcount(devvp) > 1 && devvp != rootvp)
		return EBUSY;

	vn_lock(devvp, LK_EXCLUSIVE | LK_RETRY);
	error = vinvalbuf(devvp, V_SAVE, p->p_ucred, p, 0, INFSLP);
	VOP_UNLOCK(devvp);
	if (error != 0)
		return error;

	error = VOP_OPEN(devvp, FREAD, FSCRED, p);
	if (error != 0)
		return error;

	error = VOP_IOCTL(devvp, DIOCGPDINFO, (caddr_t)&dl, FREAD, FSCRED,
	    p);
	if (error != 0)
		goto out;
	if (dl.d_secsize < DEV_BSIZE ||
	    (dl.d_secsize & (dl.d_secsize - 1)) != 0) {
		error = EINVAL;
		goto out;
	}
	/*
	 * Zone reports carry device-absolute LBAs, while I/O through
	 * devvp is relative to the mounted partition.  Require the
	 * partition to start at LBA 0 so the two coincide.
	 */
	if (DL_GETPOFFSET(&dl.d_partitions[DISKPART(dev)]) != 0) {
		error = EINVAL;
		goto out;
	}

	zmp = malloc(sizeof(*zmp), M_ZLFS, M_WAITOK | M_ZERO);
	rw_init(&zmp->zm_lock, "zlfs");
	rw_init(&zmp->zm_wlock, "zlfswr");
	LIST_INIT(&zmp->zm_nodes);
	zmp->zm_devvp = devvp;
	zmp->zm_dev = dev;
	zmp->zm_secsize = dl.d_secsize;
	zmp->zm_rdonly = (mp->mnt_flag & MNT_RDONLY) != 0;

	/*
	 * The superblock zones are the first two zones on the device;
	 * fetch their descriptors through the in-kernel report API.
	 */
	error = dk_zone_report_kern(dev, 0, sbz, ZLFS_SB_ZONES, &filled);
	if (error != 0)
		goto out;
	if (filled != ZLFS_SB_ZONES || sbz[0].dz_start_lba != 0) {
		error = EINVAL;
		goto out;
	}

	error = zlfs_sb_discover(zmp, sbz, p);
	if (error != 0)
		goto out;

	/*
	 * Cross-check the superblock against the device geometry.
	 * zs_total_zones is attacker-controllable (CRC32C is not a MAC),
	 * so bound it by the actual device size before it is used to
	 * size allocations in zlfs_zones_load().
	 */
	if (zmp->zm_super.zs_zone_size_lba != sbz[0].dz_length_lba ||
	    zmp->zm_super.zs_zone_cap_lba == 0 ||
	    zmp->zm_super.zs_zone_cap_lba > sbz[0].dz_length_lba ||
	    zmp->zm_super.zs_total_zones < ZLFS_SB_ZONES + 1 ||
	    zmp->zm_super.zs_total_zones >
	    DL_GETDSIZE(&dl) / sbz[0].dz_length_lba ||
	    zmp->zm_super.zs_block_size < zmp->zm_secsize) {
		error = EINVAL;
		goto out;
	}

	error = zlfs_zones_load(zmp);
	if (error != 0)
		goto out;

	/*
	 * A nonzero checkpoint pointer names a real filesystem image;
	 * load its inode map.  A zero pointer is a superblock-only image
	 * (older newfs_zlfs), which mounts with just a synthetic empty
	 * root directory.
	 */
	if (zmp->zm_super.zs_checkpoint_lba != 0) {
		error = zlfs_ckpt_load(zmp);
		if (error != 0)
			goto out;
	}

	/* Initialise the write-log heads (needed for a read-write mount). */
	error = zlfs_log_init(zmp, sbz);
	if (error != 0)
		goto out;

	zmp->zm_mountp = mp;
	zmp->zm_ctime = gettime();

	mp->mnt_data = zmp;
	mp->mnt_stat.f_fsid.val[0] = (long)dev;
	mp->mnt_stat.f_fsid.val[1] = mp->mnt_vfc->vfc_typenum;
	mp->mnt_stat.f_namemax = NAME_MAX;
	mp->mnt_flag |= MNT_LOCAL;

	devvp->v_specmountpoint = mp;

	return 0;

out:
	if (devvp->v_specinfo)
		devvp->v_specmountpoint = NULL;

	vn_lock(devvp, LK_EXCLUSIVE | LK_RETRY);
	VOP_CLOSE(devvp, FREAD, NOCRED, p);
	VOP_UNLOCK(devvp);

	if (zmp != NULL) {
		if (zmp->zm_imap != NULL)
			free(zmp->zm_imap, M_ZLFS,
			    zmp->zm_super.zs_block_size);
		zlfs_zones_free(zmp);
		free(zmp, M_ZLFS, sizeof(*zmp));
		mp->mnt_data = NULL;
	}
	return error;
}

int
zlfs_start(struct mount *mp, int flags, struct proc *p)
{
	return 0;
}

int
zlfs_unmount(struct mount *mp, int mntflags, struct proc *p)
{
	struct zlfs_mount *zmp = VFSTOZLFS(mp);
	int error, flags = 0;

	if (mntflags & MNT_FORCE)
		flags |= FORCECLOSE;

	/* Flush any buffered changes before tearing the mount down. */
	if (!zmp->zm_rdonly) {
		error = zlfs_commit(zmp);
		if (error != 0 && (mntflags & MNT_FORCE) == 0)
			return error;
	}

	if ((error = vflush(mp, NULL, flags)) != 0)
		return error;

	zmp = VFSTOZLFS(mp);

	zmp->zm_devvp->v_specmountpoint = NULL;
	vn_lock(zmp->zm_devvp, LK_EXCLUSIVE | LK_RETRY);
	(void)VOP_CLOSE(zmp->zm_devvp, FREAD, NOCRED, p);
	vput(zmp->zm_devvp);

	if (zmp->zm_imap != NULL)
		free(zmp->zm_imap, M_ZLFS, zmp->zm_super.zs_block_size);
	zlfs_zones_free(zmp);
	free(zmp, M_ZLFS, sizeof(*zmp));
	mp->mnt_data = NULL;
	mp->mnt_flag &= ~MNT_LOCAL;
	return 0;
}

int
zlfs_root(struct mount *mp, struct vnode **vpp)
{
	return zlfs_vget(mp, ZLFS_ROOT_INO, vpp);
}

int
zlfs_quotactl(struct mount *mp, int cmds, uid_t uid, caddr_t arg,
    struct proc *p)
{
	return EOPNOTSUPP;
}

int
zlfs_statfs(struct mount *mp, struct statfs *sbp, struct proc *p)
{
	struct zlfs_mount *zmp = VFSTOZLFS(mp);
	u_int64_t bpz;	/* filesystem blocks per zone */

	bpz = (zmp->zm_super.zs_zone_cap_lba * zmp->zm_secsize) >>
	    zmp->zm_bshift;

	sbp->f_bsize = zmp->zm_super.zs_block_size;
	sbp->f_iosize = sbp->f_bsize;
	sbp->f_blocks = (zmp->zm_super.zs_total_zones - ZLFS_SB_ZONES) *
	    bpz;
	sbp->f_bfree = zlfs_zones_empty(zmp) * bpz;
	sbp->f_bavail = sbp->f_bfree;
	sbp->f_files = 1;	/* the synthetic root */
	sbp->f_ffree = 0;
	sbp->f_favail = 0;
	copy_statfs_info(sbp, mp);

	return 0;
}

int
zlfs_sync(struct mount *mp, int waitfor, int stall, struct ucred *cred,
    struct proc *p)
{
	struct zlfs_mount *zmp = VFSTOZLFS(mp);

	if (zmp->zm_rdonly)
		return 0;
	return zlfs_commit(zmp);
}

/*
 * Build the synthetic-root inode used for a superblock-only image
 * (no checkpoint): an empty directory owned by root.
 */
static void
zlfs_synth_root(struct zlfs_mount *zmp, struct zlfs_inode *zi)
{
	memset(zi, 0, sizeof(*zi));
	zi->zi_ino = ZLFS_ROOT_INO;
	zi->zi_gen = 1;
	zi->zi_mode = S_IFDIR | 0755;
	zi->zi_nlink = 2;
	zi->zi_size = 0;
	zi->zi_atime = zi->zi_mtime = zi->zi_ctime = zi->zi_btime =
	    zmp->zm_ctime;
}

/*
 * Look up (or create) the vnode for an inode number, caching active
 * in-core inodes on zm_nodes.  The vnode-cache dance mirrors the
 * ufs/tmpfs pattern: never hold zm_lock across getnewvnode(), and
 * re-check for a racing insert before publishing.
 */
int
zlfs_vget(struct mount *mp, ino_t ino, struct vnode **vpp)
{
	struct zlfs_mount *zmp = VFSTOZLFS(mp);
	struct zlfs_node *znp, *other;
	struct zlfs_inode dinode;
	struct vnode *vp;
	int error;

	*vpp = NULL;

loop:
	rw_enter_write(&zmp->zm_lock);
	LIST_FOREACH(znp, &zmp->zm_nodes, zn_entry) {
		if (znp->zn_ino != ino)
			continue;
		vp = znp->zn_vnode;
		rw_exit_write(&zmp->zm_lock);
		error = vget(vp, LK_EXCLUSIVE);
		if (error == ENOENT)
			goto loop;	/* being reclaimed; retry */
		if (error != 0)
			return error;
		*vpp = vp;
		return 0;
	}
	rw_exit_write(&zmp->zm_lock);

	/*
	 * Cache miss: fetch the on-disk inode (or synthesise the root)
	 * only now, so a newly created inode -- present in the cache but
	 * not yet on disk -- is found above without a spurious read.
	 */
	if (zmp->zm_imap != NULL) {
		error = zlfs_read_dinode(zmp, ino, &dinode);
		if (error != 0)
			return error;
	} else {
		if (ino != ZLFS_ROOT_INO)
			return ENOENT;
		zlfs_synth_root(zmp, &dinode);
	}

	/* Create a fresh vnode without holding zm_lock. */
	error = getnewvnode(VT_ZLFS, mp, &zlfs_vops, &vp);
	if (error != 0)
		return error;

	znp = malloc(sizeof(*znp), M_ZLFS, M_WAITOK | M_ZERO);
	rrw_init_flags(&znp->zn_lock, "zlfsinode", RWL_DUPOK | RWL_IS_VNODE);
	vp->v_data = znp;
	znp->zn_vnode = vp;
	znp->zn_zmp = zmp;
	znp->zn_ino = ino;
	znp->zn_dinode = dinode;
	vp->v_type = IFTOVT(dinode.zi_mode);

	/* Publish, unless another thread beat us to this inode. */
	rw_enter_write(&zmp->zm_lock);
	LIST_FOREACH(other, &zmp->zm_nodes, zn_entry) {
		if (other->zn_ino == ino)
			break;
	}
	if (other != NULL) {
		rw_exit_write(&zmp->zm_lock);
		/* znp was never linked (zn_onlist == 0); dispose safely. */
		vp->v_type = VNON;
		vrele(vp);
		goto loop;
	}
	if (ino == ZLFS_ROOT_INO)
		vp->v_flag |= VROOT;
	LIST_INSERT_HEAD(&zmp->zm_nodes, znp, zn_entry);
	znp->zn_onlist = 1;
	rw_exit_write(&zmp->zm_lock);

	vn_lock(vp, LK_EXCLUSIVE | LK_RETRY);
	*vpp = vp;
	return 0;
}

/*
 * Allocate a brand-new inode and return its locked vnode.  The inode
 * exists only in core (dirty, empty data, no on-disk map entry) until
 * the next commit.  Serialised by the parent directory lock held by the
 * caller.
 */
int
zlfs_ialloc(struct zlfs_mount *zmp, u_int32_t mode, struct vnode **vpp)
{
	struct zlfs_node *znp;
	struct vnode *vp;
	u_int64_t ino, maxino;
	int error;

	*vpp = NULL;
	if (zmp->zm_rdonly)
		return EROFS;
	maxino = zmp->zm_super.zs_block_size / sizeof(u_int64_t);
	if (zmp->zm_next_ino >= maxino)
		return ENOSPC;
	ino = zmp->zm_next_ino;

	error = getnewvnode(VT_ZLFS, zmp->zm_mountp, &zlfs_vops, &vp);
	if (error != 0)
		return error;

	znp = malloc(sizeof(*znp), M_ZLFS, M_WAITOK | M_ZERO);
	rrw_init_flags(&znp->zn_lock, "zlfsinode", RWL_DUPOK | RWL_IS_VNODE);
	vp->v_data = znp;
	znp->zn_vnode = vp;
	znp->zn_zmp = zmp;
	znp->zn_ino = ino;
	znp->zn_dinode.zi_ino = ino;
	znp->zn_dinode.zi_gen = 1;
	znp->zn_dinode.zi_mode = mode;
	znp->zn_dinode.zi_nlink = 1;
	znp->zn_dinode.zi_atime = znp->zn_dinode.zi_mtime =
	    znp->zn_dinode.zi_ctime = znp->zn_dinode.zi_btime = gettime();
	vp->v_type = IFTOVT(mode);

	/*
	 * Start with a one-block buffer; write/setattr/dir_add grow it on
	 * demand up to the maximum file size.  The on-disk map slot fills
	 * at the next commit.
	 */
	znp->zn_dataalloc = zmp->zm_super.zs_block_size;
	znp->zn_data = malloc(znp->zn_dataalloc, M_ZLFS, M_WAITOK | M_ZERO);
	znp->zn_datalen = 0;
	zlfs_node_dirty(znp);

	rw_enter_write(&zmp->zm_lock);
	zmp->zm_next_ino = ino + 1;
	if (ino + 1 > zmp->zm_ninodes)
		zmp->zm_ninodes = ino + 1;
	zmp->zm_imap[ino] = 0;
	LIST_INSERT_HEAD(&zmp->zm_nodes, znp, zn_entry);
	znp->zn_onlist = 1;
	rw_exit_write(&zmp->zm_lock);

	vn_lock(vp, LK_EXCLUSIVE | LK_RETRY);
	*vpp = vp;
	return 0;
}

int
zlfs_fhtovp(struct mount *mp, struct fid *fhp, struct vnode **vpp)
{
	return EOPNOTSUPP;
}

int
zlfs_vptofh(struct vnode *vp, struct fid *fhp)
{
	return EOPNOTSUPP;
}

int
zlfs_init(struct vfsconf *vfsp)
{
	zlfs_crc32c_init();
	return 0;
}

int
zlfs_sysctl(int *name, u_int namelen, void *oldp, size_t *oldlenp,
    void *newp, size_t newlen, struct proc *p)
{
	return EOPNOTSUPP;
}

int
zlfs_checkexp(struct mount *mp, struct mbuf *nam, int *extflagsp,
    struct ucred **credanonp)
{
	return EOPNOTSUPP;
}

const struct vfsops zlfs_vfsops = {
	.vfs_mount	= zlfs_mount,
	.vfs_start	= zlfs_start,
	.vfs_unmount	= zlfs_unmount,
	.vfs_root	= zlfs_root,
	.vfs_quotactl	= zlfs_quotactl,
	.vfs_statfs	= zlfs_statfs,
	.vfs_sync	= zlfs_sync,
	.vfs_vget	= zlfs_vget,
	.vfs_fhtovp	= zlfs_fhtovp,
	.vfs_vptofh	= zlfs_vptofh,
	.vfs_init	= zlfs_init,
	.vfs_sysctl	= zlfs_sysctl,
	.vfs_checkexp	= zlfs_checkexp,
};
