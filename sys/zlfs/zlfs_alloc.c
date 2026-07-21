/*	$OpenBSD$	*/

/*
 * ZLFS zone allocator.
 *
 * Read-only bring-up: load a zlfs_zone_state entry for every device
 * zone through the in-kernel zone report API, validating the device
 * geometry against the superblock.  Allocation and cleaning policy
 * come with the write path.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/buf.h>
#include <sys/endian.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/vnode.h>
#include <sys/dkio.h>
#include <sys/zlfs.h>

#include <zlfs/zlfs_var.h>

/* Zones fetched per dk_zone_report_kern() call. */
#define ZLFS_ZONE_CHUNK	64

int
zlfs_zones_load(struct zlfs_mount *zmp)
{
	struct dk_zone *chunk;
	struct zlfs_zone_state *zst;
	u_int64_t next_lba = 0, want = zmp->zm_super.zs_total_zones;
	u_int32_t filled, i;
	int error = 0;

	zmp->zm_zones = mallocarray(want, sizeof(*zmp->zm_zones), M_ZLFS,
	    M_WAITOK | M_ZERO);
	zmp->zm_nzones = 0;

	chunk = mallocarray(ZLFS_ZONE_CHUNK, sizeof(*chunk), M_TEMP,
	    M_WAITOK);

	while (zmp->zm_nzones < want) {
		error = dk_zone_report_kern(zmp->zm_dev, next_lba, chunk,
		    MIN(want - zmp->zm_nzones, ZLFS_ZONE_CHUNK), &filled);
		if (error != 0)
			break;
		if (filled == 0) {
			/* device has fewer zones than the superblock */
			error = EINVAL;
			break;
		}

		for (i = 0; i < filled; i++) {
			const struct dk_zone *z = &chunk[i];

			if (z->dz_length_lba !=
			    zmp->zm_super.zs_zone_size_lba ||
			    z->dz_capacity_lba <
			    zmp->zm_super.zs_zone_cap_lba) {
				error = EINVAL;
				goto done;
			}

			zst = &zmp->zm_zones[zmp->zm_nzones];
			zst->zst_start_lba = z->dz_start_lba;
			zst->zst_wp_lba = z->dz_write_pointer_lba;
			zst->zst_cap_lba = z->dz_capacity_lba;
			zst->zst_cond = z->dz_condition;
			zst->zst_flags = 0;
			zst->zst_live_bytes = 0;
			zmp->zm_nzones++;

			next_lba = z->dz_start_lba + z->dz_length_lba;
		}
	}

done:
	free(chunk, M_TEMP, ZLFS_ZONE_CHUNK * sizeof(*chunk));
	if (error != 0)
		zlfs_zones_free(zmp);
	return error;
}

void
zlfs_zones_free(struct zlfs_mount *zmp)
{
	if (zmp->zm_zones != NULL) {
		free(zmp->zm_zones, M_ZLFS,
		    zmp->zm_super.zs_total_zones * sizeof(*zmp->zm_zones));
		zmp->zm_zones = NULL;
		zmp->zm_nzones = 0;
	}
}

/*
 * Number of data zones the allocator can still switch into: those whose
 * write pointer is still at the zone start (never written or reclaimed).
 * The current log head zone is excluded even when still unwritten -- its
 * remaining room is what (zm_log_zend - zm_log_lba) measures, and
 * counting it here too would tally the same space twice.
 */
u_int64_t
zlfs_zones_freecount(struct zlfs_mount *zmp)
{
	struct zlfs_zone_state *zst;
	u_int64_t i, n = 0;

	for (i = ZLFS_SB_ZONES; i < zmp->zm_nzones; i++) {
		if (i == zmp->zm_log_zidx)
			continue;
		zst = &zmp->zm_zones[i];
		if (zst->zst_wp_lba == zst->zst_start_lba)
			n++;
	}
	return n;
}

