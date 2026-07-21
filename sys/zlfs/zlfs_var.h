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

struct zlfs_node;
LIST_HEAD(zlfs_node_list, zlfs_node);

struct zlfs_mount {
	struct mount	*zm_mountp;
	struct vnode	*zm_devvp;
	struct rwlock	 zm_lock;	/* guards zm_nodes and vnode creation */
	struct rwlock	 zm_wlock;	/* serialises the write/commit path */
	struct zlfs_node_list zm_nodes;	/* active in-core inodes */
	dev_t		 zm_dev;
	struct zlfs_super zm_super;	/* host-endian superblock copy */
	u_int32_t	 zm_secsize;	/* device sector (LBA) size */
	u_int32_t	 zm_bshift;	/* log2(zs_block_size) */
	u_int64_t	 zm_nzones;	/* zones loaded into zm_zones */
	time_t		 zm_ctime;	/* mount time, for synthetic attrs */
	struct zlfs_zone_state *zm_zones;
	u_int64_t	*zm_imap;	/* inode -> device LBA, or NULL */
	u_int64_t	 zm_ninodes;	/* number of zm_imap entries */
	size_t		 zm_imap_alloc;	/* bytes allocated for zm_imap */
	int		 zm_rdonly;	/* mounted read-only */
	int		 zm_faultpoint;	/* TEST: armed power-cut stage, or 0 */
	int		 zm_dead;	/* TEST: fault fired; no write may
					   reach the device anymore */

	/* Data-log head (append point for new data/metadata blocks). */
	u_int64_t	 zm_log_lba;	/* next free device LBA */
	u_int64_t	 zm_log_zend;	/* end LBA of the current log zone */
	u_int64_t	 zm_log_zidx;	/* current data zone index */

	/* Superblock-log head (generation ping-pong across zones 0-1). */
	u_int64_t	 zm_sb_lba;	/* next free LBA in the active SB zone */
	u_int64_t	 zm_sb_zstart[ZLFS_SB_ZONES]; /* SB zone start LBAs */
	u_int64_t	 zm_sb_zcap;	/* usable LBAs per SB zone */
	int		 zm_sb_zidx;	/* active SB zone (0 or 1) */

	struct netexport zm_export;
};

struct zlfs_node {
	LIST_ENTRY(zlfs_node) zn_entry;	/* zm_nodes linkage */
	struct rrwlock	 zn_lock;
	struct vnode	*zn_vnode;
	struct zlfs_mount *zn_zmp;
	u_int64_t	 zn_ino;
	int		 zn_onlist;	/* linked into zm_nodes? */
	int		 zn_dirty;	/* needs commit */
	u_int8_t	*zn_data;	/* VDIR: in-core contents, or NULL */
	size_t		 zn_datalen;	/* valid bytes in zn_data */
	size_t		 zn_dataalloc;	/* bytes allocated for zn_data */
	/*
	 * VREG: sparse per-block dirty overlay.  zn_dblk[b] is a
	 * block-sized buffer holding block b's new contents, or NULL if
	 * the block is clean (read from disk via the inode's pointers).
	 * Clean blocks keep their on-disk LBAs across a commit.
	 */
	u_int8_t	**zn_dblk;	/* dirty block buffers, or NULL */
	u_int32_t	 zn_ndblk;	/* slots in zn_dblk */
	u_int32_t	 zn_dblkcnt;	/* materialised (non-NULL) buffers */
	int		 zn_relocate;	/* GC: rewrite indirect blocks even
					   if unchanged, to move them off a
					   zone being compacted */
	struct zlfs_inode zn_dinode;	/* host-endian inode copy */
};

/* Data-block pointers held in one single-indirect block. */
#define ZLFS_NINDIR(zmp) \
	((u_int64_t)(zmp)->zm_super.zs_block_size / sizeof(u_int64_t))

/* Total inode numbers the multi-block map can address. */
#define ZLFS_MAXINO(zmp) \
	((u_int64_t)ZLFS_CKPT_NIMAP((zmp)->zm_super.zs_block_size) * \
	    ((zmp)->zm_super.zs_block_size / sizeof(u_int64_t)))

/*
 * Largest file the ON-DISK FORMAT supports: the direct blocks, one
 * single-indirect block, one double-indirect tree, and one
 * triple-indirect tree (about 513 GB at a 4 KB block size).
 */
#define ZLFS_TREEMAXSZ(zmp) \
	((ZLFS_NDADDR + ZLFS_NINDIR(zmp) + \
	    ZLFS_NINDIR(zmp) * ZLFS_NINDIR(zmp) + \
	    ZLFS_NINDIR(zmp) * ZLFS_NINDIR(zmp) * ZLFS_NINDIR(zmp)) * \
	    (u_int64_t)(zmp)->zm_super.zs_block_size)

/*
 * Largest file the WRITE PATH admits.  The dirty overlay indexes
 * blocks with a dense pointer array sized to the highest dirty block
 * number, so writable size is bounded by what malloc(9) can serve for
 * that index, not by the format: at the format bound the index alone
 * would need a ~1 GB allocation.  16 GB keeps the worst-case index at
 * 32 MiB (4 KB blocks) and stays far beyond any tested device.  Reads
 * and the GC walk the full format regardless of this cap; a file
 * beyond it (foreign writer) is readable and shrinkable but is
 * skipped by the compactor.
 *
 * The overlay BUFFERS are bounded separately: once a file holds
 * ZLFS_DBLK_MAXBUFS materialised blocks (64 MB at 4 KB) the write and
 * truncate paths commit before materialising more, so a large write
 * or truncate-up streams through bounded memory instead of buffering
 * everything (gap blocks included) in core.
 */
