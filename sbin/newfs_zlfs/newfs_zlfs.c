/*	$OpenBSD$	*/

/*
 * Copyright (c) 2026 Pawel Suchanecki <subdcc@gmail.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * newfs_zlfs -- create a ZLFS (Zoned Log-Structured File System).
 *
 * Lays down the generation-0 superblock at the start of zone 0 and
 * leaves zone 1 empty, per the superblock ping-pong log described in
 * <sys/zlfs.h>.  Uses the dkzone ioctls for discovery and zone
 * management and the raw sequential write path for the superblock
 * write itself, so the target device must expose write-pointer zones
 * (host-managed ZBC/ZNS).
 */

#include <sys/types.h>
#include <sys/disklabel.h>
#include <sys/dkio.h>
#include <sys/endian.h>
#include <sys/ioctl.h>
#include <sys/zlfs.h>

#include <err.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <util.h>

#define ZLFS_DFL_BLOCK_SIZE	4096
#define ZLFS_MAX_BLOCK_SIZE	65536
#define ZLFS_REPORT_ENTRIES	256
#define ZLFS_MAX_REPORT_PAGES	(1024 * 1024)

struct zone_geometry {
	u_int64_t	total_zones;
	u_int64_t	zone_size_lba;	/* uniform zone length */
	u_int64_t	min_cap_lba;	/* minimum capacity over all zones */
	u_int64_t	device_lbas;	/* end LBA of the last zone */
	u_int64_t	sb_zone_start[ZLFS_SB_ZONES];
};

static u_int32_t	crc32c_table[256];

static void		crc32c_init(void);
static u_int32_t	crc32c(const void *, size_t);
static u_int32_t	device_secsize(int);
static void		scan_zones(int, const char *, struct zone_geometry *);
static void		zone_reset(int, const char *, u_int64_t);
static void		zone_arm(int, const char *, u_int64_t);
static void		build_super(struct zlfs_super *, u_int32_t,
			    const struct zone_geometry *, const u_int8_t *);
static void		write_super(int, const char *, const void *, size_t);
static void		verify_super(int, const char *, const void *, size_t);
static void __dead	usage(void);

static void
crc32c_init(void)
{
	u_int32_t c;
	int i, j;

	for (i = 0; i < 256; i++) {
		c = i;
		for (j = 0; j < 8; j++)
			c = (c & 1) ? (c >> 1) ^ 0x82f63b78 : c >> 1;
		crc32c_table[i] = c;
	}
}

static u_int32_t
crc32c(const void *buf, size_t len)
{
	const u_int8_t *p = buf;
	u_int32_t c = 0xffffffff;

	while (len-- > 0)
		c = crc32c_table[(c ^ *p++) & 0xff] ^ (c >> 8);

	return c ^ 0xffffffff;
}

static u_int32_t
device_secsize(int fd)
{
	struct disklabel dl;

	if (ioctl(fd, DIOCGDINFO, &dl) == -1)
		err(1, "DIOCGDINFO");
	if (dl.d_secsize == 0)
		errx(1, "disklabel reports zero sector size");
	/*
	 * Device offsets are computed from the raw partition, which
	 * must map 1:1 onto device LBAs for the write gate to accept
	 * the superblock write at LBA 0.
	 */
	if (DL_GETPOFFSET(&dl.d_partitions[RAW_PART]) != 0)
		errx(1, "raw partition does not start at sector 0");

	return dl.d_secsize;
}

/*
 * Enumerate every zone on the device and derive the geometry ZLFS
 * requires: contiguous zones of one uniform size, all sequential
 * (write-pointer) zones, with a uniform minimum capacity.
 */
