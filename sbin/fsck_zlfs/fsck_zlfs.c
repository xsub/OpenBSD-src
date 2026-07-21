/*	$OpenBSD$	*/

/*
 * fsck_zlfs -- offline ZLFS consistency check (read-only).
 *
 * ZLFS is log-structured: repair is not rewriting blocks in place (the
 * zone contract forbids it) but mounting an older generation, so this
 * tool only CHECKS and reports.  It works on the raw device or on a
 * plain image file (e.g. a dd dump), needing no zone ioctls: the
 * superblock zones are scanned forward at block stride, geometry comes
 * from the superblock itself.
 *
 * Checks, in order:
 *   - superblock log: valid generations in zones 0-1, static-geometry
 *     agreement, newest generation selection;
 *   - checkpoint: magic/version/CRC/UUID, generation matches the
 *     superblock, inode-map shape;
 *   - inode map: every referenced map block readable and in range;
 *   - every inode: identity (zi_ino), mode, link count, size bounds,
 *     full block-tree walk (direct, single, double, triple) with every
 *     LBA range-checked and cross-inode duplicate detection, zi_blocks
 *     arithmetic re-derived;
 *   - namespace: recursive walk from the root -- "." / ".." identity,
 *     dirent sanity, dangling entries, type vs target mode, directory
 *     cycles / double-reachability, file link counts vs dirent
 *     references, directory nlink = 2 + subdirs;
 *   - orphans: mapped inodes unreachable from the root.
 *
 * Exit status: 0 clean, 8 inconsistencies found, 1 operational error.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/dirent.h>
#include <sys/zlfs.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int	fd = -1;
static int	verbose;
static u_int32_t secsize = 512;
static u_int32_t bsize;
static u_int64_t zone_size, zone_cap, total_zones;
static u_int64_t nerrors, nwarns;

static struct zlfs_super sb;		/* host-endian, newest generation */

/* Inode map. */
static u_int64_t *imap;
static u_int64_t ninodes;

/* Per-inode state gathered by the walks. */
static struct zlfs_inode *inodes;	/* host-endian copies, [ninodes] */
static u_int8_t	*ino_ok;		/* inode loaded and self-consistent */
static u_int32_t *ino_refs;		/* dirent references found */
static u_int8_t	*ino_visited;		/* reached from the root */
static u_int32_t *dir_subdirs;		/* subdir count per directory */

/* Seen-LBA hash set for duplicate-block detection. */
static u_int64_t *seen_lba;		/* open addressing, 0 = empty */
static u_int64_t *seen_owner;
static u_int64_t seen_cap, seen_cnt;

static u_int32_t crc32c_table[256];

static void
crc32c_init(void)
{
	u_int32_t c;
	int i, j;

	for (i = 0; i < 256; i++) {
		c = i;
		for (j = 0; j < 8; j++)
			c = (c & 1) ? (0x82f63b78 ^ (c >> 1)) : (c >> 1);
		crc32c_table[i] = c;
	}
}

static u_int32_t
crc32c(const void *buf, size_t len)
{
	const u_int8_t *p = buf;
	u_int32_t c = 0xffffffff;

	while (len--)
		c = crc32c_table[(c ^ *p++) & 0xff] ^ (c >> 8);
	return c ^ 0xffffffff;
}

static void
error_report(const char *fmt, ...)
{
	va_list ap;

	nerrors++;
	va_start(ap, fmt);
	fprintf(stderr, "ERROR: ");
	vfprintf(stderr, fmt, ap);
	fprintf(stderr, "\n");
	va_end(ap);
}

static void
warn_report(const char *fmt, ...)
{
	va_list ap;

	nwarns++;
	va_start(ap, fmt);
	fprintf(stderr, "warn: ");
	vfprintf(stderr, fmt, ap);
	fprintf(stderr, "\n");
	va_end(ap);
}

/* Read one filesystem block at device LBA lba.  Returns 0 or -1. */
static int
read_block(u_int64_t lba, void *buf)
{
	ssize_t n;

	n = pread(fd, buf, bsize, (off_t)lba * secsize);
	if (n == (ssize_t)bsize)
		return 0;
	if (n < 0)
		warn_report("pread lba %llu: %s", (unsigned long long)lba,
		    strerror(errno));
	else
		warn_report("short read at lba %llu (image truncated?)",
		    (unsigned long long)lba);
	return -1;
}

/* CRC check for a block whose checksum field sits at csum_off. */
static int
block_csum_ok(void *buf, size_t csum_off, u_int64_t want)
{
	u_int64_t save;
	u_int32_t crc;
	u_int8_t *p = buf;

	memcpy(&save, p + csum_off, sizeof(save));
	memset(p + csum_off, 0, sizeof(save));
	crc = crc32c(p, bsize);
	memcpy(p + csum_off, &save, sizeof(save));
	return (u_int64_t)crc == want;
}

