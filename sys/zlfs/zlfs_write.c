/*	$OpenBSD$	*/

/*
 * ZLFS write path: log-structured commit.
 *
 * create/write buffer their changes in core; a commit (fsync or sync)
 * flushes them as a fresh log segment -- new data blocks, new inode
 * blocks, a new inode map, and a new checkpoint -- then appends a
 * generation N+1 superblock, which is the atomic commit point.  Every
 * block is written to a fresh LBA at the log write pointer via
 * dk_zone_write_kern(); live data is never overwritten, so a crash
 * before the superblock append simply leaves the previous checkpoint
 * in force and orphans the new blocks.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/buf.h>
#include <sys/endian.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/pool.h>
#include <sys/vnode.h>
#include <sys/dkio.h>
#include <sys/zlfs.h>

#include <zlfs/zlfs_var.h>

/*
 * Run the zone cleaner at the start of a commit when the number of free
 * data zones drops below this, so a fresh segment always has somewhere
 * to go if any dead zones can be reclaimed.
 */
#define ZLFS_GC_MIN_FREE	2

/*
 * Write one filesystem block (block_size bytes) from data to device
 * LBA lba, staging through a DMA-capable buffer.
 */
int
zlfs_write_block(struct zlfs_mount *zmp, u_int64_t lba, const void *data)
{
	u_int32_t bsize = zmp->zm_super.zs_block_size;
	void *buf;
	int error;

	buf = dma_alloc(bsize, PR_WAITOK);
	if (buf == NULL)
		return ENOMEM;
	memcpy(buf, data, bsize);
	error = dk_zone_write_kern(zmp->zm_dev, lba, buf, bsize);
	dma_free(buf, bsize);
	return error;
}

/*
 * Map a logical block number to its device LBA, following the single
 * indirect block for blocks at or beyond ZLFS_NDADDR.  *ind caches the
 * indirect block across calls (NULL on the first) so a sequential load
 * reads it once; the caller frees it with brelse when done.
 */
int
zlfs_bmap_read(struct zlfs_node *znp, u_int64_t blkno, struct buf **ind,
    u_int64_t *lbap)
{
	struct zlfs_mount *zmp = znp->zn_zmp;
	struct zlfs_inode *zi = &znp->zn_dinode;
	u_int64_t nindir = ZLFS_NINDIR(zmp);
	int error;

	if (blkno < ZLFS_NDADDR) {
		*lbap = zi->zi_db[blkno];
		return 0;
	}
	blkno -= ZLFS_NDADDR;
	if (blkno >= nindir)
		return EFBIG;		/* beyond single indirect */
	if (*ind == NULL) {
		if (zi->zi_ib[0] == 0)
			return EIO;
		error = zlfs_bread_block(zmp, zi->zi_ib[0], ind);
		if (error != 0)
			return error;
	}
	*lbap = letoh64(((u_int64_t *)(*ind)->b_data)[blkno]);
	/*
	 * Corrupt-metadata guard: an entry naming the indirect block
	 * itself would make the caller bread a buffer it already holds
	 * and sleep forever in getblk.
	 */
	if (*lbap == zi->zi_ib[0])
		return EIO;
	return 0;
}

/*
 * Grow znp->zn_data so it can hold at least need bytes, preserving the
 * current contents and zeroing the new tail.  The allocation is rounded
 * up to a block and never exceeds the maximum file size.
 */
int
zlfs_node_resize(struct zlfs_node *znp, size_t need)
{
	struct zlfs_mount *zmp = znp->zn_zmp;
	u_int32_t bsize = zmp->zm_super.zs_block_size;
	size_t newalloc;
	u_int8_t *nbuf;

	if (need <= znp->zn_dataalloc)
		return 0;
	if (need > ZLFS_MAXFILESZ(zmp))
		return EFBIG;
	newalloc = roundup(need, bsize);
	nbuf = malloc(newalloc, M_ZLFS, M_WAITOK | M_ZERO);
	if (znp->zn_data != NULL) {
		memcpy(nbuf, znp->zn_data, znp->zn_datalen);
		free(znp->zn_data, M_ZLFS, znp->zn_dataalloc);
	}
	znp->zn_data = nbuf;
	znp->zn_dataalloc = newalloc;
	return 0;
}

