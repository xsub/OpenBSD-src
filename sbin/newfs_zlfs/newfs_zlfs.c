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
#include <sys/stat.h>
#include <sys/zlfs.h>

#include <dirent.h>
#include <err.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <util.h>

#define ZLFS_DFL_BLOCK_SIZE	4096
#define ZLFS_MAX_BLOCK_SIZE	65536
#define ZLFS_REPORT_ENTRIES	256
#define ZLFS_MAX_REPORT_PAGES	(1024 * 1024)

/* Data-zone layout laid down by mkfs (block indices within the zone). */
#define ZLFS_MKFS_SUMMARY	0
#define ZLFS_MKFS_DIR		1
#define ZLFS_MKFS_FILE		2
#define ZLFS_MKFS_ROOTINO	3
#define ZLFS_MKFS_FILEINO	4
#define ZLFS_MKFS_IMAP		5
#define ZLFS_MKFS_CKPT		6
#define ZLFS_MKFS_NBLOCKS	7

static const char zlfs_welcome[] =
    "Welcome to ZLFS!\n"
    "This file was written by newfs_zlfs(8) on a zoned block device.\n";

struct zone_geometry {
	u_int64_t	total_zones;
	u_int64_t	zone_size_lba;	/* uniform zone length */
	u_int64_t	min_cap_lba;	/* minimum capacity over all zones */
	u_int64_t	device_lbas;	/* end LBA of the last zone */
	u_int64_t	sb_zone_start[ZLFS_SB_ZONES];
};

static u_int32_t	crc32c_table[256];

static void		crc32c_init(void);
static u_int32_t	crc32c_add(u_int32_t, const void *, size_t);
static u_int32_t	crc32c(const void *, size_t);
static u_int32_t	device_secsize(int);
static void		scan_zones(int, const char *, struct zone_geometry *,
			    u_int64_t);
static void		zone_reset(int, const char *, u_int64_t);
static void		zone_arm(int, const char *, u_int64_t);
static void		build_super(struct zlfs_super *, u_int32_t,
			    const struct zone_geometry *, const u_int8_t *,
			    u_int64_t);
static void		write_block(int, const char *, u_int64_t, u_int32_t,
			    const void *, u_int32_t);
static void		write_filesystem(int, const char *,
			    const struct zone_geometry *, const u_int8_t *,
			    u_int32_t, u_int32_t, int);
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
crc32c_add(u_int32_t c, const void *buf, size_t len)
{
	const u_int8_t *p = buf;

	while (len-- > 0)
		c = crc32c_table[(c ^ *p++) & 0xff] ^ (c >> 8);

	return c;
}

static u_int32_t
crc32c(const void *buf, size_t len)
{
	return crc32c_add(0xffffffff, buf, len) ^ 0xffffffff;
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
 * Enumerate the device's zones and derive the geometry ZLFS requires:
 * contiguous zones of one uniform size, all sequential (write-pointer)
 * zones, with a uniform minimum capacity.  A nonzero zone clamp stops
 * the scan there, so the filesystem occupies only the first N zones --
 * the kernel allocator and GC never step past zs_total_zones, and on a
 * capacity-class drive (26 TB SMR) a clamp is what makes the
 * space-pressure regress suites meaningful at all.
 */
static void
scan_zones(int fd, const char *path, struct zone_geometry *g,
    u_int64_t zclamp)
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

			if (zclamp != 0 && g->total_zones >= zclamp) {
				done = 1;
				break;
			}
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
    const struct zone_geometry *g, const u_int8_t *uuid,
    u_int64_t checkpoint_lba)
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
	zs->zs_checkpoint_lba = htole64(checkpoint_lba);
	zs->zs_root_ino = htole64(ZLFS_ROOT_INO);
	zs->zs_last_mount_time = htole64(0);	/* never mounted */
}

static void
write_block(int fd, const char *path, u_int64_t lba, u_int32_t secsize,
    const void *buf, u_int32_t block_size)
{
	off_t off = (off_t)lba * secsize;
	ssize_t n;

	n = pwrite(fd, buf, block_size, off);
	if (n == -1)
		err(1, "pwrite %s", path);
	if ((size_t)n != block_size)
		errx(1, "%s: short write at lba %llu (%zd of %u bytes)",
		    path, (unsigned long long)lba, n, block_size);
}

static void
build_inode(struct zlfs_inode *zi, u_int64_t ino, u_int32_t mode,
    u_int32_t nlink, u_int64_t size, u_int64_t data_lba, u_int64_t now)
{
	memset(zi, 0, sizeof(*zi));
	zi->zi_ino = htole64(ino);
	zi->zi_gen = htole64(1);
	zi->zi_size = htole64(size);
	zi->zi_blocks = htole64(size == 0 ? 0 : 1);
	zi->zi_mode = htole32(mode);
	zi->zi_nlink = htole32(nlink);
	zi->zi_atime = zi->zi_mtime = zi->zi_ctime = zi->zi_btime =
	    htole64(now);
	if (data_lba != 0)
		zi->zi_db[0] = htole64(data_lba);
}