static void
sb_letoh(struct zlfs_super *h, const struct zlfs_super *le)
{
	h->zs_magic = letoh32(le->zs_magic);
	h->zs_version = letoh32(le->zs_version);
	h->zs_block_size = letoh32(le->zs_block_size);
	h->zs_flags = letoh32(le->zs_flags);
	memcpy(h->zs_uuid, le->zs_uuid, sizeof(h->zs_uuid));
	h->zs_zone_size_lba = letoh64(le->zs_zone_size_lba);
	h->zs_zone_cap_lba = letoh64(le->zs_zone_cap_lba);
	h->zs_total_zones = letoh64(le->zs_total_zones);
	h->zs_generation = letoh64(le->zs_generation);
	h->zs_checkpoint_lba = letoh64(le->zs_checkpoint_lba);
	h->zs_root_ino = letoh64(le->zs_root_ino);
	h->zs_last_mount_time = letoh64(le->zs_last_mount_time);
	h->zs_checksum = letoh64(le->zs_checksum);
}

static void
inode_letoh(struct zlfs_inode *h, const struct zlfs_inode *le)
{
	int i;

	h->zi_ino = letoh64(le->zi_ino);
	h->zi_gen = letoh64(le->zi_gen);
	h->zi_size = letoh64(le->zi_size);
	h->zi_blocks = letoh64(le->zi_blocks);
	h->zi_mode = letoh32(le->zi_mode);
	h->zi_uid = letoh32(le->zi_uid);
	h->zi_gid = letoh32(le->zi_gid);
	h->zi_nlink = letoh32(le->zi_nlink);
	h->zi_flags = letoh32(le->zi_flags);
	h->zi_atime = letoh64(le->zi_atime);
	h->zi_mtime = letoh64(le->zi_mtime);
	h->zi_ctime = letoh64(le->zi_ctime);
	h->zi_btime = letoh64(le->zi_btime);
	for (i = 0; i < ZLFS_NDADDR; i++)
		h->zi_db[i] = letoh64(le->zi_db[i]);
	for (i = 0; i < 3; i++)
		h->zi_ib[i] = letoh64(le->zi_ib[i]);
}

/*
 * Try to read a superblock at lba with a candidate block size taken
 * from its own header.  On success fills *out (host-endian).
 */
static int
try_super(u_int64_t lba, struct zlfs_super *out)
{
	struct zlfs_super hdr;
	u_int8_t *buf, *sec;
	u_int32_t cand;
	ssize_t n;
	int ok = 0;

	/* Raw character devices demand sector-aligned, sector-sized
	 * reads; probe the header through a whole sector. */
	sec = malloc(secsize);
	if (sec == NULL)
		err(1, "malloc");
	n = pread(fd, sec, secsize, (off_t)lba * secsize);
	if (n != (ssize_t)secsize) {
		free(sec);
		return 0;
	}
	memcpy(&hdr, sec, sizeof(hdr));
	free(sec);
	if (letoh32(hdr.zs_magic) != ZLFS_MAGIC)
		return 0;
	cand = letoh32(hdr.zs_block_size);
	if (cand < 512 || cand > (1U << 20) || (cand & (cand - 1)) != 0 ||
	    cand < secsize || (cand % secsize) != 0)
		return 0;
	buf = malloc(cand);
	if (buf == NULL)
		err(1, "malloc");
	n = pread(fd, buf, cand, (off_t)lba * secsize);
	if (n == (ssize_t)cand) {
		u_int64_t save;
		u_int32_t crc;

		memcpy(&save, buf + offsetof(struct zlfs_super, zs_checksum),
		    sizeof(save));
		memset(buf + offsetof(struct zlfs_super, zs_checksum), 0,
		    sizeof(save));
		crc = crc32c(buf, cand);
		if ((u_int64_t)crc == letoh64(save)) {
			sb_letoh(out, (struct zlfs_super *)buf);
			ok = 1;
		}
	}
	free(buf);
	return ok;
}

/*
 * Scan both superblock zones forward at block stride, adopt the newest
 * valid generation, and report the log's shape.
 */