/* Mark the data zone that holds lba as containing live data. */
static void
zlfs_gc_mark(struct zlfs_mount *zmp, u_int64_t lba)
{
	u_int64_t z;

	if (lba == 0)
		return;
	z = ZLFS_ZONEOF(zmp, lba);
	if (z >= ZLFS_SB_ZONES && z < zmp->zm_nzones)
		zmp->zm_zones[z].zst_live_bytes += zmp->zm_super.zs_block_size;
}

/*
 * Mark every data and indirect block a (host-endian) inode references.
 * Returns nonzero if the indirect block cannot be read, in which case
 * the live set is incomplete and nothing may be reclaimed.
 */
static int
zlfs_gc_mark_blocks(struct zlfs_mount *zmp, const struct zlfs_inode *zi)
{
	struct buf *bp, *mbp, *lbp;
	u_int64_t j, k, m, l1, mid, leaf, nindir = ZLFS_NINDIR(zmp);
	int error;

	for (j = 0; j < ZLFS_NDADDR; j++)
		zlfs_gc_mark(zmp, zi->zi_db[j]);
	if (zi->zi_ib[0] != 0) {
		zlfs_gc_mark(zmp, zi->zi_ib[0]);
		error = zlfs_bread_block(zmp, zi->zi_ib[0], &bp);
		if (error != 0)
			return error;
		for (j = 0; j < nindir; j++)
			zlfs_gc_mark(zmp,
			    letoh64(((u_int64_t *)bp->b_data)[j]));
		brelse(bp);
	}
	if (zi->zi_ib[1] != 0) {
		zlfs_gc_mark(zmp, zi->zi_ib[1]);
		error = zlfs_bread_block(zmp, zi->zi_ib[1], &bp);
		if (error != 0)
			return error;
		for (j = 0; j < nindir; j++) {
			l1 = letoh64(((u_int64_t *)bp->b_data)[j]);
			if (l1 == 0)
				continue;
			/* An entry aliasing a buffer this walk already
			 * holds B_BUSY would self-deadlock in bread;
			 * such metadata is corrupt -- reclaim nothing. */
			if (l1 == zi->zi_ib[1]) {
				brelse(bp);
				return EFTYPE;
			}
			zlfs_gc_mark(zmp, l1);
			error = zlfs_bread_block(zmp, l1, &mbp);
			if (error != 0) {
				brelse(bp);
				return error;
			}
			for (k = 0; k < nindir; k++)
				zlfs_gc_mark(zmp,
				    letoh64(((u_int64_t *)mbp->b_data)[k]));
			brelse(mbp);
		}
		brelse(bp);
	}
	if (zi->zi_ib[2] != 0) {
		/* Triple tree: top -> mid -> leaf -> data. */
		zlfs_gc_mark(zmp, zi->zi_ib[2]);
		error = zlfs_bread_block(zmp, zi->zi_ib[2], &bp);
		if (error != 0)
			return error;
		for (j = 0; j < nindir; j++) {
			mid = letoh64(((u_int64_t *)bp->b_data)[j]);
			if (mid == 0)
				continue;
			if (mid == zi->zi_ib[2]) {
				brelse(bp);
				return EFTYPE;	/* held-buffer alias */
			}
			zlfs_gc_mark(zmp, mid);
			error = zlfs_bread_block(zmp, mid, &mbp);
			if (error != 0) {
				brelse(bp);
				return error;
			}
			for (k = 0; k < nindir; k++) {
				leaf = letoh64(((u_int64_t *)mbp->b_data)[k]);
				if (leaf == 0)
					continue;
				if (leaf == zi->zi_ib[2] || leaf == mid) {
					brelse(mbp);
					brelse(bp);
					return EFTYPE;	/* held-buffer alias */
				}
				zlfs_gc_mark(zmp, leaf);
				error = zlfs_bread_block(zmp, leaf, &lbp);
				if (error != 0) {
					brelse(mbp);
					brelse(bp);
					return error;
				}
				for (m = 0; m < nindir; m++)
					zlfs_gc_mark(zmp, letoh64(
					    ((u_int64_t *)lbp->b_data)[m]));
				brelse(lbp);
			}
			brelse(mbp);
		}
		brelse(bp);
	}
	return 0;
}