#define ZLFS_MAXFILESZ_CAP	(16ULL << 30)
#define ZLFS_MAXFILESZ(zmp) \
	MIN(ZLFS_TREEMAXSZ(zmp), ZLFS_MAXFILESZ_CAP)
#define ZLFS_DBLK_MAXBUFS	16384

/*
 * Directories are committed through the whole-contents (zn_data) path,
 * which writes direct plus single-indirect blocks only, so their growth
 * must stop at that bound (16k+ entries at a 4 KB block).
 */
#define ZLFS_MAXDIRSZ(zmp) \
	((ZLFS_NDADDR + ZLFS_NINDIR(zmp)) * \
	    (u_int64_t)(zmp)->zm_super.zs_block_size)

#define VFSTOZLFS(mp)	((struct zlfs_mount *)((mp)->mnt_data))
#define VTOZ(vp)	((struct zlfs_node *)(vp)->v_data)

/* Byte offset of a device LBA. */
#define ZLFS_LBATOB(zmp, lba)	((lba) * (u_int64_t)(zmp)->zm_secsize)

/* Index of the device zone containing an LBA (zones are uniform). */
#define ZLFS_ZONEOF(zmp, lba)	((lba) / (zmp)->zm_super.zs_zone_size_lba)

/*
 * Free-data-zone thresholds.  Below MIN_FREE a commit resets any fully
 * dead zones first; below MIN_COMPACT (higher, so it triggers earlier)
 * the sync path relocates a mixed zone's live blocks so a dead zone can
 * then be reclaimed.
 */
#define ZLFS_GC_MIN_FREE	2
#define ZLFS_GC_MIN_COMPACT	4

/*
 * TEST power-cut stages for the -o faultpoint=N mount option.  When
 * the armed stage is reached the mount goes dead: the hook and every
 * later write return failure, exactly as if power was lost there.
 * The sync/fsync that fires the fault reports EIO (a crashing commit
 * IS a failed commit); afterwards the mount is frozen -- syncs
 * succeed as no-ops so unmount works, and writes fail EIO rather
 * than accumulate unbounded overlay memory.  Recovery is a clean
 * remount.
 */
#define ZLFS_FAULT_SEG		1	/* at commit start: nothing written */
#define ZLFS_FAULT_CKPT		2	/* segment written, checkpoint dropped */
#define ZLFS_FAULT_SB		3	/* checkpoint written, superblock dropped */
#define ZLFS_FAULT_AFTER	4	/* full commit, then dead */

extern const struct vops zlfs_vops;

/* zlfs_vfsops.c */
int		zlfs_ialloc(struct zlfs_mount *, u_int32_t, struct vnode **);

/* zlfs_subr.c */
void		zlfs_crc32c_init(void);
u_int32_t	zlfs_crc32c_update(u_int32_t, const void *, size_t);
#define ZLFS_CRC32C_INITIAL	0xffffffffU
#define ZLFS_CRC32C_FINAL(c)	((c) ^ 0xffffffffU)
void		zlfs_sb_letoh(struct zlfs_super *, const struct zlfs_super *);
void		zlfs_inode_htole(struct zlfs_inode *, const struct zlfs_inode *);
int		zlfs_sb_discover(struct zlfs_mount *, const struct dk_zone *,
		    struct proc *);
int		zlfs_ckpt_load(struct zlfs_mount *);
int		zlfs_read_dinode(struct zlfs_mount *, u_int64_t,
		    struct zlfs_inode *);
int		zlfs_read_dinode_at(struct zlfs_mount *, u_int64_t,
		    struct zlfs_inode *);
void		zlfs_cache_purge_dev(struct zlfs_mount *);
int		zlfs_bread_block(struct zlfs_mount *, u_int64_t, struct buf **);

/* zlfs_alloc.c */
int		zlfs_zones_load(struct zlfs_mount *);
void		zlfs_zones_free(struct zlfs_mount *);
u_int64_t	zlfs_zones_freecount(struct zlfs_mount *);
int		zlfs_log_init(struct zlfs_mount *, const struct dk_zone *);
int		zlfs_compact(struct zlfs_mount *);
int		zlfs_alloc_block(struct zlfs_mount *, u_int64_t *);
void		zlfs_clean(struct zlfs_mount *);

/* zlfs_write.c */
int		zlfs_fault(struct zlfs_mount *, int);
int		zlfs_write_block(struct zlfs_mount *, u_int64_t, const void *);
int		zlfs_bmap_read(struct zlfs_node *, u_int64_t, u_int64_t *);
int		zlfs_node_load(struct zlfs_node *);
int		zlfs_node_owns_range(struct zlfs_node *, u_int64_t, u_int64_t);
int		zlfs_node_owns_meta(struct zlfs_node *, u_int64_t, u_int64_t);
int		zlfs_node_resize(struct zlfs_node *, size_t);
int		zlfs_dblk_prepare(struct zlfs_node *, u_int64_t, int);
int		zlfs_dblk_backpressure(struct zlfs_node *);
void		zlfs_dblk_free(struct zlfs_node *);
void		zlfs_node_dirty(struct zlfs_node *);
int		zlfs_commit(struct zlfs_mount *);

#endif /* _KERNEL */
#endif /* _ZLFS_ZLFS_VAR_H_ */