static void
scan_superblocks(void)
{
	struct zlfs_super cur, best;
	u_int64_t zstart[ZLFS_SB_ZONES], lba, stride, seen = 0, torn = 0;
	int z, have = 0;

	/* Bootstrap geometry from any zone-start superblock. */
	if (!try_super(0, &best)) {
		/*
		 * Zone 0 slot 0 invalid.  First a fine scan of the first
		 * 1 MB (a later entry of zone 0); failing that, zone 0 is
		 * empty (freshly recycled) and the log lives in zone 1,
		 * whose start IS the zone size -- unknown yet.  Probe
		 * power-of-two candidates and demand the entry name its
		 * own LBA as the zone size, which self-verifies the hit.
		 */
		for (lba = 1; lba < (1024 * 1024) / secsize && !have; lba++) {
			if (try_super(lba, &best))
				have = 1;
		}
		for (lba = 8; lba <= (1ULL << 36) && !have; lba <<= 1) {
			if (try_super(lba, &best) &&
			    best.zs_zone_size_lba == lba)
				have = 1;
		}
		if (!have)
			errx(1, "no valid superblock found (is this ZLFS?)");
	}
	bsize = best.zs_block_size;
	zone_size = best.zs_zone_size_lba;
	zone_cap = best.zs_zone_cap_lba;
	total_zones = best.zs_total_zones;
	stride = bsize / secsize;

	/*
	 * The checksum is not a MAC: bound the adopted geometry before a
	 * corrupt superblock can drive a division by zero or an absurd
	 * scan (mirrors the kernel's mount-time validation, which also
	 * cross-checks the device zone report -- unavailable here).
	 */
	if (zone_size == 0 || zone_cap == 0 || zone_cap > zone_size ||
	    total_zones <= ZLFS_SB_ZONES || (zone_size % stride) != 0)
		errx(1, "superblock geometry insane (zone size %llu, "
		    "cap %llu, %llu zones)",
		    (unsigned long long)zone_size,
		    (unsigned long long)zone_cap,
		    (unsigned long long)total_zones);

	zstart[0] = 0;
	zstart[1] = zone_size;
	have = 0;
	for (z = 0; z < ZLFS_SB_ZONES; z++) {
		for (lba = zstart[z]; lba + stride <= zstart[z] + zone_cap;
		    lba += stride) {
			if (!try_super(lba, &cur)) {
				/* Distinguish "never written" heuristically:
				 * count only entries after a valid one as
				 * torn candidates. */
				continue;
			}
			seen++;
			if (cur.zs_block_size != bsize ||
			    cur.zs_zone_size_lba != zone_size ||
			    cur.zs_total_zones != total_zones ||
			    memcmp(cur.zs_uuid, best.zs_uuid, 16) != 0) {
				warn_report("superblock at lba %llu has "
				    "foreign geometry/UUID (stale mkfs?)",
				    (unsigned long long)lba);
				continue;
			}
			if (!have || cur.zs_generation > sb.zs_generation) {
				sb = cur;
				have = 1;
			}
		}
	}
	(void)torn;
	if (!have)
		errx(1, "no matching superblock generation found");
	printf("superblock log: %llu valid entries, newest generation %llu, "
	    "checkpoint at lba %llu\n", (unsigned long long)seen,
	    (unsigned long long)sb.zs_generation,
	    (unsigned long long)sb.zs_checkpoint_lba);
	if (sb.zs_version != ZLFS_VERSION)
		error_report("superblock version %u, expected %u",
		    sb.zs_version, ZLFS_VERSION);
}

/*
 * An LBA a filesystem block must live at: inside a data zone.  The
 * hard bound is the zone SIZE; the uniform zs_zone_cap_lba is the
 * smallest capacity found at mkfs time, but the kernel allocator uses
 * each zone's own (possibly larger) capacity, which an image does not
 * record -- so beyond-cap-within-size is only a warning (see
 * ref_block), never an error.
 */
static int
lba_in_data_zones(u_int64_t lba)
{
	u_int64_t z, off;

	if (lba == 0)
		return 0;
	z = lba / zone_size;
	off = lba % zone_size;
	if (z < ZLFS_SB_ZONES || z >= total_zones)
		return 0;
	if (off + bsize / secsize > zone_size)
		return 0;
	return 1;
}

/* Record a referenced metadata/data block; detect cross references. */
static void
seen_add(u_int64_t lba, u_int64_t owner, const char *what)
{
	u_int64_t h;

	if (seen_cnt * 2 >= seen_cap) {
		u_int64_t ncap = seen_cap ? seen_cap * 2 : 65536, i;
		u_int64_t *nl, *no;

		nl = calloc(ncap, sizeof(u_int64_t));
		no = calloc(ncap, sizeof(u_int64_t));
		if (nl == NULL || no == NULL)
			err(1, "calloc");
		for (i = 0; i < seen_cap; i++) {
			if (seen_lba[i] == 0)
				continue;
			h = (seen_lba[i] * 0x9e3779b97f4a7c15ULL) &
			    (ncap - 1);
			while (nl[h] != 0)
				h = (h + 1) & (ncap - 1);
			nl[h] = seen_lba[i];
			no[h] = seen_owner[i];
		}
		free(seen_lba);
		free(seen_owner);
		seen_lba = nl;
		seen_owner = no;
		seen_cap = ncap;
	}
	h = (lba * 0x9e3779b97f4a7c15ULL) & (seen_cap - 1);
	while (seen_lba[h] != 0) {
		if (seen_lba[h] == lba) {
			error_report("block lba %llu (%s of inode %llu) "
			    "already referenced by inode %llu",
			    (unsigned long long)lba, what,
			    (unsigned long long)owner,
			    (unsigned long long)seen_owner[h]);
			return;
		}
		h = (h + 1) & (seen_cap - 1);
	}
	seen_lba[h] = lba;
	seen_owner[h] = owner;
	seen_cnt++;
}