/*
 * Mark the inode block at lba and everything the inode it holds
 * references.  Returns nonzero if any read fails.
 */
static int
zlfs_gc_mark_inode(struct zlfs_mount *zmp, u_int64_t lba)
{
	struct zlfs_inode di;
	int error;

	zlfs_gc_mark(zmp, lba);
	error = zlfs_read_dinode_at(zmp, lba, &di);
	if (error != 0)
		return error;
	return zlfs_gc_mark_blocks(zmp, &di);
}

/*
 * Compute per-zone liveness (zst_live_bytes) as the union of three live
 * sets: the durable checkpoint re-read from disk (what a crash recovery
 * reaches -- the in-core map is not a substitute, since unlink/rename
 * clear entries before the next commit and a failed commit leaves stale
 * ones), the in-core inode map, and every in-core vnode.  Returns
 * nonzero if any read failed, in which case the accounting is
 * incomplete and callers must not reclaim or relocate anything.
 */
static int
zlfs_gc_mark_all(struct zlfs_mount *zmp)
{
	struct zlfs_node *znp;
	struct buf *bp;
	const struct zlfs_checkpoint *dc;
	u_int64_t *dimap, *lbas;
	u_int32_t bsize = zmp->zm_super.zs_block_size;
	u_int64_t i, j, ino, lba, nblocks, epb = bsize / sizeof(u_int64_t);

	for (i = 0; i < zmp->zm_nzones; i++)
		zmp->zm_zones[i].zst_live_bytes = 0;

	/* Metadata root: the durable checkpoint block. */
	zlfs_gc_mark(zmp, zmp->zm_super.zs_checkpoint_lba);

	/*
	 * Pass 1: the durable live set.  Re-read the checkpoint block to
	 * find the inode-map blocks it references, mark them, and walk
	 * every inode they name.
	 */
	if (zmp->zm_super.zs_checkpoint_lba != 0) {
		if (zlfs_bread_block(zmp, zmp->zm_super.zs_checkpoint_lba,
		    &bp) != 0)
			return 1;
		dc = (const struct zlfs_checkpoint *)bp->b_data;
		nblocks = letoh64(dc->zc_imap_nblocks);
		if (nblocks == 0 || nblocks > ZLFS_CKPT_NIMAP(bsize)) {
			brelse(bp);
			return 1;		/* unusable map; reclaim nothing */
		}
		lbas = mallocarray(nblocks, sizeof(u_int64_t), M_TEMP,
		    M_WAITOK);
		for (j = 0; j < nblocks; j++)
			lbas[j] = letoh64(((const u_int64_t *)
			    ((const u_int8_t *)bp->b_data +
			    sizeof(struct zlfs_checkpoint)))[j]);
		brelse(bp);

		dimap = malloc(bsize, M_TEMP, M_WAITOK);
		for (j = 0; j < nblocks; j++) {
			if (lbas[j] == 0)
				continue;
			zlfs_gc_mark(zmp, lbas[j]);
			if (zlfs_bread_block(zmp, lbas[j], &bp) != 0)
				goto pass1_fail;
			memcpy(dimap, bp->b_data, bsize);
			brelse(bp);
			for (ino = 0; ino < epb; ino++) {
				lba = letoh64(dimap[ino]);
				if (lba == 0)
					continue;
				if (zlfs_gc_mark_inode(zmp, lba) != 0)
					goto pass1_fail;
			}
		}
		free(dimap, M_TEMP, bsize);
		free(lbas, M_TEMP, nblocks * sizeof(u_int64_t));
		goto pass1_done;
pass1_fail:
		free(dimap, M_TEMP, bsize);
		free(lbas, M_TEMP, nblocks * sizeof(u_int64_t));
		return 1;
	}
pass1_done:

	/* Pass 2: the in-core live set, through zm_imap.  Snapshot each
	 * entry under zm_lock (the map may be grown/replaced) and read
	 * the inode outside it. */
	for (ino = 0;; ino++) {
		rw_enter_read(&zmp->zm_lock);
		lba = (ino < zmp->zm_ninodes) ? zmp->zm_imap[ino] : 0;
		if (ino >= zmp->zm_ninodes) {
			rw_exit_read(&zmp->zm_lock);
			break;
		}
		rw_exit_read(&zmp->zm_lock);
		if (lba == 0)
			continue;
		if (zlfs_gc_mark_inode(zmp, lba) != 0)
			return 1;
	}

	/*
	 * Pass 3: blocks still reachable through in-core vnodes.  An
	 * unlinked-but-open file is in neither map once its removal has
	 * been committed, yet the open vnode still reads its old blocks
	 * from disk, so mark everything each in-core inode points at.
	 * The marking sleeps (bread), so snapshot the inodes under
	 * zm_lock first -- the list may change while we sleep.
	 */
	{
		struct zlfs_inode *snap = NULL;
		u_int64_t nsnap = 0, cap = 0, n, s;

		/*
		 * Count and fill under one zm_lock hold, with a headroom
		 * retry when the list grew while we slept in malloc --
		 * a node prepended during that sleep must not displace an
		 * older one (the unlinked-but-open case pass 3 exists for)
		 * out of a capped snapshot.
		 */
		for (;;) {
			rw_enter_read(&zmp->zm_lock);
			n = 0;
			LIST_FOREACH(znp, &zmp->zm_nodes, zn_entry)
				n++;
			if (n == 0) {
				rw_exit_read(&zmp->zm_lock);
				free(snap, M_TEMP, cap * sizeof(*snap));
				goto pass3_done;
			}
			if (snap == NULL || n > cap) {
				rw_exit_read(&zmp->zm_lock);
				free(snap, M_TEMP, cap * sizeof(*snap));
				cap = n + 8;
				snap = mallocarray(cap, sizeof(*snap),
				    M_TEMP, M_WAITOK);
				continue;
			}
			LIST_FOREACH(znp, &zmp->zm_nodes, zn_entry) {
				if (nsnap >= cap)
					break;
				snap[nsnap++] = znp->zn_dinode;
			}
			rw_exit_read(&zmp->zm_lock);
			break;
		}
		for (s = 0; s < nsnap; s++) {
			if (zlfs_gc_mark_blocks(zmp, &snap[s]) != 0) {
				free(snap, M_TEMP, cap * sizeof(*snap));
				return 1;
			}
		}
		free(snap, M_TEMP, cap * sizeof(*snap));
	}
pass3_done:

	return 0;
}

