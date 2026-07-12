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
 * Number of empty data zones (superblock zones excluded), for statfs.
 */
u_int64_t
zlfs_zones_empty(struct zlfs_mount *zmp)
{
	u_int64_t i, n = 0;

	for (i = ZLFS_SB_ZONES; i < zmp->zm_nzones; i++) {
		if (zmp->zm_zones[i].zst_cond == DK_ZONE_COND_EMPTY)
			n++;
	}

	return n;
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
	struct buf *bp;
	u_int64_t j, nindir = ZLFS_NINDIR(zmp);
	int error;

	/*
	 * This implementation never writes double/triple indirect blocks;
	 * if some other writer of this format did, the blocks they reach
	 * would not be marked, so refuse to reclaim anything.
	 */
	if (zi->zi_ib[1] != 0 || zi->zi_ib[2] != 0)
		return EFTYPE;

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
 * Reclaim fully dead data zones.
 *
 * A zone may be reset only if neither of two live sets reaches it:
 *
 *  1. The DURABLE set: everything reachable from the on-disk checkpoint
 *     through the inode-map block it references.  The in-core map is
 *     not a substitute -- unlink/rmdir/rename clear its entries before
 *     the next commit, and a failed commit leaves it pointing at new,
 *     not-yet-durable inode blocks -- so the map is re-read from disk.
 *     This is what a crash recovery can reach; erasing any of it would
 *     lose committed data.
 *
 *  2. The IN-CORE set: everything reachable through zm_imap right now,
 *     including inode blocks written by an earlier failed commit that
 *     the durable checkpoint does not know about.  The mounted
 *     filesystem still reads through these.
 *
 * Any read failure leaves a live set incomplete, so the scan bails and
 * reclaims nothing rather than risk resetting live data.  Zones with a
 * mix of live and dead blocks are left alone (a copying cleaner is
 * future work); the log head zone is never reset.
 */
void
zlfs_clean(struct zlfs_mount *zmp)
{
	struct zlfs_zone_state *zst;
	struct zlfs_node *znp;
	struct buf *bp;
	u_int64_t *dimap;
	u_int32_t bsize = zmp->zm_super.zs_block_size;
	u_int64_t i, ino, lba, maxino = bsize / sizeof(u_int64_t);
	int error;

	for (i = 0; i < zmp->zm_nzones; i++)
		zmp->zm_zones[i].zst_live_bytes = 0;

	/* Metadata roots of the durable checkpoint. */
	zlfs_gc_mark(zmp, zmp->zm_super.zs_checkpoint_lba);
	zlfs_gc_mark(zmp, zmp->zm_imap_lba);

	/* Pass 1: the durable live set, from the on-disk inode map. */
	if (zmp->zm_imap_lba != 0) {
		if (zlfs_bread_block(zmp, zmp->zm_imap_lba, &bp) != 0)
			return;
		dimap = malloc(bsize, M_TEMP, M_WAITOK);
		memcpy(dimap, bp->b_data, bsize);
		brelse(bp);
		for (ino = 0; ino < maxino; ino++) {
			lba = letoh64(dimap[ino]);
			if (lba == 0)
				continue;
			if (zlfs_gc_mark_inode(zmp, lba) != 0) {
				free(dimap, M_TEMP, bsize);
				return;
			}
		}
		free(dimap, M_TEMP, bsize);
	}

	/* Pass 2: the in-core live set, through zm_imap. */
	for (ino = 0; ino < zmp->zm_ninodes; ino++) {
		if (zmp->zm_imap[ino] == 0)
			continue;
		if (zlfs_gc_mark_inode(zmp, zmp->zm_imap[ino]) != 0)
			return;
	}

	/*
	 * Pass 3: blocks still reachable through in-core vnodes.  An
	 * unlinked-but-open file is in neither map once its removal has
	 * been committed, yet the open vnode still reads its old blocks
	 * from disk, so mark everything each in-core inode points at.
	 */
	LIST_FOREACH(znp, &zmp->zm_nodes, zn_entry) {
		if (zlfs_gc_mark_blocks(zmp, &znp->zn_dinode) != 0)
			return;
	}

	/* Reset any written-but-dead data zone (never the log head). */
	for (i = ZLFS_SB_ZONES; i < zmp->zm_nzones; i++) {
		if (i == zmp->zm_log_zidx)
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
	}
}

/*
 * Initialise the write-log heads from the loaded zone state.  The data
 * log appends to the highest already-written data zone (continuing from
 * its write pointer) or the first data zone if none has been written.
 * The superblock log appends to the active SB zone found by discovery.
 */
int
zlfs_log_init(struct zlfs_mount *zmp, const struct dk_zone *sbz)
{
	struct zlfs_zone_state *zst;
	u_int64_t i;
	int found = 0;

	zmp->zm_sb_zstart[0] = sbz[0].dz_start_lba;
	zmp->zm_sb_zstart[1] = sbz[1].dz_start_lba;
	zmp->zm_sb_zcap = zmp->zm_super.zs_zone_cap_lba;
	if (zmp->zm_sb_zidx < 0 || zmp->zm_sb_zidx >= ZLFS_SB_ZONES)
		return EINVAL;
	zmp->zm_sb_lba = zmp->zm_zones[zmp->zm_sb_zidx].zst_wp_lba;

	for (i = zmp->zm_nzones; i > ZLFS_SB_ZONES; i--) {
		zst = &zmp->zm_zones[i - 1];
		if (zst->zst_wp_lba <= zst->zst_start_lba)
			continue;	/* zone never written */

		if (zst->zst_wp_lba < zst->zst_start_lba + zst->zst_cap_lba) {
			zmp->zm_log_zidx = i - 1;
			zmp->zm_log_lba = zst->zst_wp_lba;
		} else if (i < zmp->zm_nzones) {
			zst = &zmp->zm_zones[i];
			zmp->zm_log_zidx = i;
			zmp->zm_log_lba = zst->zst_start_lba;
		} else {
			return ENOSPC;
		}
		zmp->zm_log_zend = zmp->zm_zones[zmp->zm_log_zidx].zst_start_lba +
		    zmp->zm_zones[zmp->zm_log_zidx].zst_cap_lba;
		found = 1;
		break;
	}
	if (!found) {
		zst = &zmp->zm_zones[ZLFS_SB_ZONES];
		zmp->zm_log_zidx = ZLFS_SB_ZONES;
		zmp->zm_log_lba = zst->zst_start_lba;
		zmp->zm_log_zend = zst->zst_start_lba + zst->zst_cap_lba;
	}

	/*
	 * A superblock-only image (no checkpoint) has no inode map; a
	 * read-write mount still needs one to create files, so allocate
	 * an empty full-block map with only the reserved inodes.
	 */
	if (zmp->zm_imap == NULL) {
		zmp->zm_imap = malloc(zmp->zm_super.zs_block_size, M_ZLFS,
		    M_WAITOK | M_ZERO);
		zmp->zm_ninodes = ZLFS_FIRST_INO;
	}

	/* New inodes are appended at the end of the inode map. */
	zmp->zm_next_ino = zmp->zm_ninodes;
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