/* Check + record one referenced block; returns 1 if usable. */
static int
ref_block(u_int64_t lba, u_int64_t owner, const char *what)
{
	if (!lba_in_data_zones(lba)) {
		error_report("inode %llu: %s lba %llu outside the data zones",
		    (unsigned long long)owner, what, (unsigned long long)lba);
		return 0;
	}
	if (lba % zone_size + bsize / secsize > zone_cap)
		warn_report("inode %llu: %s lba %llu beyond the uniform zone "
		    "capacity (non-uniform device?)",
		    (unsigned long long)owner, what, (unsigned long long)lba);
	if ((lba % (bsize / secsize)) != 0)
		warn_report("inode %llu: %s lba %llu not block-aligned",
		    (unsigned long long)owner, what, (unsigned long long)lba);
	seen_add(lba, owner, what);
	return 1;
}

/*
 * Walk an indirect block of entries at depth d (1 = entries are data,
 * 2 = entries are leaf blocks, 3 = entries are mid blocks), counting
 * data and metadata blocks.  nvalid caps how many DATA blocks below
 * this subtree are inside the file; entries beyond it must be zero.
 */
static void
walk_indir(u_int64_t lba, int depth, u_int64_t ino, u_int64_t nvalid,
    u_int64_t *ndata, u_int64_t *nmeta)
{
	u_int64_t *ents, sub, per, i, nindir = bsize / sizeof(u_int64_t);
	u_int8_t *buf;

	buf = malloc(bsize);
	if (buf == NULL)
		err(1, "malloc");
	if (read_block(lba, buf) != 0) {
		error_report("inode %llu: unreadable indirect block %llu",
		    (unsigned long long)ino, (unsigned long long)lba);
		free(buf);
		return;
	}
	(*nmeta)++;
	ents = (u_int64_t *)buf;

	per = 1;
	for (i = 1; (int)i < depth; i++)
		per *= nindir;		/* data blocks per entry subtree */

	for (i = 0; i < nindir; i++) {
		u_int64_t e = letoh64(ents[i]);

		if (i * per >= nvalid) {
			if (e != 0)
				error_report("inode %llu: entry %llu of "
				    "indirect %llu points past the file end",
				    (unsigned long long)ino,
				    (unsigned long long)i,
				    (unsigned long long)lba);
			continue;
		}
		sub = nvalid - i * per;
		if (sub > per)
			sub = per;
		if (e == 0) {
			error_report("inode %llu: hole at indirect %llu "
			    "entry %llu (no-holes invariant)",
			    (unsigned long long)ino, (unsigned long long)lba,
			    (unsigned long long)i);
			continue;
		}
		if (depth == 1) {
			if (ref_block(e, ino, "data"))
				(*ndata)++;
		} else {
			if (ref_block(e, ino, "indirect"))
				walk_indir(e, depth - 1, ino, sub,
				    ndata, nmeta);
		}
	}
	free(buf);
}