static void
put_dirent(void *slot, u_int64_t ino, u_int8_t type, const char *name)
{
	struct zlfs_dirent zd;
	size_t namlen = strlen(name);

	memset(&zd, 0, sizeof(zd));
	zd.zd_ino = htole64(ino);
	zd.zd_type = type;
	zd.zd_namlen = (u_int8_t)namlen;
	memcpy(zd.zd_name, name, namlen);
	memcpy(slot, &zd, sizeof(zd));
}

/*
 * Lay down a minimal but real filesystem in the first data zone: a
 * zone summary, a root directory containing one regular file, the file
 * data, the two inodes, an inode map, and a checkpoint.  Then write
 * the generation-0 superblock pointing at the checkpoint.
 */
static void
write_filesystem(int fd, const char *path, const struct zone_geometry *g,
    const u_int8_t *uuid, u_int32_t block_size, u_int32_t secsize, int quiet)
{
	struct zlfs_super zs;
	struct zlfs_zone_summary zsum;
	struct zlfs_checkpoint zc;
	struct zlfs_inode zi;
	u_int8_t *blk;
	u_int64_t bpb = block_size / secsize;
	u_int64_t dzs = (u_int64_t)ZLFS_SB_ZONES * g->zone_size_lba;
	u_int64_t lba[ZLFS_MKFS_NBLOCKS];
	u_int64_t now = (u_int64_t)time(NULL);
	u_int64_t imap_ent;
	u_int32_t crc, i;
	size_t filelen = sizeof(zlfs_welcome) - 1;

	if (g->min_cap_lba < (u_int64_t)ZLFS_MKFS_NBLOCKS * bpb)
		errx(1, "%s: data zone too small for the filesystem layout",
		    path);
	if (filelen > block_size)
		errx(1, "%s: sample file larger than one block", path);

	for (i = 0; i < ZLFS_MKFS_NBLOCKS; i++)
		lba[i] = dzs + (u_int64_t)i * bpb;

	blk = calloc(1, block_size);
	if (blk == NULL)
		err(1, "calloc");

	/* Reset the superblock zones and the first data zone. */
	zone_reset(fd, path, g->sb_zone_start[1]);
	zone_reset(fd, path, g->sb_zone_start[0]);
	zone_reset(fd, path, dzs);

	/* Fill the data zone sequentially from its write pointer. */
	zone_arm(fd, path, dzs);

	/* Block 0: zone summary. */
	memset(blk, 0, block_size);
	memset(&zsum, 0, sizeof(zsum));
	zsum.zz_magic = htole32(ZLFS_MAGIC);
	zsum.zz_seq_num = htole64(0);
	zsum.zz_zone_start_lba = htole64(dzs);
	zsum.zz_num_blocks = htole32((u_int32_t)(g->min_cap_lba / bpb));
	crc = crc32c_add(0xffffffff, uuid, 16);
	crc = crc32c_add(crc, &zsum, sizeof(zsum)) ^ 0xffffffff;
	zsum.zz_checksum = htole64(crc);
	memcpy(blk, &zsum, sizeof(zsum));
	write_block(fd, path, lba[ZLFS_MKFS_SUMMARY], secsize, blk, block_size);

	/* Block 1: root directory. */
	memset(blk, 0, block_size);
	put_dirent(blk + 0 * ZLFS_DIRENT_SIZE, ZLFS_ROOT_INO, DT_DIR, ".");
	put_dirent(blk + 1 * ZLFS_DIRENT_SIZE, ZLFS_ROOT_INO, DT_DIR, "..");
	put_dirent(blk + 2 * ZLFS_DIRENT_SIZE, ZLFS_FIRST_INO, DT_REG,
	    "welcome");
	write_block(fd, path, lba[ZLFS_MKFS_DIR], secsize, blk, block_size);

	/* Block 2: file data. */
	memset(blk, 0, block_size);
	memcpy(blk, zlfs_welcome, filelen);
	write_block(fd, path, lba[ZLFS_MKFS_FILE], secsize, blk, block_size);

	/* Block 3: root inode. */
	memset(blk, 0, block_size);
	build_inode(&zi, ZLFS_ROOT_INO, S_IFDIR | 0755, 2,
	    3 * ZLFS_DIRENT_SIZE, lba[ZLFS_MKFS_DIR], now);
	memcpy(blk, &zi, sizeof(zi));
	write_block(fd, path, lba[ZLFS_MKFS_ROOTINO], secsize, blk, block_size);

	/* Block 4: file inode. */
	memset(blk, 0, block_size);
	build_inode(&zi, ZLFS_FIRST_INO, S_IFREG | 0644, 1, filelen,
	    lba[ZLFS_MKFS_FILE], now);
	memcpy(blk, &zi, sizeof(zi));
	write_block(fd, path, lba[ZLFS_MKFS_FILEINO], secsize, blk, block_size);

	/* Block 5: inode map (imap[ino] = inode block LBA). */
	memset(blk, 0, block_size);
	imap_ent = htole64(lba[ZLFS_MKFS_ROOTINO]);
	memcpy(blk + ZLFS_ROOT_INO * sizeof(u_int64_t), &imap_ent,
	    sizeof(imap_ent));
	imap_ent = htole64(lba[ZLFS_MKFS_FILEINO]);
	memcpy(blk + ZLFS_FIRST_INO * sizeof(u_int64_t), &imap_ent,
	    sizeof(imap_ent));
	write_block(fd, path, lba[ZLFS_MKFS_IMAP], secsize, blk, block_size);

	/* Block 6: checkpoint (one-block inode map; LBA follows header). */
	memset(blk, 0, block_size);
	memset(&zc, 0, sizeof(zc));
	zc.zc_magic = htole32(ZLFS_MAGIC);
	zc.zc_version = htole32(ZLFS_VERSION);
	zc.zc_generation = htole64(0);
	zc.zc_root_ino = htole64(ZLFS_ROOT_INO);
	zc.zc_imap_nblocks = htole64(1);
	zc.zc_ninodes = htole64(ZLFS_FIRST_INO + 1);
	memcpy(zc.zc_uuid, uuid, sizeof(zc.zc_uuid));
	memcpy(blk, &zc, sizeof(zc));
	imap_ent = htole64(lba[ZLFS_MKFS_IMAP]);
	memcpy(blk + sizeof(struct zlfs_checkpoint), &imap_ent,
	    sizeof(imap_ent));
	crc = crc32c(blk, block_size);
	((struct zlfs_checkpoint *)blk)->zc_checksum = htole64(crc);
	write_block(fd, path, lba[ZLFS_MKFS_CKPT], secsize, blk, block_size);

	/* Finally the superblock, pointing at the checkpoint. */
	zone_reset(fd, path, g->sb_zone_start[0]);
	zone_arm(fd, path, g->sb_zone_start[0]);
	memset(blk, 0, block_size);
	build_super(&zs, block_size, g, uuid, lba[ZLFS_MKFS_CKPT]);
	memcpy(blk, &zs, sizeof(zs));
	crc = crc32c(blk, block_size);
	((struct zlfs_super *)blk)->zs_checksum = htole64(crc);
	write_block(fd, path, g->sb_zone_start[0], secsize, blk, block_size);

	free(blk);

	if (!quiet)
		printf("superblock generation 0 written to zone 0, "
		    "checkpoint at lba %llu, root + 1 file\n",
		    (unsigned long long)lba[ZLFS_MKFS_CKPT]);
}