/*
 * Ensure znp->zn_data holds the inode's current contents so it can be
 * modified in core before the next commit.  The buffer is sized to the
 * file's current length (rounded to a block), grown on demand later.
 */
int
zlfs_node_load(struct zlfs_node *znp)
{
	struct zlfs_mount *zmp = znp->zn_zmp;
	struct zlfs_inode *zi = &znp->zn_dinode;
	u_int32_t bsize = zmp->zm_super.zs_block_size;
	struct buf *bp, *ind = NULL;
	u_int64_t off, blkno, lba;
	size_t n;
	int error;

	if (znp->zn_data != NULL)
		return 0;
	if (zi->zi_size > ZLFS_MAXFILESZ(zmp))
		return EFBIG;

	znp->zn_dataalloc = roundup(MAX(zi->zi_size, bsize), bsize);
	znp->zn_data = malloc(znp->zn_dataalloc, M_ZLFS, M_WAITOK | M_ZERO);
	znp->zn_datalen = zi->zi_size;

	for (off = 0; off < zi->zi_size; off += bsize) {
		blkno = off / bsize;
		error = zlfs_bmap_read(znp, blkno, &ind, &lba);
		if (error != 0)
			goto fail;
		if (lba == 0) {
			error = EIO;
			goto fail;
		}
		error = zlfs_bread_block(zmp, lba, &bp);
		if (error != 0)
			goto fail;
		n = bsize;
		if (off + n > zi->zi_size)
			n = zi->zi_size - off;
		memcpy(znp->zn_data + off, bp->b_data, n);
		brelse(bp);
	}
	if (ind != NULL)
		brelse(ind);
	return 0;

fail:
	if (ind != NULL)
		brelse(ind);
	free(znp->zn_data, M_ZLFS, znp->zn_dataalloc);
	znp->zn_data = NULL;
	znp->zn_dataalloc = 0;
	return error;
}

void
zlfs_node_dirty(struct zlfs_node *znp)
{
	znp->zn_dirty = 1;
}

/*
 * Write a modified inode out as fresh log blocks: its data blocks (for
 * a loaded file/directory) followed by the inode block, recording the
 * inode block's LBA in the in-core inode map.
 */
