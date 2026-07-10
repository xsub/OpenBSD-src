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

	if ((mp->mnt_flag & MNT_RDONLY) == 0)
		return EROFS;

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
	rw_init(&zmp->zm_rootlk, "zlfsroot");
	zmp->zm_devvp = devvp;
	zmp->zm_dev = dev;
	zmp->zm_secsize = dl.d_secsize;

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
	struct zlfs_mount *zmp;
	int error, flags = 0;

	if (mntflags & MNT_FORCE)
		flags |= FORCECLOSE;

	if ((error = vflush(mp, NULL, flags)) != 0)
		return error;

	zmp = VFSTOZLFS(mp);

	zmp->zm_devvp->v_specmountpoint = NULL;
	vn_lock(zmp->zm_devvp, LK_EXCLUSIVE | LK_RETRY);
	(void)VOP_CLOSE(zmp->zm_devvp, FREAD, NOCRED, p);
	vput(zmp->zm_devvp);

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
	return 0;
}

/*
 * Until the checkpoint and inode map formats exist, the only inode is
 * a synthetic empty root directory.
 */
int
zlfs_vget(struct mount *mp, ino_t ino, struct vnode **vpp)
{
	struct zlfs_mount *zmp = VFSTOZLFS(mp);
	struct zlfs_node *znp;
	struct zlfs_inode *zi;
	struct vnode *vp;
	int error;

	if (ino != ZLFS_ROOT_INO)
		return ENOENT;

again:
	if (zmp->zm_rootvp != NULL) {
		vp = zmp->zm_rootvp;
		if (vget(vp, LK_EXCLUSIVE) != 0)
			goto again;
		*vpp = vp;
		return 0;
	}

	/* Serialise creation so only one root vnode is ever published. */
	rw_enter_write(&zmp->zm_rootlk);
	if (zmp->zm_rootvp != NULL) {
		rw_exit_write(&zmp->zm_rootlk);
		goto again;
	}

	error = getnewvnode(VT_ZLFS, mp, &zlfs_vops, &vp);
	if (error != 0) {
		rw_exit_write(&zmp->zm_rootlk);
		return error;
	}

	znp = malloc(sizeof(*znp), M_ZLFS, M_WAITOK | M_ZERO);
	rrw_init_flags(&znp->zn_lock, "zlfsinode", RWL_DUPOK | RWL_IS_VNODE);
	vp->v_data = znp;
	znp->zn_vnode = vp;
	znp->zn_zmp = zmp;
	znp->zn_ino = ino;

	zi = &znp->zn_dinode;
	zi->zi_ino = ino;
	zi->zi_gen = 1;
	zi->zi_mode = S_IFDIR | 0755;
	zi->zi_nlink = 2;
	zi->zi_size = zmp->zm_super.zs_block_size;
	zi->zi_atime = zi->zi_mtime = zi->zi_ctime = zi->zi_btime =
	    zmp->zm_ctime;

	vp->v_type = VDIR;
	vp->v_flag |= VROOT;

	vn_lock(vp, LK_EXCLUSIVE | LK_RETRY);
	zmp->zm_rootvp = vp;
	rw_exit_write(&zmp->zm_rootlk);
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