static void
scan_zones(int fd, const char *path, struct zone_geometry *g)
{
	struct dk_zone_report report;
	struct dk_zone *zones;
	u_int64_t next_lba = 0, pages = 0;
	u_int32_t i;
	int done = 0;

	memset(g, 0, sizeof(*g));

	zones = calloc(ZLFS_REPORT_ENTRIES, sizeof(*zones));
	if (zones == NULL)
		err(1, "calloc");

	while (!done) {
		if (++pages > ZLFS_MAX_REPORT_PAGES)
			errx(1, "%s: zone report did not terminate", path);

		memset(zones, 0, ZLFS_REPORT_ENTRIES * sizeof(*zones));
		memset(&report, 0, sizeof(report));
		report.dzr_version = DK_ZONE_VERSION;
		report.dzr_report_option = DK_ZONE_REP_ALL;
		report.dzr_start_lba = next_lba;
		report.dzr_entries = ZLFS_REPORT_ENTRIES;
		report.dzr_zones = zones;

		if (ioctl(fd, DIOCGZONEREPORT, &report) == -1)
			err(1, "DIOCGZONEREPORT %s", path);
		if (report.dzr_entries_filled > ZLFS_REPORT_ENTRIES)
			errx(1, "%s: driver returned too many descriptors",
			    path);
		if (report.dzr_entries_filled == 0)
			break;

		for (i = 0; i < report.dzr_entries_filled; i++) {
			const struct dk_zone *z = &zones[i];

			if (z->dz_length_lba == 0)
				errx(1, "%s: zone at lba %llu has zero "
				    "length", path,
				    (unsigned long long)z->dz_start_lba);
			if (z->dz_start_lba != g->device_lbas)
				errx(1, "%s: zones not contiguous at lba "
				    "%llu", path,
				    (unsigned long long)z->dz_start_lba);
			if (g->zone_size_lba == 0)
				g->zone_size_lba = z->dz_length_lba;
			else if (z->dz_length_lba != g->zone_size_lba)
				errx(1, "%s: zone size not uniform at lba "
				    "%llu", path,
				    (unsigned long long)z->dz_start_lba);

			if (z->dz_type != DK_ZONE_TYPE_SEQ_REQUIRED &&
			    z->dz_type != DK_ZONE_TYPE_SEQ_PREFERRED)
				errx(1, "%s: zone at lba %llu is not a "
				    "sequential write zone (conventional "
				    "zones are not supported yet)", path,
				    (unsigned long long)z->dz_start_lba);

			if (z->dz_capacity_lba == 0 ||
			    z->dz_capacity_lba > z->dz_length_lba)
				errx(1, "%s: bogus zone capacity %llu at "
				    "lba %llu", path,
				    (unsigned long long)z->dz_capacity_lba,
				    (unsigned long long)z->dz_start_lba);
			if (g->min_cap_lba == 0 ||
			    z->dz_capacity_lba < g->min_cap_lba)
				g->min_cap_lba = z->dz_capacity_lba;

			if (g->total_zones < ZLFS_SB_ZONES)
				g->sb_zone_start[g->total_zones] =
				    z->dz_start_lba;

			if (z->dz_start_lba >
			    UINT64_MAX - z->dz_length_lba)
				errx(1, "%s: zone range overflow", path);
			g->device_lbas = z->dz_start_lba + z->dz_length_lba;
			g->total_zones++;
		}

		if (report.dzr_max_lba != 0 &&
		    g->device_lbas > report.dzr_max_lba)
			done = 1;
		if (g->device_lbas <= next_lba)
			errx(1, "%s: zone report made no progress", path);
		next_lba = g->device_lbas;
	}

	free(zones);

	if (g->total_zones < ZLFS_SB_ZONES + 1)
		errx(1, "%s: need at least %d zones, found %llu", path,
		    ZLFS_SB_ZONES + 1, (unsigned long long)g->total_zones);
}

static void
zone_reset(int fd, const char *path, u_int64_t start_lba)
{
	struct dk_zone_op op;

	memset(&op, 0, sizeof(op));
	op.dzo_version = DK_ZONE_VERSION;
	op.dzo_op = DK_ZONE_OP_RESET;
	op.dzo_lba = start_lba;

	if (ioctl(fd, DIOCZONECMD, &op) == -1)
		err(1, "DIOCZONECMD reset lba %llu %s",
		    (unsigned long long)start_lba, path);
}

/*
 * Arm the kernel's zoned write gate: a zone report starting at the
 * target zone caches that zone's descriptor, which is what admits the
 * following sequential write at its write pointer.
 */