/* Load and self-check every mapped inode; walk its block tree. */
static void
check_inodes(void)
{
	struct zlfs_inode le, *zi;
	u_int64_t ino, nindir = bsize / sizeof(u_int64_t);
	u_int64_t nblocks, want, ndata, nmeta, dvalid, tvalid;
	u_int64_t maxbytes, e0;
	u_int8_t *buf;

	buf = malloc(bsize);
	if (buf == NULL)
		err(1, "malloc");
	/* Size bound in BYTES: comparing zi_size directly cannot
	 * overflow, unlike deriving a block count from a huge size. */
	maxbytes = (u_int64_t)bsize * (ZLFS_NDADDR + nindir +
	    nindir * nindir + nindir * nindir * nindir);

	for (ino = 0; ino < ninodes; ino++) {
		if (imap[ino] == 0)
			continue;
		if (!ref_block(imap[ino], ino, "inode block"))
			continue;
		if (read_block(imap[ino], buf) != 0) {
			error_report("inode %llu: unreadable inode block %llu",
			    (unsigned long long)ino,
			    (unsigned long long)imap[ino]);
			continue;
		}
		memcpy(&le, buf, sizeof(le));
		zi = &inodes[ino];
		inode_letoh(zi, &le);

		if (zi->zi_ino != ino) {
			error_report("inode %llu: on-disk zi_ino is %llu",
			    (unsigned long long)ino,
			    (unsigned long long)zi->zi_ino);
			continue;
		}
		if ((zi->zi_mode & S_IFMT) != S_IFREG &&
		    (zi->zi_mode & S_IFMT) != S_IFDIR) {
			error_report("inode %llu: unsupported mode 0%o",
			    (unsigned long long)ino, zi->zi_mode);
			continue;
		}
		if (zi->zi_nlink == 0) {
			error_report("inode %llu: mapped but nlink 0",
			    (unsigned long long)ino);
			continue;
		}
		if (zi->zi_size > maxbytes) {
			error_report("inode %llu: size %llu beyond the "
			    "triple-indirect bound", (unsigned long long)ino,
			    (unsigned long long)zi->zi_size);
			continue;
		}
		if ((zi->zi_mode & S_IFMT) == S_IFDIR &&
		    zi->zi_size > (u_int64_t)bsize * (ZLFS_NDADDR + nindir)) {
			/* The kernel caps directories at direct + single
			 * indirect (ZLFS_MAXDIRSZ); anything bigger was
			 * never written by it. */
			error_report("dir %llu: size %llu beyond the "
			    "directory bound", (unsigned long long)ino,
			    (unsigned long long)zi->zi_size);
			continue;
		}

		/*
		 * Everything from here on reports but keeps checking; the
		 * inode joins the namespace walk only if none of it fired
		 * (a broken inode must not be walked into).
		 */
		e0 = nerrors;

		/* Re-derive the block count from the no-holes invariant. */
		nblocks = (zi->zi_size + bsize - 1) / bsize;
		dvalid = (nblocks > ZLFS_NDADDR + nindir) ?
		    nblocks - ZLFS_NDADDR - nindir : 0;
		if (dvalid > nindir * nindir)
			dvalid = nindir * nindir;
		tvalid = (nblocks > ZLFS_NDADDR + nindir + nindir * nindir) ?
		    nblocks - ZLFS_NDADDR - nindir - nindir * nindir : 0;

		ndata = nmeta = 0;
		{
			u_int64_t b, nd = nblocks;

			if (nd > ZLFS_NDADDR)
				nd = ZLFS_NDADDR;
			for (b = 0; b < ZLFS_NDADDR; b++) {
				if (b < nd) {
					if (zi->zi_db[b] == 0)
						error_report("inode %llu: "
						    "direct block %llu is a "
						    "hole",
						    (unsigned long long)ino,
						    (unsigned long long)b);
					else if (ref_block(zi->zi_db[b], ino,
					    "data"))
						ndata++;
				} else if (zi->zi_db[b] != 0) {
					error_report("inode %llu: direct "
					    "block %llu past the file end",
					    (unsigned long long)ino,
					    (unsigned long long)b);
				}
			}
		}
		if (nblocks > ZLFS_NDADDR) {
			u_int64_t sv = nblocks - ZLFS_NDADDR;

			if (sv > nindir)
				sv = nindir;
			if (zi->zi_ib[0] == 0)
				error_report("inode %llu: missing single "
				    "indirect", (unsigned long long)ino);
			else if (ref_block(zi->zi_ib[0], ino, "indirect"))
				walk_indir(zi->zi_ib[0], 1, ino, sv,
				    &ndata, &nmeta);
		} else if (zi->zi_ib[0] != 0) {
			error_report("inode %llu: stale single indirect",
			    (unsigned long long)ino);
		}
		if (dvalid > 0) {
			if (zi->zi_ib[1] == 0)
				error_report("inode %llu: missing double "
				    "indirect", (unsigned long long)ino);
			else if (ref_block(zi->zi_ib[1], ino, "indirect"))
				walk_indir(zi->zi_ib[1], 2, ino, dvalid,
				    &ndata, &nmeta);
		} else if (zi->zi_ib[1] != 0) {
			error_report("inode %llu: stale double indirect",
			    (unsigned long long)ino);
		}
		if (tvalid > 0) {
			if (zi->zi_ib[2] == 0)
				error_report("inode %llu: missing triple "
				    "indirect", (unsigned long long)ino);
			else if (ref_block(zi->zi_ib[2], ino, "indirect"))
				walk_indir(zi->zi_ib[2], 3, ino, tvalid,
				    &ndata, &nmeta);
		} else if (zi->zi_ib[2] != 0) {
			error_report("inode %llu: stale triple indirect",
			    (unsigned long long)ino);
		}

		want = nblocks + nmeta;
		if (zi->zi_blocks != want)
			error_report("inode %llu: zi_blocks %llu, expected "
			    "%llu (%llu data + %llu metadata)",
			    (unsigned long long)ino,
			    (unsigned long long)zi->zi_blocks,
			    (unsigned long long)want,
			    (unsigned long long)nblocks,
			    (unsigned long long)nmeta);
		if (ndata != nblocks)
			error_report("inode %llu: %llu reachable data blocks, "
			    "size says %llu", (unsigned long long)ino,
			    (unsigned long long)ndata,
			    (unsigned long long)nblocks);

		ino_ok[ino] = (nerrors == e0);
		if (verbose)
			printf("inode %llu: %s size %llu nlink %u "
			    "blocks %llu at lba %llu\n",
			    (unsigned long long)ino,
			    (zi->zi_mode & S_IFMT) == S_IFDIR ? "dir" : "file",
			    (unsigned long long)zi->zi_size, zi->zi_nlink,
			    (unsigned long long)zi->zi_blocks,
			    (unsigned long long)imap[ino]);
	}
	free(buf);
}