static int
zlfs_commit_node(struct zlfs_mount *zmp, struct zlfs_node *znp)
{
	struct zlfs_inode *zi = &znp->zn_dinode;
	u_int32_t bsize = zmp->zm_super.zs_block_size;
	u_int64_t maxino = ZLFS_MAXINO(zmp);
	u_int8_t *blk;
	u_int64_t off, blkno, lba;
	size_t n;
	int error;

	if (znp->zn_ino >= maxino)
		return EFBIG;

	/*
	 * A removed inode (nlink 0) is dropped from the map and not
	 * written; its blocks become garbage for a later cleaner.
	 */
	if (znp->zn_dinode.zi_nlink == 0) {
		zmp->zm_imap[znp->zn_ino] = 0;
		return 0;
	}

	if (znp->zn_data != NULL) {
		u_int64_t nindir = ZLFS_NINDIR(zmp);
		u_int8_t *iblk = NULL;
		int used_indir = 0;

		if (znp->zn_datalen > ZLFS_MAXFILESZ(zmp))
			return EFBIG;
		blk = malloc(bsize, M_ZLFS, M_WAITOK);
		zi->zi_size = znp->zn_datalen;
		zi->zi_blocks = 0;
		for (blkno = 0; blkno < ZLFS_NDADDR; blkno++)
			zi->zi_db[blkno] = 0;
		zi->zi_ib[0] = 0;
		for (off = 0; off < znp->zn_datalen; off += bsize) {
			blkno = off / bsize;
			error = zlfs_alloc_block(zmp, &lba);
			if (error != 0)
				goto data_fail;
			memset(blk, 0, bsize);
			n = bsize;
			if (off + n > znp->zn_datalen)
				n = znp->zn_datalen - off;
			memcpy(blk, znp->zn_data + off, n);
			error = zlfs_write_block(zmp, lba, blk);
			if (error != 0)
				goto data_fail;
			if (blkno < ZLFS_NDADDR) {
				zi->zi_db[blkno] = lba;
			} else {
				if (blkno - ZLFS_NDADDR >= nindir) {
					error = EFBIG;
					goto data_fail;
				}
				if (iblk == NULL)
					iblk = malloc(bsize, M_ZLFS,
					    M_WAITOK | M_ZERO);
				((u_int64_t *)iblk)[blkno - ZLFS_NDADDR] =
				    htole64(lba);
				used_indir = 1;
			}
			zi->zi_blocks++;
		}
		/* Write the single indirect block, if the file needed one. */
		if (used_indir) {
			error = zlfs_alloc_block(zmp, &lba);
			if (error != 0)
				goto data_fail;
			error = zlfs_write_block(zmp, lba, iblk);
			if (error != 0)
				goto data_fail;
			zi->zi_ib[0] = lba;
			zi->zi_blocks++;
		}
		free(blk, M_ZLFS, bsize);
		if (iblk != NULL)
			free(iblk, M_ZLFS, bsize);
		goto write_inode;
data_fail:
		free(blk, M_ZLFS, bsize);
		if (iblk != NULL)
			free(iblk, M_ZLFS, bsize);
		return error;
	}

write_inode:

	error = zlfs_alloc_block(zmp, &lba);
	if (error != 0)
		return error;
	blk = malloc(bsize, M_ZLFS, M_WAITOK | M_ZERO);
	zlfs_inode_htole((struct zlfs_inode *)blk, zi);
	error = zlfs_write_block(zmp, lba, blk);
	free(blk, M_ZLFS, bsize);
	if (error != 0)
		return error;

	zmp->zm_imap[znp->zn_ino] = lba;
	if (znp->zn_ino + 1 > zmp->zm_ninodes)
		zmp->zm_ninodes = znp->zn_ino + 1;
	return 0;
}

/*
 * Append a generation N+1 superblock to the superblock log.  This is
 * the atomic commit point.  When the active SB zone is full the log
 * ping-pongs to the other zone (which must be empty); resetting the
 * now-stale zone is left to a later garbage-collection pass.
 */
static int
zlfs_commit_super(struct zlfs_mount *zmp, u_int8_t *blk, u_int64_t generation,
    u_int64_t ckpt_lba)
{
	struct zlfs_super *zs = &zmp->zm_super, *d;
	u_int32_t bsize = zmp->zm_super.zs_block_size, crc;
	u_int64_t bpb = bsize / zmp->zm_secsize;
	u_int64_t sb_lba;
	int error, other;

	if (zmp->zm_sb_lba + bpb >
	    zmp->zm_sb_zstart[zmp->zm_sb_zidx] + zmp->zm_sb_zcap) {
		other = zmp->zm_sb_zidx ^ 1;
		/*
		 * Recycle the other superblock zone before switching into it.
		 * It holds only superseded superblocks -- the live one is in
		 * the current zone, which is left intact until the new
		 * superblock below is written -- so the reset is crash-safe:
		 * a failure any time before that write leaves the current
		 * zone's superblock in force.
		 */
		if (zmp->zm_zones[other].zst_wp_lba !=
		    zmp->zm_zones[other].zst_start_lba) {
			error = dk_zone_reset_kern(zmp->zm_dev,
			    zmp->zm_sb_zstart[other]);
			if (error != 0)
				return error;
			zmp->zm_zones[other].zst_wp_lba =
			    zmp->zm_zones[other].zst_start_lba;
			/* Cached blocks of the reset zone are now stale. */
			zlfs_cache_purge_dev(zmp);
		}
		zmp->zm_sb_zidx = other;
		zmp->zm_sb_lba = zmp->zm_sb_zstart[other];
	}
	sb_lba = zmp->zm_sb_lba;

	memset(blk, 0, bsize);
	d = (struct zlfs_super *)blk;
	d->zs_magic = htole32(ZLFS_MAGIC);
	d->zs_version = htole32(ZLFS_VERSION);
	d->zs_block_size = htole32(bsize);
	d->zs_flags = htole32(zs->zs_flags);
	memcpy(d->zs_uuid, zs->zs_uuid, sizeof(d->zs_uuid));
	d->zs_zone_size_lba = htole64(zs->zs_zone_size_lba);
	d->zs_zone_cap_lba = htole64(zs->zs_zone_cap_lba);
	d->zs_total_zones = htole64(zs->zs_total_zones);
	d->zs_generation = htole64(generation);
	d->zs_checkpoint_lba = htole64(ckpt_lba);
	d->zs_root_ino = htole64(zs->zs_root_ino);
	d->zs_last_mount_time = htole64(zs->zs_last_mount_time);
	crc = ZLFS_CRC32C_FINAL(zlfs_crc32c_update(ZLFS_CRC32C_INITIAL,
	    blk, bsize));
	d->zs_checksum = htole64(crc);

	error = zlfs_write_block(zmp, sb_lba, blk);
	if (error != 0)
		return error;
	zmp->zm_sb_lba += bpb;
	/*
	 * Track the active superblock zone's write pointer so the
	 * empty-zone test on a later ping-pong sees a filled zone as
	 * non-empty and resets it before switching back in.
	 */
	zmp->zm_zones[zmp->zm_sb_zidx].zst_wp_lba = zmp->zm_sb_lba;
	return 0;
}

