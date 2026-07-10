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