/*
 * Read a directory's full contents.  Directories never exceed the
 * direct + single-indirect range (check_inodes gates their size at
 * the kernel's ZLFS_MAXDIRSZ before this runs), so only those two
 * legs exist here.
 */
static u_int8_t *
read_dir(u_int64_t ino, u_int64_t *lenp)
{
	struct zlfs_inode *zi = &inodes[ino];
	u_int64_t nindir = bsize / sizeof(u_int64_t);
	u_int64_t nblocks = (zi->zi_size + bsize - 1) / bsize;
	u_int64_t b, lba;
	u_int8_t *data, *buf;

	if (zi->zi_size == 0) {
		*lenp = 0;
		return calloc(1, 1);
	}
	data = malloc(nblocks * bsize);
	buf = malloc(bsize);
	if (data == NULL || buf == NULL)
		err(1, "malloc");
	for (b = 0; b < nblocks; b++) {
		lba = 0;
		if (b < ZLFS_NDADDR) {
			lba = zi->zi_db[b];
		} else if (b < ZLFS_NDADDR + nindir &&
		    zi->zi_ib[0] != 0 && read_block(zi->zi_ib[0], buf) == 0) {
			lba = letoh64(((u_int64_t *)buf)[b - ZLFS_NDADDR]);
		}
		if (lba == 0 || read_block(lba, data + b * bsize) != 0) {
			error_report("dir %llu: unreadable block %llu",
			    (unsigned long long)ino, (unsigned long long)b);
			memset(data + b * bsize, 0, bsize);
		}
	}
	free(buf);
	*lenp = zi->zi_size;
	return data;
}

/*
 * Walk the directory tree from the root with an explicit worklist --
 * recursion depth would equal namespace depth, and the format admits
 * far deeper legal hierarchies than any thread stack.  A directory is
 * marked visited when queued; meeting an already-visited directory
 * again is structural corruption (cycle or double link).
 */
struct dirwork {
	u_int64_t ino;
	u_int64_t parent;
};

static void
check_dirs(u_int64_t root)
{
	const struct zlfs_dirent *zd;
	struct dirwork *wl;
	u_int8_t *data;
	u_int64_t wcap = 64, wn = 0, ino, parent, len, off, cino;
	int dot, dotdot;

	wl = reallocarray(NULL, wcap, sizeof(*wl));
	if (wl == NULL)
		err(1, "reallocarray");
	wl[wn].ino = root;
	wl[wn].parent = root;
	wn = 1;
	ino_visited[root] = 1;

	while (wn > 0) {
		wn--;
		ino = wl[wn].ino;
		parent = wl[wn].parent;
		dot = dotdot = 0;

		data = read_dir(ino, &len);
		for (off = 0; off + ZLFS_DIRENT_SIZE <= len;
		    off += ZLFS_DIRENT_SIZE) {
			zd = (const struct zlfs_dirent *)(data + off);
			cino = letoh64(zd->zd_ino);
			if (cino == 0)
				continue;
			if (zd->zd_namlen == 0 ||
			    zd->zd_namlen > ZLFS_MAXNAMLEN) {
				error_report("dir %llu: bad name length %u "
				    "at offset %llu", (unsigned long long)ino,
				    zd->zd_namlen, (unsigned long long)off);
				continue;
			}
			if (memchr(zd->zd_name, '/', zd->zd_namlen) != NULL ||
			    memchr(zd->zd_name, '\0', zd->zd_namlen) != NULL) {
				error_report("dir %llu: illegal character in "
				    "name at offset %llu",
				    (unsigned long long)ino,
				    (unsigned long long)off);
				continue;
			}
			if (zd->zd_namlen == 1 && zd->zd_name[0] == '.') {
				dot = 1;
				if (cino != ino)
					error_report("dir %llu: \".\" points "
					    "at %llu", (unsigned long long)ino,
					    (unsigned long long)cino);
				continue;
			}
			if (zd->zd_namlen == 2 && zd->zd_name[0] == '.' &&
			    zd->zd_name[1] == '.') {
				dotdot = 1;
				if (cino != parent)
					error_report("dir %llu: \"..\" points "
					    "at %llu, parent is %llu",
					    (unsigned long long)ino,
					    (unsigned long long)cino,
					    (unsigned long long)parent);
				continue;
			}
			if (cino >= ninodes || imap[cino] == 0 ||
			    !ino_ok[cino]) {
				error_report("dir %llu: entry \"%.*s\" "
				    "references %s inode %llu",
				    (unsigned long long)ino,
				    zd->zd_namlen, zd->zd_name,
				    (cino < ninodes && imap[cino] != 0) ?
				    "a broken" : "a nonexistent",
				    (unsigned long long)cino);
				continue;
			}
			if ((inodes[cino].zi_mode & S_IFMT) == S_IFDIR) {
				if (zd->zd_type != DT_DIR)
					error_report("dir %llu: \"%.*s\" "
					    "typed %u, target is a directory",
					    (unsigned long long)ino,
					    zd->zd_namlen, zd->zd_name,
					    zd->zd_type);
				dir_subdirs[ino]++;
				if (ino_visited[cino]) {
					error_report("dir %llu reached twice "
					    "(cycle or double link)",
					    (unsigned long long)cino);
					continue;
				}
				ino_visited[cino] = 1;
				if (wn == wcap) {
					wcap *= 2;
					wl = reallocarray(wl, wcap,
					    sizeof(*wl));
					if (wl == NULL)
						err(1, "reallocarray");
				}
				wl[wn].ino = cino;
				wl[wn].parent = ino;
				wn++;
			} else {
				if (zd->zd_type != DT_REG)
					error_report("dir %llu: \"%.*s\" "
					    "typed %u, target is a file",
					    (unsigned long long)ino,
					    zd->zd_namlen, zd->zd_name,
					    zd->zd_type);
				ino_refs[cino]++;
				ino_visited[cino] = 1;
			}
		}
		if (!dot)
			error_report("dir %llu: missing \".\"",
			    (unsigned long long)ino);
		if (!dotdot)
			error_report("dir %llu: missing \"..\"",
			    (unsigned long long)ino);
		free(data);
	}
	free(wl);
}