void
zlfs_clean(struct zlfs_mount *zmp)
{
	struct zlfs_zone_state *zst;
	u_int64_t i;
	int error, nreset = 0;

	if (zmp->zm_dead)
		return;		/* TEST: powered off; no resets either */
	if (zlfs_gc_mark_all(zmp) != 0)
		return;

	/*
	 * Reset any written-but-dead data zone.  The log head is spared
	 * only while it still has room; an exhausted head (including the
	 * placeholder used when mounting a full filesystem) protects
	 * nothing and may itself be reclaimed.
	 */
	for (i = ZLFS_SB_ZONES; i < zmp->zm_nzones; i++) {
		if (i == zmp->zm_log_zidx &&
		    zmp->zm_log_lba < zmp->zm_log_zend)
			continue;
		zst = &zmp->zm_zones[i];
		if (zst->zst_wp_lba == zst->zst_start_lba)
			continue;		/* already empty */
		if (zst->zst_live_bytes != 0)
			continue;		/* still holds live data */

		error = dk_zone_reset_kern(zmp->zm_dev, zst->zst_start_lba);
		if (error != 0)
			continue;		/* best effort */
		zst->zst_wp_lba = zst->zst_start_lba;
		zst->zst_cond = DK_ZONE_COND_EMPTY;
		nreset++;
	}

	/*
	 * Cached buffers for the reset zones would serve stale contents
	 * once their LBAs are reused; drop the device's cache before the
	 * allocator can hand them out again.
	 */
	if (nreset > 0)
		zlfs_cache_purge_dev(zmp);
}