static void
zone_arm(int fd, const char *path, u_int64_t start_lba)
{
	struct dk_zone_report report;
	struct dk_zone zone;

	memset(&zone, 0, sizeof(zone));
	memset(&report, 0, sizeof(report));
	report.dzr_version = DK_ZONE_VERSION;
	report.dzr_report_option = DK_ZONE_REP_ALL;
	report.dzr_start_lba = start_lba;
	report.dzr_entries = 1;
	report.dzr_zones = &zone;

	if (ioctl(fd, DIOCGZONEREPORT, &report) == -1)
		err(1, "DIOCGZONEREPORT %s", path);
	if (report.dzr_entries_filled != 1)
		errx(1, "%s: no zone descriptor at lba %llu", path,
		    (unsigned long long)start_lba);
	if (zone.dz_start_lba != start_lba)
		errx(1, "%s: expected zone at lba %llu, got %llu", path,
		    (unsigned long long)start_lba,
		    (unsigned long long)zone.dz_start_lba);
	if (zone.dz_condition != DK_ZONE_COND_EMPTY ||
	    zone.dz_write_pointer_lba != start_lba)
		errx(1, "%s: zone at lba %llu not empty after reset", path,
		    (unsigned long long)start_lba);
}

static void
build_super(struct zlfs_super *zs, u_int32_t block_size,
    const struct zone_geometry *g, const u_int8_t *uuid)
{
	memset(zs, 0, sizeof(*zs));
	zs->zs_magic = htole32(ZLFS_MAGIC);
	zs->zs_version = htole32(ZLFS_VERSION);
	zs->zs_block_size = htole32(block_size);
	zs->zs_flags = htole32(0);
	memcpy(zs->zs_uuid, uuid, sizeof(zs->zs_uuid));
	zs->zs_zone_size_lba = htole64(g->zone_size_lba);
	zs->zs_zone_cap_lba = htole64(g->min_cap_lba);
	zs->zs_total_zones = htole64(g->total_zones);
	zs->zs_generation = htole64(0);
	zs->zs_checkpoint_lba = htole64(0);	/* no checkpoint yet */
	zs->zs_root_ino = htole64(ZLFS_ROOT_INO);
	zs->zs_last_mount_time = htole64(0);	/* never mounted */
}

static void
write_super(int fd, const char *path, const void *buf, size_t block_size)
{
	ssize_t n;

	n = pwrite(fd, buf, block_size, 0);
	if (n == -1)
		err(1, "pwrite %s", path);
	if ((size_t)n != block_size)
		errx(1, "%s: short superblock write (%zd of %zu bytes)",
		    path, n, block_size);
}

static void
verify_super(int fd, const char *path, const void *expect, size_t block_size)
{
	void *buf;
	ssize_t n;

	buf = malloc(block_size);
	if (buf == NULL)
		err(1, "malloc");

	n = pread(fd, buf, block_size, 0);
	if (n == -1)
		err(1, "pread %s", path);
	if ((size_t)n != block_size)
		errx(1, "%s: short superblock read (%zd of %zu bytes)",
		    path, n, block_size);
	if (memcmp(buf, expect, block_size) != 0)
		errx(1, "%s: superblock verify failed", path);

	free(buf);
}

static void __dead
usage(void)
{
	fprintf(stderr,
	    "usage: %s [-Nq] [-b block-size] special\n", getprogname());
	exit(1);
}