static void
check_namespace(void)
{
	u_int64_t ino;

	if (sb.zs_root_ino >= ninodes || imap[sb.zs_root_ino] == 0 ||
	    !ino_ok[sb.zs_root_ino]) {
		error_report("root inode %llu missing or broken",
		    (unsigned long long)sb.zs_root_ino);
		return;
	}
	if ((inodes[sb.zs_root_ino].zi_mode & S_IFMT) != S_IFDIR) {
		error_report("root inode is not a directory");
		return;
	}
	check_dirs(sb.zs_root_ino);

	for (ino = 0; ino < ninodes; ino++) {
		if (imap[ino] == 0 || !ino_ok[ino])
			continue;
		if (!ino_visited[ino]) {
			error_report("inode %llu mapped but unreachable "
			    "from the root (orphan)",
			    (unsigned long long)ino);
			continue;
		}
		if ((inodes[ino].zi_mode & S_IFMT) == S_IFDIR) {
			u_int32_t want = 2 + dir_subdirs[ino];

			if (inodes[ino].zi_nlink != want)
				error_report("dir %llu: nlink %u, expected "
				    "%u (2 + %u subdirs)",
				    (unsigned long long)ino,
				    inodes[ino].zi_nlink, want,
				    dir_subdirs[ino]);
		} else if (inodes[ino].zi_nlink != ino_refs[ino]) {
			error_report("file %llu: nlink %u, %u dirent "
			    "references", (unsigned long long)ino,
			    inodes[ino].zi_nlink, ino_refs[ino]);
		}
	}
}