/*
 * Dirty every in-core inode that owns a block in the victim zone, so
 * the next commit relocates those blocks to fresh LBAs and the zone
 * becomes fully dead (a later zlfs_clean then resets it).  A regular
 * file's data blocks are pulled into its dirty overlay; any hit on an
 * inode's metadata (indirect or inode block) or on a directory just
 * marks the whole inode dirty, so the commit rewrites it.
 *
 * MUST be called with no vnode locks held (it blocks in VFS_VGET); the
 * only caller is the sync path.
 */
static int
zlfs_compact_zone(struct zlfs_mount *zmp, u_int64_t victim)
{
	struct zlfs_node *znp;
	struct vnode *vp;
	u_int32_t bsize = zmp->zm_super.zs_block_size;
	u_int64_t vstart = zmp->zm_zones[victim].zst_start_lba;
	u_int64_t vend = vstart + zmp->zm_zones[victim].zst_cap_lba;
	u_int64_t *inos, ninos = 0, cap, k, b, nblk, lba;
	int error = 0, prepared;

	/* Snapshot the live inode numbers under zm_lock. */
	rw_enter_read(&zmp->zm_lock);
	cap = zmp->zm_ninodes;
	inos = mallocarray(cap ? cap : 1, sizeof(*inos), M_TEMP, M_WAITOK);
	for (k = 0; k < zmp->zm_ninodes && ninos < cap; k++) {
		if (zmp->zm_imap[k] != 0)
			inos[ninos++] = k;
	}
	rw_exit_read(&zmp->zm_lock);

	for (k = 0; k < ninos; k++) {
		error = VFS_VGET(zmp->zm_mountp, inos[k], &vp);
		if (error != 0) {
			if (error == ENOENT)	/* removed since the snapshot */
				continue;
			break;
		}
		znp = VTOZ(vp);

		if (vp->v_type == VDIR) {
			/* Whole directory relocates if any block is here. */
			int hit = zlfs_node_owns_range(znp, vstart, vend);
			if (hit > 0) {
				if (zlfs_node_load(znp) == 0)
					zlfs_node_dirty(znp);
			}
			vput(vp);
			if (hit < 0) {
				error = EIO;
				break;
			}
			continue;
		}
		if (vp->v_type != VREG) {
			/* Only VREG and VDIR inodes exist today (mknod/
			 * symlink/link are unsupported); any other type
			 * would keep its victim blocks live and stall the
			 * compactor, so it must relocate them if added. */
			vput(vp);
			continue;
		}

		/*
		 * Pull each in-victim data block into the overlay so the
		 * commit rewrites it elsewhere; if any of the inode's
		 * metadata (inode or indirect blocks) is also in the
		 * victim, force the commit to relocate the indirect blocks
		 * too.  Either way the node must be dirtied to be picked up.
		 *
		 * A file beyond the write cap (only another implementation
		 * of the format can produce one) cannot go through the
		 * overlay: skip it.  Its zones stay uncompactable, which
		 * degrades reclamation but keeps the data safe.
		 */
		nblk = howmany(znp->zn_dinode.zi_size, bsize);
		if (nblk > ZLFS_MAXFILESZ(zmp) / bsize) {
			vput(vp);
			continue;
		}
		prepared = 0;
		for (b = 0; b < nblk; b++) {
			error = zlfs_bmap_read(znp, b, &lba);
			if (error != 0)
				break;
			if (lba >= vstart && lba < vend) {
				error = zlfs_dblk_prepare(znp, b, 1);
				if (error != 0)
					break;
				prepared = 1;
			}
		}
		if (error == 0 && zlfs_node_owns_meta(znp, vstart, vend)) {
			znp->zn_relocate = 1;
			prepared = 1;
		}
		if (error == 0 && prepared)
			zlfs_node_dirty(znp);
		vput(vp);
		if (error != 0)
			break;
	}

	free(inos, M_TEMP, (cap ? cap : 1) * sizeof(*inos));
	return error;
}

