/*	$OpenBSD$	*/

/*
 * Zoned Log-Structured File System (ZLFS).
 *
 * Skeleton only: the filesystem registers with the VFS so kernel
 * plumbing can be exercised, but every operation is rejected until
 * the on-disk format (sys/sys/zlfs.h) is final.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/namei.h>
#include <sys/proc.h>
#include <sys/vnode.h>
#include <sys/mount.h>
#include <sys/buf.h>
#include <sys/malloc.h>
#include <sys/zlfs.h>

static int	zlfs_mount(struct mount *, const char *, void *,
		    struct nameidata *, struct proc *);
static int	zlfs_start(struct mount *, int, struct proc *);
static int	zlfs_unmount(struct mount *, int, struct proc *);
static int	zlfs_root(struct mount *, struct vnode **);
static int	zlfs_quotactl(struct mount *, int, uid_t, caddr_t,
		    struct proc *);
static int	zlfs_statfs(struct mount *, struct statfs *, struct proc *);
static int	zlfs_sync(struct mount *, int, int, struct ucred *,
		    struct proc *);
static int	zlfs_vget(struct mount *, ino_t, struct vnode **);
static int	zlfs_fhtovp(struct mount *, struct fid *, struct vnode **);
static int	zlfs_vptofh(struct vnode *, struct fid *);
static int	zlfs_init(struct vfsconf *);
static int	zlfs_sysctl(int *, u_int, void *, size_t *, void *, size_t,
		    struct proc *);
static int	zlfs_checkexp(struct mount *, struct mbuf *, int *,
		    struct ucred **);

static int
zlfs_mount(struct mount *mp, const char *path, void *data,
    struct nameidata *ndp, struct proc *p)
{
	return EOPNOTSUPP;
}

static int
zlfs_start(struct mount *mp, int flags, struct proc *p)
{
	return 0;
}

static int
zlfs_unmount(struct mount *mp, int mntflags, struct proc *p)
{
	return EOPNOTSUPP;
}

static int
zlfs_root(struct mount *mp, struct vnode **vpp)
{
	return EOPNOTSUPP;
}

static int
zlfs_quotactl(struct mount *mp, int cmds, uid_t uid, caddr_t arg,
    struct proc *p)
{
	return EOPNOTSUPP;
}

static int
zlfs_statfs(struct mount *mp, struct statfs *sbp, struct proc *p)
{
	return EOPNOTSUPP;
}

static int
zlfs_sync(struct mount *mp, int waitfor, int stall, struct ucred *cred,
    struct proc *p)
{
	return 0;
}

static int
zlfs_vget(struct mount *mp, ino_t ino, struct vnode **vpp)
{
	return EOPNOTSUPP;
}

static int
zlfs_fhtovp(struct mount *mp, struct fid *fhp, struct vnode **vpp)
{
	return EOPNOTSUPP;
}

static int
zlfs_vptofh(struct vnode *vp, struct fid *fhp)
{
	return EOPNOTSUPP;
}

static int
zlfs_init(struct vfsconf *vfsp)
{
	return 0;
}

static int
zlfs_sysctl(int *name, u_int namelen, void *oldp, size_t *oldlenp,
    void *newp, size_t newlen, struct proc *p)
{
	return EOPNOTSUPP;
}

static int
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