int
main(int argc, char *argv[])
{
	struct dk_zone_info info;
	struct zone_geometry g;
	struct zlfs_super zs;
	const char *errstr, *path;
	char *realname = NULL;
	u_int8_t *block, uuid[16];
	u_int64_t cap_bytes;
	u_int32_t block_size = ZLFS_DFL_BLOCK_SIZE, crc, secsize;
	int ch, fd, i, dryrun = 0, quiet = 0;

	while ((ch = getopt(argc, argv, "Nb:q")) != -1) {
		switch (ch) {
		case 'N':
			dryrun = 1;
			break;
		case 'b':
			block_size = strtonum(optarg, 512,
			    ZLFS_MAX_BLOCK_SIZE, &errstr);
			if (errstr != NULL)
				errx(1, "block size is %s: %s", errstr,
				    optarg);
			break;
		case 'q':
			quiet = 1;
			break;
		default:
			usage();
		}
	}
	argc -= optind;
	argv += optind;
	if (argc != 1)
		usage();

	if ((block_size & (block_size - 1)) != 0)
		errx(1, "block size %u is not a power of two", block_size);

	crc32c_init();

	fd = opendev(argv[0], dryrun ? O_RDONLY : O_RDWR, OPENDEV_PART,
	    &realname);
	if (fd == -1)
		err(1, "open %s", realname != NULL ? realname : argv[0]);
	path = realname;

	memset(&info, 0, sizeof(info));
	if (ioctl(fd, DIOCGZONEINFO, &info) == -1)
		err(1, "DIOCGZONEINFO %s", path);
	if (info.dzi_zone_mode != DK_ZONE_MODE_HOST_MANAGED)
		errx(1, "%s: not a host-managed zoned device (zone mode %u)",
		    path, info.dzi_zone_mode);

	secsize = device_secsize(fd);
	if (block_size < secsize || block_size % secsize != 0)
		errx(1, "block size %u incompatible with %u-byte sectors",
		    block_size, secsize);

	scan_zones(fd, path, &g);

	if (g.min_cap_lba > UINT64_MAX / secsize)
		errx(1, "%s: zone capacity overflow", path);
	cap_bytes = g.min_cap_lba * secsize;
	if (cap_bytes < block_size)
		errx(1, "%s: zone capacity %llu bytes too small for %u-byte "
		    "blocks", path, (unsigned long long)cap_bytes,
		    block_size);
	if (g.sb_zone_start[0] != 0)
		errx(1, "%s: first zone does not start at lba 0", path);

	arc4random_buf(uuid, sizeof(uuid));
	uuid[6] = (uuid[6] & 0x0f) | 0x40;	/* UUID version 4 */
	uuid[8] = (uuid[8] & 0x3f) | 0x80;	/* RFC 4122 variant */

	if (!quiet) {
		printf("%s: %llu zones of %llu LBAs (%u-byte sectors), "
		    "capacity %llu LBAs/zone\n", path,
		    (unsigned long long)g.total_zones,
		    (unsigned long long)g.zone_size_lba, secsize,
		    (unsigned long long)g.min_cap_lba);
		printf("block size %u, superblock zones at lba 0 and %llu, "
		    "%llu data zones\n", block_size,
		    (unsigned long long)g.sb_zone_start[1],
		    (unsigned long long)(g.total_zones - ZLFS_SB_ZONES));
		printf("uuid ");
		for (i = 0; i < 16; i++)
			printf("%02x%s", uuid[i],
			    (i == 3 || i == 5 || i == 7 || i == 9) ?
			    "-" : "");
		printf("\n");
	}

	if (dryrun) {
		close(fd);
		return 0;
	}

	block = calloc(1, block_size);
	if (block == NULL)
		err(1, "calloc");

	build_super(&zs, block_size, &g, uuid);
	memcpy(block, &zs, sizeof(zs));
	crc = crc32c(block, block_size);
	((struct zlfs_super *)block)->zs_checksum = htole64(crc);

	/*
	 * Zone 1 is reset (left empty) so superblock discovery is
	 * unambiguous: generation 0 lives in zone 0 only.  The write
	 * gate requires a fresh zone report after each reset before
	 * it admits the sequential write at the write pointer.
	 */
	zone_reset(fd, path, g.sb_zone_start[1]);
	zone_reset(fd, path, g.sb_zone_start[0]);
	zone_arm(fd, path, g.sb_zone_start[0]);
	write_super(fd, path, block, block_size);
	verify_super(fd, path, block, block_size);

	if (!quiet)
		printf("superblock generation 0 written to zone 0, "
		    "zone 1 reserved\n");

	free(block);
	close(fd);
	return 0;
}
