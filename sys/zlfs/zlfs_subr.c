/*	$OpenBSD$	*/

/*
 * ZLFS support routines: CRC32C and superblock-log discovery.
 *
 * The discovery algorithm follows the on-disk format documentation in
 * <sys/zlfs.h>: the superblock is a generation-numbered log ping-ponged
 * across zones 0 and 1.  The block size is bootstrapped from a
 * zone-start entry, then each superblock zone is scanned backward from
 * its write pointer (bounded), and the newest entry with a valid
 * checksum wins.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/buf.h>
#include <sys/endian.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/proc.h>
#include <sys/vnode.h>
#include <sys/dkio.h>
#include <sys/zlfs.h>

#include <zlfs/zlfs_var.h>

/* How many blocks below the write pointer discovery is willing to scan. */
#define ZLFS_SB_SCAN_MAX	64

static u_int32_t zlfs_crc32c_table[256];

static u_int32_t	zlfs_sb_probe(const void *, u_int32_t);
static int		zlfs_sb_block_valid(const void *, u_int32_t);
static int		zlfs_sb_try(struct zlfs_mount *, u_int64_t, u_int32_t,
			    struct zlfs_super *, u_int64_t *, int *);

void
zlfs_crc32c_init(void)
{
	u_int32_t c;
	int i, j;

	if (zlfs_crc32c_table[1] != 0)
		return;

	for (i = 0; i < 256; i++) {
		c = i;
		for (j = 0; j < 8; j++)
			c = (c & 1) ? (c >> 1) ^ 0x82f63b78 : c >> 1;
		zlfs_crc32c_table[i] = c;
	}
}

u_int32_t
zlfs_crc32c_update(u_int32_t c, const void *buf, size_t len)
{
	const u_int8_t *p = buf;

	while (len-- > 0)
		c = zlfs_crc32c_table[(c ^ *p++) & 0xff] ^ (c >> 8);

	return c;
}