/*
 * Commit all dirty inodes as a new log segment and checkpoint.
 *
 * Serialised against other writers by zm_wlock.  The node list is
 * walked without zm_lock (taking it here would invert the vnode-lock
 * ordering used by zlfs_vget); this bring-up therefore assumes commits
 * do not run concurrently with vnode creation or reclaim.  Proper
 * concurrent-safe commit is future work.
 */
int
zlfs_commit(struct zlfs_mount *zmp)
{
	struct zlfs_node *znp;
	struct zlfs_checkpoint *zc;
	u_int32_t bsize = zmp->zm_super.zs_block_size, crc;
	u_int64_t imap_lba, ckpt_lba, newgen, i, j, n, epb, nblocks, ninodes;
	u_int64_t *imap_lbas = NULL;
	u_int8_t *blk = NULL;
	int error = 0, ndirty = 0;

	if (zmp->zm_rdonly)
		return EROFS;

	rw_enter_write(&zmp->zm_wlock);

	LIST_FOREACH(znp, &zmp->zm_nodes, zn_entry) {
		if (znp->zn_dirty)
			ndirty++;
	}
	if (ndirty == 0) {
		rw_exit_write(&zmp->zm_wlock);
		return 0;
	}

	/*
	 * 0. Reclaim dead zones before allocating this segment, when free
	 * zones run low or the segment's worst-case size (each dirty
	 * inode's data blocks plus an indirect and an inode block, plus
	 * the map and checkpoint) might not fit in what is left.  The
	 * cleaner unions the durable (on-disk) and in-core live sets, so
	 * it never resets a block either the last checkpoint or the
	 * mounted filesystem can still reach.
	 */
	{
		u_int64_t bpb = bsize / zmp->zm_secsize;
		u_int64_t bpz = zmp->zm_super.zs_zone_cap_lba / bpb;
		u_int64_t need, headfree, freez;

		/* Checkpoint plus however many inode-map blocks. */
		need = 1 + howmany(zmp->zm_ninodes,
		    bsize / sizeof(u_int64_t));
		LIST_FOREACH(znp, &zmp->zm_nodes, zn_entry) {
			if (znp->zn_dirty)
				need += howmany(znp->zn_datalen, bsize) + 2;
		}
		headfree = (zmp->zm_log_zend - zmp->zm_log_lba) / bpb;
		freez = zlfs_zones_freecount(zmp);
		if (freez < ZLFS_GC_MIN_FREE || freez * bpz + headfree < need)
			zlfs_clean(zmp);
	}

	/* 1. Data and inode blocks for every dirty inode. */
	LIST_FOREACH(znp, &zmp->zm_nodes, zn_entry) {
		if (!znp->zn_dirty)
			continue;
		error = zlfs_commit_node(zmp, znp);
		if (error != 0)
			goto out;
	}

	blk = malloc(bsize, M_ZLFS, M_WAITOK | M_ZERO);
	imap_lbas = malloc(bsize - sizeof(struct zlfs_checkpoint), M_ZLFS,
	    M_WAITOK | M_ZERO);

	/*
	 * 2. New inode map (one block per epb entries).  Snapshot
	 * zm_ninodes once: the block count, the entries written, and the
	 * checkpoint's zc_ninodes below must all agree, and a create
	 * running between the reads could otherwise leave the durable
	 * checkpoint claiming more entries than its map blocks hold --
	 * which a later mount rejects.
	 */
	epb = bsize / sizeof(u_int64_t);
	ninodes = zmp->zm_ninodes;
	nblocks = howmany(ninodes, epb);
	if (nblocks > ZLFS_CKPT_NIMAP(bsize)) {
		error = EFBIG;
		goto out_blk;
	}
	for (j = 0; j < nblocks; j++) {
		error = zlfs_alloc_block(zmp, &imap_lba);
		if (error != 0)
			goto out_blk;
		memset(blk, 0, bsize);
		n = MIN(epb, ninodes - j * epb);
		for (i = 0; i < n; i++)
			((u_int64_t *)blk)[i] =
			    htole64(zmp->zm_imap[j * epb + i]);
		error = zlfs_write_block(zmp, imap_lba, blk);
		if (error != 0)
			goto out_blk;
		imap_lbas[j] = imap_lba;
	}

	/* 3. New checkpoint (generation N+1). */
	error = zlfs_alloc_block(zmp, &ckpt_lba);
	if (error != 0)
		goto out_blk;
	newgen = zmp->zm_super.zs_generation + 1;
	memset(blk, 0, bsize);
	zc = (struct zlfs_checkpoint *)blk;
	zc->zc_magic = htole32(ZLFS_MAGIC);
	zc->zc_version = htole32(ZLFS_VERSION);
	zc->zc_generation = htole64(newgen);
	zc->zc_root_ino = htole64(zmp->zm_super.zs_root_ino);
	zc->zc_imap_nblocks = htole64(nblocks);
	zc->zc_ninodes = htole64(ninodes);
	memcpy(zc->zc_uuid, zmp->zm_super.zs_uuid, sizeof(zc->zc_uuid));
	/* The map-block LBA array lives right after the header. */
	for (j = 0; j < nblocks; j++)
		((u_int64_t *)(blk + sizeof(struct zlfs_checkpoint)))[j] =
		    htole64(imap_lbas[j]);
	crc = ZLFS_CRC32C_FINAL(zlfs_crc32c_update(ZLFS_CRC32C_INITIAL,
	    blk, bsize));
	zc->zc_checksum = htole64(crc);
	error = zlfs_write_block(zmp, ckpt_lba, blk);
	if (error != 0)
		goto out_blk;

	/*
	 * 4. Flush the segment (data, inodes, map, checkpoint) to stable
	 * storage, then append the generation N+1 superblock as the
	 * durable commit point.
	 */
	error = dk_zone_flush_kern(zmp->zm_dev);
	if (error != 0)
		goto out_blk;
	error = zlfs_commit_super(zmp, blk, newgen, ckpt_lba);
	if (error != 0)
		goto out_blk;

	/*
	 * 5. Committed: only now advance the in-core superblock so a
	 * failed commit leaves it consistent with the durable state.
	 */
	zmp->zm_super.zs_generation = newgen;
	zmp->zm_super.zs_checkpoint_lba = ckpt_lba;
	LIST_FOREACH(znp, &zmp->zm_nodes, zn_entry)
		znp->zn_dirty = 0;

out_blk:
	free(imap_lbas, M_ZLFS, bsize - sizeof(struct zlfs_checkpoint));
	free(blk, M_ZLFS, bsize);
out:
	rw_exit_write(&zmp->zm_wlock);
	return error;
}