static void
load_checkpoint(void)
{
	struct zlfs_checkpoint ck;
	u_int64_t *lbas, nblocks, epb, i, j, n;
	u_int8_t *buf;

	buf = malloc(bsize);
	if (buf == NULL)
		err(1, "malloc");
	if (sb.zs_checkpoint_lba == 0) {
		/* A superblock-only image (older newfs) is mountable --
		 * the kernel synthesises an empty root -- so it is clean,
		 * not an error. */
		printf("no checkpoint yet (superblock-only image); "
		    "nothing to check\nclean\n");
		exit(0);
	}
	if (!lba_in_data_zones(sb.zs_checkpoint_lba))
		errx(1, "checkpoint lba %llu outside the data zones",
		    (unsigned long long)sb.zs_checkpoint_lba);
	if (read_block(sb.zs_checkpoint_lba, buf) != 0)
		errx(1, "checkpoint unreadable");
	memcpy(&ck, buf, sizeof(ck));
	if (letoh32(ck.zc_magic) != ZLFS_MAGIC ||
	    letoh32(ck.zc_version) != ZLFS_VERSION)
		errx(1, "checkpoint magic/version wrong");
	if (!block_csum_ok(buf, offsetof(struct zlfs_checkpoint, zc_checksum),
	    letoh64(ck.zc_checksum)))
		errx(1, "checkpoint checksum wrong");
	if (memcmp(ck.zc_uuid, sb.zs_uuid, sizeof(ck.zc_uuid)) != 0)
		errx(1, "checkpoint UUID does not match the superblock");
	if (letoh64(ck.zc_generation) != sb.zs_generation)
		error_report("checkpoint generation %llu, superblock %llu",
		    (unsigned long long)letoh64(ck.zc_generation),
		    (unsigned long long)sb.zs_generation);
	if (letoh64(ck.zc_root_ino) != sb.zs_root_ino)
		error_report("checkpoint root inode %llu, superblock %llu",
		    (unsigned long long)letoh64(ck.zc_root_ino),
		    (unsigned long long)sb.zs_root_ino);

	epb = bsize / sizeof(u_int64_t);
	nblocks = letoh64(ck.zc_imap_nblocks);
	ninodes = letoh64(ck.zc_ninodes);
	if (nblocks == 0 || nblocks > ZLFS_CKPT_NIMAP(bsize) ||
	    ninodes <= ZLFS_ROOT_INO || ninodes > nblocks * epb)
		errx(1, "checkpoint inode-map shape invalid "
		    "(%llu blocks, %llu inodes)",
		    (unsigned long long)nblocks,
		    (unsigned long long)ninodes);

	lbas = calloc(nblocks, sizeof(u_int64_t));
	imap = calloc(ninodes, sizeof(u_int64_t));
	if (lbas == NULL || imap == NULL)
		err(1, "calloc");
	for (j = 0; j < nblocks; j++)
		lbas[j] = letoh64(((u_int64_t *)(buf +
		    sizeof(struct zlfs_checkpoint)))[j]);

	seen_add(sb.zs_checkpoint_lba, 0, "checkpoint");
	for (j = 0; j < (ninodes + epb - 1) / epb; j++) {
		if (lbas[j] == 0) {
			error_report("inode-map block %llu has no LBA",
			    (unsigned long long)j);
			continue;
		}
		if (!ref_block(lbas[j], 0, "inode map"))
			continue;
		if (read_block(lbas[j], buf) != 0) {
			error_report("inode-map block %llu unreadable",
			    (unsigned long long)j);
			continue;
		}
		n = ninodes - j * epb;
		if (n > epb)
			n = epb;
		for (i = 0; i < n; i++)
			imap[j * epb + i] = letoh64(((u_int64_t *)buf)[i]);
	}
	free(lbas);
	free(buf);
	printf("checkpoint: generation %llu, %llu inode slots\n",
	    (unsigned long long)sb.zs_generation,
	    (unsigned long long)ninodes);
}

static void
usage(void)
{
	fprintf(stderr, "usage: fsck_zlfs [-v] [-S secsize] special|image\n");
	exit(1);
}

int
main(int argc, char *argv[])
{
	const char *errstr;
	u_int64_t ino, nfiles = 0, ndirs = 0;
	int ch;

	while ((ch = getopt(argc, argv, "vS:")) != -1) {
		switch (ch) {
		case 'v':
			verbose = 1;
			break;
		case 'S':
			secsize = strtonum(optarg, 512, 65536, &errstr);
			if (errstr != NULL)
				errx(1, "secsize is %s: %s", errstr, optarg);
			break;
		default:
			usage();
		}
	}
	argc -= optind;
	argv += optind;
	if (argc != 1)
		usage();

	fd = open(argv[0], O_RDONLY);
	if (fd == -1)
		err(1, "open %s", argv[0]);

	crc32c_init();
	scan_superblocks();
	load_checkpoint();

	inodes = calloc(ninodes, sizeof(*inodes));
	ino_ok = calloc(ninodes, 1);
	ino_refs = calloc(ninodes, sizeof(*ino_refs));
	ino_visited = calloc(ninodes, 1);
	dir_subdirs = calloc(ninodes, sizeof(*dir_subdirs));
	if (inodes == NULL || ino_ok == NULL || ino_refs == NULL ||
	    ino_visited == NULL || dir_subdirs == NULL)
		err(1, "calloc");

	check_inodes();
	check_namespace();

	for (ino = 0; ino < ninodes; ino++) {
		if (imap[ino] == 0 || !ino_ok[ino])
			continue;
		if ((inodes[ino].zi_mode & S_IFMT) == S_IFDIR)
			ndirs++;
		else
			nfiles++;
	}
	printf("%llu files, %llu directories, %llu referenced blocks\n",
	    (unsigned long long)nfiles, (unsigned long long)ndirs,
	    (unsigned long long)seen_cnt);
	if (nerrors == 0) {
		printf("clean%s\n", nwarns ?
		    " (with warnings)" : "");
		return 0;
	}
	printf("%llu error(s), %llu warning(s)\n",
	    (unsigned long long)nerrors, (unsigned long long)nwarns);
	return 8;
}