/*
 * Copying garbage collection: relocate the live blocks out of the
 * least-live written zone so a later zlfs_clean can reclaim it.  Called
 * from the sync path when whole-zone reclamation cannot keep up (mixed
 * live/dead zones dominate).  Returns nonzero on an I/O error, in which
 * case nothing was relocated.
 */
int
zlfs_compact(struct zlfs_mount *zmp)
{
	struct zlfs_zone_state *zst;
	u_int64_t i, victim = 0, best = 0;

	if (zmp->zm_dead)
		return EIO;	/* TEST: powered off */
	if (zlfs_gc_mark_all(zmp) != 0)
		return EIO;

	/* Pick the written, non-head zone with the fewest live bytes. */
	for (i = ZLFS_SB_ZONES; i < zmp->zm_nzones; i++) {
		if (i == zmp->zm_log_zidx)
			continue;
		zst = &zmp->zm_zones[i];
		if (zst->zst_wp_lba == zst->zst_start_lba)
			continue;		/* empty, nothing to move */
		if (zst->zst_live_bytes == 0)
			continue;		/* fully dead; clean handles it */
		if (best == 0 || zst->zst_live_bytes < best) {
			best = zst->zst_live_bytes;
			victim = i;
		}
	}
	if (best == 0)
		return 0;		/* no mixed zone to compact */

	return zlfs_compact_zone(zmp, victim);
}

/*
 * Initialise the write-log heads from the loaded zone state.  The data
 * log continues at the head of the circular log -- the one written zone
 * that still has room -- or starts at an empty data zone.  The
 * superblock log appends to the active SB zone found by discovery.
 */
int
zlfs_log_init(struct zlfs_mount *zmp, const struct dk_zone *sbz)
{
	struct zlfs_zone_state *zst;
	u_int64_t i, bpb = zmp->zm_super.zs_block_size / zmp->zm_secsize;
	int found = 0;

	zmp->zm_sb_zstart[0] = sbz[0].dz_start_lba;
	zmp->zm_sb_zstart[1] = sbz[1].dz_start_lba;
	zmp->zm_sb_zcap = zmp->zm_super.zs_zone_cap_lba;
	if (zmp->zm_sb_zidx < 0 || zmp->zm_sb_zidx >= ZLFS_SB_ZONES)
		return EINVAL;
	/*
	 * A power cut can leave a torn (partial-block) append at the SB
	 * zone's write pointer.  Appends must land at the device write
	 * pointer AND at block stride (discovery scans block-aligned
	 * entries from the zone start), so an unaligned head can never
	 * be appended to again: declare the zone full and let the next
	 * commit ping-pong to the other zone -- the switch resets it,
	 * realigning everything.  The live superblock in the torn zone
	 * stays where discovery found it.
	 */
	if ((zmp->zm_zones[zmp->zm_sb_zidx].zst_wp_lba -
	    zmp->zm_sb_zstart[zmp->zm_sb_zidx]) % bpb != 0)
		zmp->zm_sb_lba = zmp->zm_sb_zstart[zmp->zm_sb_zidx] +
		    zmp->zm_sb_zcap;
	else
		zmp->zm_sb_lba = zmp->zm_zones[zmp->zm_sb_zidx].zst_wp_lba;

	/*
	 * Find the data-log head.  The log is circular, so after a wrap
	 * the head is not the highest written zone: it is the single
	 * written-but-not-full zone with room for at least one block
	 * (unique while the zone capacity is a multiple of the block
	 * size; zones with a smaller tail count as full and their space
	 * comes back through the cleaner).
	 */
	for (i = ZLFS_SB_ZONES; i < zmp->zm_nzones; i++) {
		zst = &zmp->zm_zones[i];
		/*
		 * The in-range bound also rejects degraded zones the
		 * device reports with an invalid (all-ones) write pointer.
		 * A torn (block-unaligned) write pointer disqualifies the
		 * zone as the head too: block appends must land at the
		 * device write pointer, which an aligned head requires.
		 * The zone's earlier, aligned blocks stay readable and
		 * the cleaner reclaims it like any written zone.
		 */
		if (zst->zst_wp_lba > zst->zst_start_lba &&
		    zst->zst_wp_lba < zst->zst_start_lba + zst->zst_cap_lba &&
		    (zst->zst_wp_lba - zst->zst_start_lba) % bpb == 0 &&
		    zst->zst_start_lba + zst->zst_cap_lba -
		    zst->zst_wp_lba >= bpb) {
			zmp->zm_log_zidx = i;
			zmp->zm_log_lba = zst->zst_wp_lba;
			found = 1;
			break;
		}
	}
	if (!found) {
		/* No partial head; start at the first empty data zone. */
		for (i = ZLFS_SB_ZONES; i < zmp->zm_nzones; i++) {
			zst = &zmp->zm_zones[i];
			if (zst->zst_wp_lba == zst->zst_start_lba) {
				zmp->zm_log_zidx = i;
				zmp->zm_log_lba = zst->zst_start_lba;
				found = 1;
				break;
			}
		}
	}
	if (found) {
		zst = &zmp->zm_zones[zmp->zm_log_zidx];
		zmp->zm_log_zend = zst->zst_start_lba + zst->zst_cap_lba;
	} else {
		/*
		 * Every data zone is written.  Mount anyway with an
		 * exhausted head: the first commit runs the cleaner and
		 * the allocator wraps into whatever it frees; until then
		 * writes fail with ENOSPC.  Failing the mount here would
		 * make a merely-full filesystem unmountable.
		 */
		zst = &zmp->zm_zones[ZLFS_SB_ZONES];
		zmp->zm_log_zidx = ZLFS_SB_ZONES;
		zmp->zm_log_lba = zmp->zm_log_zend = zst->zst_start_lba;
	}

	/*
	 * A superblock-only image (no checkpoint) has no inode map; a
	 * read-write mount still needs one to create files, so allocate
	 * an empty full-block map with only the reserved inodes.
	 */
	if (zmp->zm_imap == NULL) {
		zmp->zm_imap_alloc = zmp->zm_super.zs_block_size;
		zmp->zm_imap = malloc(zmp->zm_imap_alloc, M_ZLFS,
		    M_WAITOK | M_ZERO);
		zmp->zm_ninodes = ZLFS_FIRST_INO;
	}

	return 0;
}