void
zlfs_sb_letoh(struct zlfs_super *h, const struct zlfs_super *le)
{
	memset(h, 0, sizeof(*h));
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

/*
 * Bootstrap check on a buffer that may hold only one device sector:
 * returns the filesystem block size if the buffer starts with a
 * plausible superblock, 0 otherwise.  The checksum cannot be checked
 * yet because it covers the whole block.
 */
static u_int32_t
zlfs_sb_probe(const void *buf, u_int32_t secsize)
{
	const struct zlfs_super *le = buf;
	u_int32_t bsize;

	if (secsize < sizeof(struct zlfs_super))
		return 0;
	if (letoh32(le->zs_magic) != ZLFS_MAGIC)
		return 0;
	if (letoh32(le->zs_version) != ZLFS_VERSION)
		return 0;
	bsize = letoh32(le->zs_block_size);
	if (bsize < 512 || bsize > MAXBSIZE ||
	    (bsize & (bsize - 1)) != 0 ||
	    bsize < secsize || bsize % secsize != 0)
		return 0;

	return bsize;
}

/*
 * Full validation of one block-sized superblock-log entry (still
 * little-endian): magic, version, block size and CRC32C over the whole
 * block with zs_checksum taken as zero.
 */
static int
zlfs_sb_block_valid(const void *block, u_int32_t bsize)
{
	static const u_int64_t zero64 = 0;
	const struct zlfs_super *le = block;
	u_int32_t c;

	if (letoh32(le->zs_magic) != ZLFS_MAGIC)
		return 0;
	if (letoh32(le->zs_version) != ZLFS_VERSION)
		return 0;
	if (letoh32(le->zs_block_size) != bsize)
		return 0;

	c = ZLFS_CRC32C_INITIAL;
	c = zlfs_crc32c_update(c, block,
	    offsetof(struct zlfs_super, zs_checksum));
	c = zlfs_crc32c_update(c, &zero64, sizeof(zero64));
	c = zlfs_crc32c_update(c, (const u_int8_t *)block +
	    sizeof(struct zlfs_super), bsize - sizeof(struct zlfs_super));

	return (u_int64_t)ZLFS_CRC32C_FINAL(c) == letoh64(le->zs_checksum);
}

/*
 * Read one candidate block and fold it into the running best.  Read
 * errors just skip the candidate: unwritten LBAs are not readable on
 * all zoned devices.  All discovery buffers are marked B_INVAL so no
 * stale copies linger in the buffer cache.
 */
static int
zlfs_sb_try(struct zlfs_mount *zmp, u_int64_t lba, u_int32_t bsize,
    struct zlfs_super *best, u_int64_t *best_gen, int *best_valid)
{
	struct buf *bp;
	u_int64_t gen;

	/*
	 * bread() hands back the buffer even on error, and reads of
	 * never-written LBAs are an expected miss on zoned devices, so
	 * release the buffer on every path.
	 */
	if (bread(zmp->zm_devvp, btodb(ZLFS_LBATOB(zmp, lba)), bsize,
	    &bp) != 0) {
		brelse(bp);
		return 0;
	}

	if (zlfs_sb_block_valid(bp->b_data, bsize)) {
		gen = letoh64(((struct zlfs_super *)bp->b_data)->
		    zs_generation);
		if (!*best_valid || gen > *best_gen) {
			memcpy(best, bp->b_data, sizeof(*best));
			*best_gen = gen;
			*best_valid = 1;
		}
	}

	bp->b_flags |= B_INVAL;
	brelse(bp);
	return 0;
}

/*
 * Find the newest valid superblock in the superblock zones described
 * by sbz[0..ZLFS_SB_ZONES-1] and leave a host-endian copy in
 * zmp->zm_super.
 */
int
zlfs_sb_discover(struct zlfs_mount *zmp, const struct dk_zone *sbz,
    struct proc *p)
{
	struct buf *bp;
	struct zlfs_super best;
	u_int64_t best_gen = 0, bs_lbas, cand, k, nscan, start, wp;
	u_int32_t bsize = 0, secsize = zmp->zm_secsize;
	int best_valid = 0, error, z;

	/* Bootstrap the block size from a zone-start entry. */
	for (z = 0; z < ZLFS_SB_ZONES && bsize == 0; z++) {
		error = bread(zmp->zm_devvp,
		    btodb(ZLFS_LBATOB(zmp, sbz[z].dz_start_lba)), secsize,
		    &bp);
		if (error != 0) {
			brelse(bp);
			continue;
		}
		bsize = zlfs_sb_probe(bp->b_data, secsize);
		bp->b_flags |= B_INVAL;
		brelse(bp);
	}
	if (bsize == 0)
		return EINVAL;

	bs_lbas = bsize / secsize;

	for (z = 0; z < ZLFS_SB_ZONES; z++) {
		start = sbz[z].dz_start_lba;
		wp = sbz[z].dz_write_pointer_lba;

		error = zlfs_sb_try(zmp, start, bsize, &best, &best_gen,
		    &best_valid);
		if (error != 0)
			return error;

		if (wp != DK_ZONE_WP_INVALID && wp > start) {
			/*
			 * Write-pointer zone: the newest entry sits just
			 * below the write pointer; torn tails fail the
			 * checksum and the scan steps further back.
			 */
			nscan = (wp - start) / bs_lbas;
			if (nscan > ZLFS_SB_SCAN_MAX)
				nscan = ZLFS_SB_SCAN_MAX;
			for (k = 1; k <= nscan; k++) {
				cand = wp - k * bs_lbas;
				if (cand <= start)
					break;
				error = zlfs_sb_try(zmp, cand, bsize, &best,
				    &best_gen, &best_valid);
				if (error != 0)
					return error;
			}
		} else if (wp == DK_ZONE_WP_INVALID) {
			/*
			 * Conventional zone (no write pointer): bounded
			 * forward scan for the highest generation.
			 */
			for (k = 1; k < ZLFS_SB_SCAN_MAX; k++) {
				cand = start + k * bs_lbas;
				if (cand >= start + sbz[z].dz_capacity_lba)
					break;
				error = zlfs_sb_try(zmp, cand, bsize, &best,
				    &best_gen, &best_valid);
				if (error != 0)
					return error;
			}
		}
	}

	if (!best_valid)
		return EINVAL;

	zlfs_sb_letoh(&zmp->zm_super, &best);
	zmp->zm_bshift = ffs(bsize) - 1;
	return 0;
}

/*
 * Read one filesystem block (zs_block_size bytes) at device LBA lba.
 * Returns the buffer in *bpp on success; on error the buffer is
 * released and *bpp is NULL.
 */
int
zlfs_bread_block(struct zlfs_mount *zmp, u_int64_t lba, struct buf **bpp)
{
	struct buf *bp;
	int error;

	error = bread(zmp->zm_devvp, btodb(ZLFS_LBATOB(zmp, lba)),
	    zmp->zm_super.zs_block_size, &bp);
	if (error != 0) {
		brelse(bp);
		*bpp = NULL;
		return error;
	}
	*bpp = bp;
	return 0;
}

/* CRC32C over a block with the 8-byte checksum field at off taken as zero. */
static int
zlfs_block_csum_ok(const void *block, u_int32_t bsize, size_t off,
    u_int64_t stored)
{
	static const u_int64_t zero64 = 0;
	u_int32_t c;

	c = ZLFS_CRC32C_INITIAL;
	c = zlfs_crc32c_update(c, block, off);
	c = zlfs_crc32c_update(c, &zero64, sizeof(zero64));
	c = zlfs_crc32c_update(c, (const u_int8_t *)block + off + sizeof(zero64),
	    bsize - off - sizeof(zero64));

	return (u_int64_t)ZLFS_CRC32C_FINAL(c) == stored;
}

/*
 * Load and validate the checkpoint named by zs_checkpoint_lba, then
 * read its inode map into zmp->zm_imap.  The caller has already
 * verified the superblock, so cross-check the checkpoint against it.
 */
int
zlfs_ckpt_load(struct zlfs_mount *zmp)
{
	struct buf *bp;
	const struct zlfs_checkpoint *dc;
	u_int64_t gen, root_ino, imap_lba, ninodes, i, maxino;
	u_int32_t bsize = zmp->zm_super.zs_block_size;
	int error;

	maxino = bsize / sizeof(u_int64_t);

	error = zlfs_bread_block(zmp, zmp->zm_super.zs_checkpoint_lba, &bp);
	if (error != 0)
		return error;

	dc = (const struct zlfs_checkpoint *)bp->b_data;
	if (letoh32(dc->zc_magic) != ZLFS_MAGIC ||
	    letoh32(dc->zc_version) != ZLFS_VERSION ||
	    !zlfs_block_csum_ok(bp->b_data, bsize,
	    offsetof(struct zlfs_checkpoint, zc_checksum),
	    letoh64(dc->zc_checksum)) ||
	    memcmp(dc->zc_uuid, zmp->zm_super.zs_uuid,
	    sizeof(dc->zc_uuid)) != 0) {
		brelse(bp);
		return EINVAL;
	}

	gen = letoh64(dc->zc_generation);
	root_ino = letoh64(dc->zc_root_ino);
	imap_lba = letoh64(dc->zc_imap_lba);
	ninodes = letoh64(dc->zc_ninodes);
	brelse(bp);

	if (gen != zmp->zm_super.zs_generation ||
	    root_ino != zmp->zm_super.zs_root_ino ||
	    ninodes <= ZLFS_ROOT_INO || ninodes > maxino) {
		return EINVAL;
	}

	error = zlfs_bread_block(zmp, imap_lba, &bp);
	if (error != 0)
		return error;

	zmp->zm_imap = mallocarray(ninodes, sizeof(u_int64_t), M_ZLFS,
	    M_WAITOK);
	for (i = 0; i < ninodes; i++)
		zmp->zm_imap[i] =
		    letoh64(((u_int64_t *)bp->b_data)[i]);
	zmp->zm_ninodes = ninodes;
	brelse(bp);

	return 0;
}

/*
 * Read the on-disk inode for ino into *zi (host-endian).  Returns
 * ENOENT for an out-of-range or absent inode.
 */
int
zlfs_read_dinode(struct zlfs_mount *zmp, u_int64_t ino, struct zlfs_inode *zi)
{
	struct buf *bp;
	const struct zlfs_inode *dip;
	u_int64_t lba;
	int error, i;

	if (ino >= zmp->zm_ninodes)
		return ENOENT;
	lba = zmp->zm_imap[ino];
	if (lba == 0)
		return ENOENT;

	error = zlfs_bread_block(zmp, lba, &bp);
	if (error != 0)
		return error;

	dip = (const struct zlfs_inode *)bp->b_data;
	memset(zi, 0, sizeof(*zi));
	zi->zi_ino = letoh64(dip->zi_ino);
	zi->zi_gen = letoh64(dip->zi_gen);
	zi->zi_size = letoh64(dip->zi_size);
	zi->zi_blocks = letoh64(dip->zi_blocks);
	zi->zi_mode = letoh32(dip->zi_mode);
	zi->zi_uid = letoh32(dip->zi_uid);
	zi->zi_gid = letoh32(dip->zi_gid);
	zi->zi_nlink = letoh32(dip->zi_nlink);
	zi->zi_atime = letoh64(dip->zi_atime);
	zi->zi_mtime = letoh64(dip->zi_mtime);
	zi->zi_ctime = letoh64(dip->zi_ctime);
	zi->zi_btime = letoh64(dip->zi_btime);
	zi->zi_atimensec = letoh32(dip->zi_atimensec);
	zi->zi_mtimensec = letoh32(dip->zi_mtimensec);
	zi->zi_ctimensec = letoh32(dip->zi_ctimensec);
	zi->zi_btimensec = letoh32(dip->zi_btimensec);
	for (i = 0; i < ZLFS_NDADDR; i++)
		zi->zi_db[i] = letoh64(dip->zi_db[i]);
	for (i = 0; i < 3; i++)
		zi->zi_ib[i] = letoh64(dip->zi_ib[i]);
	brelse(bp);

	if (zi->zi_ino != ino)
		return EIO;
	return 0;
}
