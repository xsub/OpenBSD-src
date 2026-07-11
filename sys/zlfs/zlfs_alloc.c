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
 * Allocate the next block-sized run of LBAs from the data log,
 * advancing to the next empty data zone when the current one fills.
 */
int
zlfs_alloc_block(struct zlfs_mount *zmp, u_int64_t *lbap)
{
	u_int64_t bpb = zmp->zm_super.zs_block_size / zmp->zm_secsize;
	struct zlfs_zone_state *zst;

	if (zmp->zm_log_lba + bpb > zmp->zm_log_zend) {
		if (zmp->zm_log_zidx + 1 >= zmp->zm_nzones)
			return ENOSPC;
		zmp->zm_log_zidx++;
		zst = &zmp->zm_zones[zmp->zm_log_zidx];
		if (zst->zst_wp_lba != zst->zst_start_lba)
			return ENOSPC;	/* next zone not empty */
		zmp->zm_log_lba = zst->zst_start_lba;
		zmp->zm_log_zend = zst->zst_start_lba + zst->zst_cap_lba;
	}

	*lbap = zmp->zm_log_lba;
	zmp->zm_log_lba += bpb;
	return 0;
}
