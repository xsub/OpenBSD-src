/*	$OpenBSD$	*/

/*
 * ZLFS in-memory structures.  Kernel-internal; the on-disk format
 * lives in <sys/zlfs.h>.
 */

#ifndef _ZLFS_ZLFS_VAR_H_
#define _ZLFS_ZLFS_VAR_H_

#ifdef _KERNEL

#include <sys/mount.h>		/* struct netexport, embedded by value */
#include <sys/rwlock.h>
#include <sys/zlfs.h>

struct dk_zone;
struct proc;

/*
 * In-memory state for tracking one zone.
 */
struct zlfs_zone_state {
	u_int64_t	 zst_start_lba;
	u_int64_t	 zst_wp_lba;	/* current write pointer */
	u_int64_t	 zst_cap_lba;	/* usable capacity */
	u_int32_t	 zst_cond;	/* DK_ZONE_COND_* */
	u_int32_t	 zst_flags;
	u_int64_t	 zst_live_bytes; /* garbage collection heuristics */
};

struct zlfs_mount {
	struct mount	*zm_mountp;
	struct vnode	*zm_devvp;
	struct rwlock	 zm_rootlk;	/* serialises root vnode creation */
	struct vnode	*zm_rootvp;	/* cached root vnode, or NULL */
	dev_t		 zm_dev;
	struct zlfs_super zm_super;	/* host-endian superblock copy */
	u_int32_t	 zm_secsize;	/* device sector (LBA) size */
	u_int32_t	 zm_bshift;	/* log2(zs_block_size) */
	u_int64_t	 zm_nzones;	/* zones loaded into zm_zones */
	time_t		 zm_ctime;	/* mount time, for synthetic attrs */
	struct zlfs_zone_state *zm_zones;
	struct netexport zm_export;
};

struct zlfs_node {
	struct rrwlock	 zn_lock;
	struct vnode	*zn_vnode;
	struct zlfs_mount *zn_zmp;
	u_int64_t	 zn_ino;
	struct zlfs_inode zn_dinode;	/* host-endian inode copy */
};

#define VFSTOZLFS(mp)	((struct zlfs_mount *)((mp)->mnt_data))
#define VTOZ(vp)	((struct zlfs_node *)(vp)->v_data)

/* Byte offset of a device LBA. */
#define ZLFS_LBATOB(zmp, lba)	((lba) * (u_int64_t)(zmp)->zm_secsize)

extern const struct vops zlfs_vops;

/* zlfs_subr.c */
void		zlfs_crc32c_init(void);
u_int32_t	zlfs_crc32c_update(u_int32_t, const void *, size_t);
#define ZLFS_CRC32C_INITIAL	0xffffffffU
#define ZLFS_CRC32C_FINAL(c)	((c) ^ 0xffffffffU)
void		zlfs_sb_letoh(struct zlfs_super *, const struct zlfs_super *);
int		zlfs_sb_discover(struct zlfs_mount *, const struct dk_zone *,
		    struct proc *);

/* zlfs_alloc.c */
int		zlfs_zones_load(struct zlfs_mount *);
void		zlfs_zones_free(struct zlfs_mount *);
u_int64_t	zlfs_zones_empty(struct zlfs_mount *);

#endif /* _KERNEL */
#endif /* _ZLFS_ZLFS_VAR_H_ */