/*
 * Allocate the next block-sized run of LBAs from the data log.  When the
 * current zone fills, wrap around the data zones to the next empty one
 * (never-written or reclaimed by the cleaner); the log is therefore a
 * circular allocator over the data zones, not a one-shot linear sweep.
 */
int
zlfs_alloc_block(struct zlfs_mount *zmp, u_int64_t *lbap)
{
	u_int64_t bpb = zmp->zm_super.zs_block_size / zmp->zm_secsize;
	u_int64_t ndata = zmp->zm_nzones - ZLFS_SB_ZONES;
	struct zlfs_zone_state *zst = NULL;

	if (zmp->zm_log_lba + bpb > zmp->zm_log_zend) {
		u_int64_t step, cand = 0;
		int found = 0;

		for (step = 1; step <= ndata; step++) {
			cand = ZLFS_SB_ZONES +
			    ((zmp->zm_log_zidx - ZLFS_SB_ZONES + step) % ndata);
			zst = &zmp->zm_zones[cand];
			if (zst->zst_wp_lba == zst->zst_start_lba) {
				found = 1;
				break;
			}
		}
		if (!found)
			return ENOSPC;
		zmp->zm_log_zidx = cand;
		zmp->zm_log_lba = zst->zst_start_lba;
		zmp->zm_log_zend = zst->zst_start_lba + zst->zst_cap_lba;
	}

	*lbap = zmp->zm_log_lba;
	zmp->zm_log_lba += bpb;
	/* Keep the zone's write pointer and condition current so the
	 * free/empty, reclaim, and statfs accounting stay accurate. */
	zst = &zmp->zm_zones[zmp->zm_log_zidx];
	zst->zst_wp_lba = zmp->zm_log_lba;
	if (zst->zst_cond == DK_ZONE_COND_EMPTY)
		zst->zst_cond = DK_ZONE_COND_IMPLICIT_OPEN;
	return 0;
}