static void __dead
usage(void)
{
	fprintf(stderr,
	    "usage: %s [-Nq] [-b block-size] [-z zones] special\n",
	    getprogname());
	exit(1);
}

int
main(int argc, char *argv[])
{
	struct dk_zone_info info;
	struct zone_geometry g;
	const char *errstr, *path;
	char *realname = NULL;
	u_int8_t uuid[16];
	u_int64_t cap_bytes, zclamp = 0;
	u_int32_t block_size = ZLFS_DFL_BLOCK_SIZE, secsize;
	int ch, fd, i, dryrun = 0, quiet = 0;

	while ((ch = getopt(argc, argv, "Nb:qz:")) != -1) {
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
		case 'z':
			/*
			 * Occupy only the first N zones.  On capacity-class
			 * drives this keeps the space-pressure tests
			 * meaningful; the kernel needs no matching option
			 * because nothing steps past zs_total_zones.
			 */
			zclamp = strtonum(optarg, ZLFS_SB_ZONES + 1,
			    1LL << 32, &errstr);
			if (errstr != NULL)
				errx(1, "zone count is %s: %s", errstr,
				    optarg);
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

	scan_zones(fd, path, &g, zclamp);

	/*
	 * A clamp larger than the device is silently unmet -- the scan
	 * just runs out of zones first.  Warn loudly so a test that sized
	 * its workload from the requested clamp is not misled into
	 * believing the rest of the device was left untouched.
	 */
	if (zclamp != 0 && g.total_zones < zclamp)
		warnx("%s: requested %llu zones but the device has only %llu; "
		    "the whole device was formatted", path,
		    (unsigned long long)zclamp,
		    (unsigned long long)g.total_zones);

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

	/*
	 * Zone 1 is left empty so superblock discovery is unambiguous:
	 * generation 0 lives in zone 0 only.  The data zone is written
	 * first so the superblock can point at the checkpoint.
	 */
	write_filesystem(fd, path, &g, uuid, block_size, secsize, quiet);

	close(fd);
	return 0;
}
