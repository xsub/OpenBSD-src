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
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/pool.h>
#include <sys/stat.h>
#include <sys/vnode.h>
#include <sys/dkio.h>
#include <sys/zlfs.h>

#include <zlfs/zlfs_var.h>

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
 * Read one little-endian LBA entry out of the indirect block at lba.
 * Guards against an entry naming its own block (corrupt metadata that
 * would otherwise deadlock a caller re-breading a held buffer).
 */
static int
zlfs_indir_entry(struct zlfs_mount *zmp, u_int64_t lba, u_int64_t idx,
    u_int64_t *lbap)
{
	struct buf *bp;
	int error;

	error = zlfs_bread_block(zmp, lba, &bp);
	if (error != 0)
		return error;
	*lbap = letoh64(((u_int64_t *)bp->b_data)[idx]);
	brelse(bp);
	if (*lbap == lba)
		return EIO;
	return 0;
}

/*
 * Map a logical block number to its device LBA through the direct,
 * single-, double-, and triple-indirect pointers.  Indirect blocks are
 * read through the buffer cache, so repeated lookups stay cheap.
 * An unmapped block yields *lbap = 0.
 */
int
zlfs_bmap_read(struct zlfs_node *znp, u_int64_t blkno, u_int64_t *lbap)
{
	struct zlfs_mount *zmp = znp->zn_zmp;
	struct zlfs_inode *zi = &znp->zn_dinode;
	u_int64_t nindir = ZLFS_NINDIR(zmp);
	u_int64_t l1, l2;
	int error;

	if (blkno < ZLFS_NDADDR) {
		*lbap = zi->zi_db[blkno];
		return 0;
	}
	blkno -= ZLFS_NDADDR;
	if (blkno < nindir) {
		if (zi->zi_ib[0] == 0) {
			*lbap = 0;
			return 0;
		}
		return zlfs_indir_entry(zmp, zi->zi_ib[0], blkno, lbap);
	}
	blkno -= nindir;
	if (blkno < nindir * nindir) {
		if (zi->zi_ib[1] == 0) {
			*lbap = 0;
			return 0;
		}
		error = zlfs_indir_entry(zmp, zi->zi_ib[1], blkno / nindir,
		    &l1);
		if (error != 0)
			return error;
		if (l1 == 0) {
			*lbap = 0;
			return 0;
		}
		return zlfs_indir_entry(zmp, l1, blkno % nindir, lbap);
	}
	blkno -= nindir * nindir;
	if (blkno >= nindir * nindir * nindir)
		return EFBIG;		/* beyond triple indirect */
	if (zi->zi_ib[2] == 0) {
		*lbap = 0;
		return 0;
	}
	error = zlfs_indir_entry(zmp, zi->zi_ib[2], blkno / (nindir * nindir),
	    &l1);
	if (error != 0)
		return error;
	if (l1 == 0) {
		*lbap = 0;
		return 0;
	}
	error = zlfs_indir_entry(zmp, l1, (blkno / nindir) % nindir, &l2);
	if (error != 0)
		return error;
	if (l2 == 0) {
		*lbap = 0;
		return 0;
	}
	return zlfs_indir_entry(zmp, l2, blkno % nindir, lbap);
}

/* True if lba is inside [start, end). */
#define ZLFS_INRANGE(lba, start, end)	((lba) >= (start) && (lba) < (end))

/*
 * Return >0 if any block of the inode (data or metadata) falls in the
 * LBA range [start, end), 0 if none, <0 on a read error.  Used by the
 * copying cleaner to decide whether an inode must be relocated.
 */
int
zlfs_node_owns_range(struct zlfs_node *znp, u_int64_t start, u_int64_t end)
{
	struct zlfs_mount *zmp = znp->zn_zmp;
	struct zlfs_inode *zi = &znp->zn_dinode;
	u_int32_t bsize = zmp->zm_super.zs_block_size;
	u_int64_t nblk = howmany(zi->zi_size, bsize), b, lba;
	int error;

	if (zlfs_node_owns_meta(znp, start, end))
		return 1;
	for (b = 0; b < nblk; b++) {
		error = zlfs_bmap_read(znp, b, &lba);
		if (error != 0)
			return -1;
		if (ZLFS_INRANGE(lba, start, end))
			return 1;
	}
	return 0;
}

/*
 * Return nonzero if any of the inode's metadata blocks -- its inode
 * block or its indirect blocks (single, the double tree's L1/L2, and
 * the triple tree's top/mid/leaf) -- lie in [start, end).  The inode
 * block LBA comes from the in-core map.
 */
int
zlfs_node_owns_meta(struct zlfs_node *znp, u_int64_t start, u_int64_t end)
{
	struct zlfs_mount *zmp = znp->zn_zmp;
	struct zlfs_inode *zi = &znp->zn_dinode;
	u_int64_t nindir = ZLFS_NINDIR(zmp);
	struct buf *bp, *mbp;
	u_int64_t iblk, j, k, l1, mid, leaf;

	rw_enter_read(&zmp->zm_lock);
	iblk = (znp->zn_ino < zmp->zm_ninodes) ?
	    zmp->zm_imap[znp->zn_ino] : 0;
	rw_exit_read(&zmp->zm_lock);
	if (ZLFS_INRANGE(iblk, start, end))
		return 1;
	if (ZLFS_INRANGE(zi->zi_ib[0], start, end))
		return 1;
	if (ZLFS_INRANGE(zi->zi_ib[1], start, end))
		return 1;
	if (ZLFS_INRANGE(zi->zi_ib[2], start, end))
		return 1;
	if (zi->zi_ib[1] != 0 &&
	    zlfs_bread_block(zmp, zi->zi_ib[1], &bp) == 0) {
		for (j = 0; j < nindir; j++) {
			l1 = letoh64(((u_int64_t *)bp->b_data)[j]);
			if (ZLFS_INRANGE(l1, start, end)) {
				brelse(bp);
				return 1;
			}
		}
		brelse(bp);
	}
	if (zi->zi_ib[2] != 0 &&
	    zlfs_bread_block(zmp, zi->zi_ib[2], &bp) == 0) {
		for (j = 0; j < nindir; j++) {
			mid = letoh64(((u_int64_t *)bp->b_data)[j]);
			if (ZLFS_INRANGE(mid, start, end)) {
				brelse(bp);
				return 1;
			}
			/* Skip an entry aliasing the held top buffer:
			 * breading it would self-deadlock (corrupt
			 * metadata; the GC walk refuses it separately). */
			if (mid == 0 || mid == zi->zi_ib[2] ||
			    zlfs_bread_block(zmp, mid, &mbp) != 0)
				continue;
			for (k = 0; k < nindir; k++) {
				leaf = letoh64(((u_int64_t *)mbp->b_data)[k]);
				if (ZLFS_INRANGE(leaf, start, end)) {
					brelse(mbp);
					brelse(bp);
					return 1;
				}
			}
			brelse(mbp);
		}
		brelse(bp);
	}
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
	struct buf *bp;
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
		error = zlfs_bmap_read(znp, blkno, &lba);
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
	return 0;

fail:
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
 * Make block blkno of a regular file dirty: ensure the overlay array
 * covers it and the slot holds a block-sized buffer.  A fresh buffer is
 * zeroed; when rmw is set and the block exists on disk its current
 * contents are read in first, so a partial overwrite preserves the
 * rest.  A buffer that is already dirty is left as is.
 */
int
zlfs_dblk_prepare(struct zlfs_node *znp, u_int64_t blkno, int rmw)
{
	struct zlfs_mount *zmp = znp->zn_zmp;
	u_int32_t bsize = zmp->zm_super.zs_block_size;
	struct buf *bp;
	u_int8_t **narr;
	u_int64_t lba, want;
	int error;

	if (blkno >= ZLFS_MAXFILESZ(zmp) / bsize)
		return EFBIG;

	if (blkno >= znp->zn_ndblk) {
		/* Grow geometrically: sequential writes stay O(n).  The
		 * MAXFILESZ cap above bounds the doubling (no wrap) and
		 * keeps the resulting array within what malloc(9) serves. */
		want = 8;
		while (want < blkno + 1)
			want <<= 1;
		narr = mallocarray(want, sizeof(*narr), M_ZLFS,
		    M_WAITOK | M_ZERO);
		if (znp->zn_dblk != NULL) {
			memcpy(narr, znp->zn_dblk,
			    znp->zn_ndblk * sizeof(*narr));
			free(znp->zn_dblk, M_ZLFS,
			    znp->zn_ndblk * sizeof(*narr));
		}
		znp->zn_dblk = narr;
		znp->zn_ndblk = want;
	}
	if (znp->zn_dblk[blkno] != NULL)
		return 0;

	znp->zn_dblk[blkno] = malloc(bsize, M_ZLFS, M_WAITOK | M_ZERO);
	if (rmw) {
		error = zlfs_bmap_read(znp, blkno, &lba);
		if (error == 0 && lba != 0) {
			error = zlfs_bread_block(zmp, lba, &bp);
			if (error != 0) {
				free(znp->zn_dblk[blkno], M_ZLFS, bsize);
				znp->zn_dblk[blkno] = NULL;
				return error;
			}
			memcpy(znp->zn_dblk[blkno], bp->b_data, bsize);
			brelse(bp);
		}
		/* No on-disk block: stays zero (a hole being filled). */
	}
	znp->zn_dblkcnt++;
	return 0;
}

/*
 * Bound the overlay's memory: once a file holds ZLFS_DBLK_MAXBUFS
 * materialised buffers, commit so they flush and free before more are
 * materialised.  Callers hold the vnode lock; the commit recurses
 * through it exactly like the fsync path (rrwlock, RWL_DUPOK).  The
 * caller must have already grown zi_size over every materialised
 * block -- the commit only writes (and the success path only frees)
 * blocks below the current size, so an uncovered buffer would be
 * dropped unwritten.
 */
int
zlfs_dblk_backpressure(struct zlfs_node *znp)
{
	if (znp->zn_dblkcnt < ZLFS_DBLK_MAXBUFS)
		return 0;
	return zlfs_commit(znp->zn_zmp);
}

/* Free a regular file's dirty-block overlay. */
void
zlfs_dblk_free(struct zlfs_node *znp)
{
	struct zlfs_mount *zmp = znp->zn_zmp;
	u_int32_t b, bsize = zmp->zm_super.zs_block_size;

	if (znp->zn_dblk == NULL)
		return;
	for (b = 0; b < znp->zn_ndblk; b++) {
		if (znp->zn_dblk[b] != NULL)
			free(znp->zn_dblk[b], M_ZLFS, bsize);
	}
	free(znp->zn_dblk, M_ZLFS, znp->zn_ndblk * sizeof(*znp->zn_dblk));
	znp->zn_dblk = NULL;
	znp->zn_ndblk = 0;
	znp->zn_dblkcnt = 0;
}

/* Load a whole indirect block of little-endian LBAs into dst. */
static int
zlfs_load_entries(struct zlfs_mount *zmp, u_int64_t lba, u_int64_t *dst)
{
	struct buf *bp;
	u_int64_t i, nindir = ZLFS_NINDIR(zmp);
	int error;

	error = zlfs_bread_block(zmp, lba, &bp);
	if (error != 0)
		return error;
	for (i = 0; i < nindir; i++)
		dst[i] = letoh64(((u_int64_t *)bp->b_data)[i]);
	brelse(bp);
	return 0;
}

/*
 * Lazily materialise a host-endian entry table for one indirect block,
 * loading its current contents when the block already exists on disk
 * (src_lba != 0).  Used for the double tree's L2 blocks and the triple
 * tree's mid and leaf blocks alike.
 */
static int
zlfs_tab_get(struct zlfs_mount *zmp, u_int64_t src_lba, u_int64_t **tabp)
{
	u_int64_t nindir = ZLFS_NINDIR(zmp);

	if (*tabp != NULL)
		return 0;
	*tabp = mallocarray(nindir, sizeof(u_int64_t), M_ZLFS,
	    M_WAITOK | M_ZERO);
	if (src_lba != 0)
		return zlfs_load_entries(zmp, src_lba, *tabp);
	return 0;
}

/*
 * Commit a regular file's dirty blocks: write each overlay buffer to a
 * fresh LBA, keep the LBAs of clean blocks, and rebuild an indirect
 * block (single, the double tree's L2/L1, or the triple tree's
 * leaf/mid/top) only when an entry in its range changed.  Works on a
 * local copy of the inode so a failure leaves the in-core inode and the
 * dirty overlay untouched (the retry re-runs from intact state; partial
 * writes are unreferenced garbage).
 *
 * The triple tree hangs off zi_ib[2]: the top block's entries name mid
 * blocks (each covering nindir^2 data blocks), a mid block's entries
 * name leaf blocks, and a leaf block's entries name data blocks.  Leaf
 * tables are indexed globally (0 .. nleaf-1 across the whole valid
 * triple range) and materialised lazily, so only the leaves an actual
 * dirty block or boundary touches ever occupy memory.
 */
static int
zlfs_commit_file_blocks(struct zlfs_mount *zmp, struct zlfs_node *znp)
{
	struct zlfs_inode di = znp->zn_dinode;
	u_int32_t bsize = zmp->zm_super.zs_block_size;
	u_int64_t nindir = ZLFS_NINDIR(zmp);
	u_int64_t nindir2 = nindir * nindir;
	u_int64_t nblocks = howmany(di.zi_size, bsize);
	u_int64_t dvalid, tvalid, nleaf;
	u_int64_t *sient, *l1ent, **l2tab, *tent, **mtab, **ltab = NULL;
	u_int8_t *l2mod, *mmod, *lmod = NULL, *blk;
	u_int64_t b, i, lba, idx, tidx, slot, lf;
	int error = 0, s_dirty = 0, l1_dirty = 0, t_dirty = 0;

	if (nblocks > ZLFS_NDADDR + nindir + nindir2 + nindir2 * nindir)
		return EFBIG;

	/* Blocks held by the double tree (capped), then by the triple. */
	dvalid = (nblocks > ZLFS_NDADDR + nindir) ?
	    nblocks - ZLFS_NDADDR - nindir : 0;
	if (dvalid > nindir2)
		dvalid = nindir2;
	tvalid = (nblocks > ZLFS_NDADDR + nindir + nindir2) ?
	    nblocks - ZLFS_NDADDR - nindir - nindir2 : 0;
	nleaf = howmany(tvalid, nindir);

	sient = mallocarray(nindir, sizeof(u_int64_t), M_ZLFS,
	    M_WAITOK | M_ZERO);
	l1ent = mallocarray(nindir, sizeof(u_int64_t), M_ZLFS,
	    M_WAITOK | M_ZERO);
	l2tab = mallocarray(nindir, sizeof(u_int64_t *), M_ZLFS,
	    M_WAITOK | M_ZERO);
	l2mod = malloc(nindir, M_ZLFS, M_WAITOK | M_ZERO);
	tent = mallocarray(nindir, sizeof(u_int64_t), M_ZLFS,
	    M_WAITOK | M_ZERO);
	mtab = mallocarray(nindir, sizeof(u_int64_t *), M_ZLFS,
	    M_WAITOK | M_ZERO);
	mmod = malloc(nindir, M_ZLFS, M_WAITOK | M_ZERO);
	if (nleaf > 0) {
		ltab = mallocarray(nleaf, sizeof(u_int64_t *), M_ZLFS,
		    M_WAITOK | M_ZERO);
		lmod = malloc(nleaf, M_ZLFS, M_WAITOK | M_ZERO);
	}
	blk = malloc(bsize, M_ZLFS, M_WAITOK | M_ZERO);

	/* Current entry tables (host-endian), if the file has them. */
	if (di.zi_ib[0] != 0 &&
	    (error = zlfs_load_entries(zmp, di.zi_ib[0], sient)) != 0)
		goto out;
	if (di.zi_ib[1] != 0 &&
	    (error = zlfs_load_entries(zmp, di.zi_ib[1], l1ent)) != 0)
		goto out;
	if (di.zi_ib[2] != 0 &&
	    (error = zlfs_load_entries(zmp, di.zi_ib[2], tent)) != 0)
		goto out;

	/* Dirty blocks inside the file get fresh LBAs. */
	for (b = 0; b < znp->zn_ndblk && b < nblocks; b++) {
		if (znp->zn_dblk[b] == NULL)
			continue;
		error = zlfs_alloc_block(zmp, &lba);
		if (error != 0)
			goto out;
		error = zlfs_write_block(zmp, lba, znp->zn_dblk[b]);
		if (error != 0)
			goto out;
		if (b < ZLFS_NDADDR) {
			di.zi_db[b] = lba;
		} else if (b < ZLFS_NDADDR + nindir) {
			sient[b - ZLFS_NDADDR] = lba;
			s_dirty = 1;
		} else if ((idx = b - ZLFS_NDADDR - nindir) < nindir2) {
			slot = idx / nindir;
			error = zlfs_tab_get(zmp, l1ent[slot], &l2tab[slot]);
			if (error != 0)
				goto out;
			l2tab[slot][idx % nindir] = lba;
			l2mod[slot] = 1;
		} else {
			tidx = idx - nindir2;
			lf = tidx / nindir;
			slot = tidx / nindir2;
			error = zlfs_tab_get(zmp, tent[slot], &mtab[slot]);
			if (error != 0)
				goto out;
			error = zlfs_tab_get(zmp, mtab[slot][lf % nindir],
			    &ltab[lf]);
			if (error != 0)
				goto out;
			ltab[lf][tidx % nindir] = lba;
			lmod[lf] = 1;
		}
	}

	/* A shrunk file drops the pointers beyond its new end. */
	for (b = nblocks; b < ZLFS_NDADDR; b++)
		di.zi_db[b] = 0;
	for (i = (nblocks > ZLFS_NDADDR) ? nblocks - ZLFS_NDADDR : 0;
	    i < nindir; i++) {
		if (sient[i] != 0) {
			sient[i] = 0;
			s_dirty = 1;
		}
	}
	for (slot = howmany(dvalid, nindir); slot < nindir; slot++) {
		if (l1ent[slot] != 0) {
			l1ent[slot] = 0;	/* whole L2 subtree orphaned */
			l1_dirty = 1;
		}
	}
	if (dvalid % nindir != 0) {
		slot = dvalid / nindir;
		error = zlfs_tab_get(zmp, l1ent[slot], &l2tab[slot]);
		if (error != 0)
			goto out;
		for (i = dvalid % nindir; i < nindir; i++) {
			if (l2tab[slot][i] != 0) {
				l2tab[slot][i] = 0;
				l2mod[slot] = 1;
			}
		}
	}
	for (slot = howmany(tvalid, nindir2); slot < nindir; slot++) {
		if (tent[slot] != 0) {
			tent[slot] = 0;		/* whole mid subtree orphaned */
			t_dirty = 1;
		}
	}
	if (tvalid % nindir2 != 0) {
		slot = tvalid / nindir2;
		error = zlfs_tab_get(zmp, tent[slot], &mtab[slot]);
		if (error != 0)
			goto out;
		for (i = howmany(tvalid % nindir2, nindir); i < nindir; i++) {
			if (mtab[slot][i] != 0) {
				mtab[slot][i] = 0; /* whole leaf orphaned */
				mmod[slot] = 1;
			}
		}
	}
	if (tvalid % nindir != 0) {
		lf = tvalid / nindir;
		slot = lf / nindir;
		error = zlfs_tab_get(zmp, tent[slot], &mtab[slot]);
		if (error != 0)
			goto out;
		error = zlfs_tab_get(zmp, mtab[slot][lf % nindir], &ltab[lf]);
		if (error != 0)
			goto out;
		for (i = tvalid % nindir; i < nindir; i++) {
			if (ltab[lf][i] != 0) {
				ltab[lf][i] = 0;
				lmod[lf] = 1;
			}
		}
	}

	/*
	 * Compaction: force every live indirect block to move even though
	 * its entries did not change, so it leaves the zone being cleaned.
	 */
	if (znp->zn_relocate) {
		if (nblocks > ZLFS_NDADDR)
			s_dirty = 1;		/* rewrite single indirect */
		for (slot = 0; slot < howmany(dvalid, nindir); slot++) {
			error = zlfs_tab_get(zmp, l1ent[slot], &l2tab[slot]);
			if (error != 0)
				goto out;
			l2mod[slot] = 1;	/* rewrite this L2 (and L1) */
		}
		for (lf = 0; lf < nleaf; lf++) {
			slot = lf / nindir;
			error = zlfs_tab_get(zmp, tent[slot], &mtab[slot]);
			if (error != 0)
				goto out;
			error = zlfs_tab_get(zmp, mtab[slot][lf % nindir],
			    &ltab[lf]);
			if (error != 0)
				goto out;
			lmod[lf] = 1;	/* rewrite this leaf (mid, top follow) */
		}
	}

	/* Modified L2 blocks get fresh LBAs, recorded in the L1 table. */
	for (slot = 0; slot < nindir; slot++) {
		if (!l2mod[slot])
			continue;
		for (i = 0; i < nindir; i++)
			((u_int64_t *)blk)[i] = htole64(l2tab[slot][i]);
		error = zlfs_alloc_block(zmp, &lba);
		if (error != 0)
			goto out;
		error = zlfs_write_block(zmp, lba, blk);
		if (error != 0)
			goto out;
		l1ent[slot] = lba;
		l1_dirty = 1;
	}

	/* The single-indirect block. */
	if (nblocks <= ZLFS_NDADDR) {
		di.zi_ib[0] = 0;
	} else if (s_dirty || di.zi_ib[0] == 0) {
		for (i = 0; i < nindir; i++)
			((u_int64_t *)blk)[i] = htole64(sient[i]);
		error = zlfs_alloc_block(zmp, &lba);
		if (error != 0)
			goto out;
		error = zlfs_write_block(zmp, lba, blk);
		if (error != 0)
			goto out;
		di.zi_ib[0] = lba;
	}

	/* The double-indirect L1 block. */
	if (dvalid == 0) {
		di.zi_ib[1] = 0;
	} else if (l1_dirty || di.zi_ib[1] == 0) {
		for (i = 0; i < nindir; i++)
			((u_int64_t *)blk)[i] = htole64(l1ent[i]);
		error = zlfs_alloc_block(zmp, &lba);
		if (error != 0)
			goto out;
		error = zlfs_write_block(zmp, lba, blk);
		if (error != 0)
			goto out;
		di.zi_ib[1] = lba;
	}

	/* Modified leaf blocks get fresh LBAs, recorded in their mid. */
	for (lf = 0; lf < nleaf; lf++) {
		if (!lmod[lf])
			continue;
		slot = lf / nindir;
		error = zlfs_tab_get(zmp, tent[slot], &mtab[slot]);
		if (error != 0)
			goto out;
		for (i = 0; i < nindir; i++)
			((u_int64_t *)blk)[i] = htole64(ltab[lf][i]);
		error = zlfs_alloc_block(zmp, &lba);
		if (error != 0)
			goto out;
		error = zlfs_write_block(zmp, lba, blk);
		if (error != 0)
			goto out;
		mtab[slot][lf % nindir] = lba;
		mmod[slot] = 1;
	}

	/* Modified mid blocks get fresh LBAs, recorded in the top table. */
	for (slot = 0; slot < nindir; slot++) {
		if (!mmod[slot])
			continue;
		for (i = 0; i < nindir; i++)
			((u_int64_t *)blk)[i] = htole64(mtab[slot][i]);
		error = zlfs_alloc_block(zmp, &lba);
		if (error != 0)
			goto out;
		error = zlfs_write_block(zmp, lba, blk);
		if (error != 0)
			goto out;
		tent[slot] = lba;
		t_dirty = 1;
	}

	/* The triple-indirect top block. */
	if (tvalid == 0) {
		di.zi_ib[2] = 0;
	} else if (t_dirty || di.zi_ib[2] == 0) {
		for (i = 0; i < nindir; i++)
			((u_int64_t *)blk)[i] = htole64(tent[i]);
		error = zlfs_alloc_block(zmp, &lba);
		if (error != 0)
			goto out;
		error = zlfs_write_block(zmp, lba, blk);
		if (error != 0)
			goto out;
		di.zi_ib[2] = lba;
	}

	/*
	 * Block count by arithmetic: the no-holes invariant (gaps are
	 * materialised as zero blocks) means every block below the size
	 * exists, plus the live metadata blocks.
	 */
	di.zi_blocks = nblocks;
	if (nblocks > ZLFS_NDADDR)
		di.zi_blocks++;				/* single indirect */
	if (dvalid > 0)
		di.zi_blocks += 1 + howmany(dvalid, nindir); /* L1 + L2s */
	if (tvalid > 0)
		di.zi_blocks += 1 + howmany(tvalid, nindir2) + nleaf;
						/* top + mids + leaves */

	/* Success: adopt the new pointers, drop the overlay. */
	znp->zn_dinode = di;
	znp->zn_relocate = 0;
	zlfs_dblk_free(znp);
out:
	for (slot = 0; slot < nindir; slot++) {
		if (l2tab[slot] != NULL)
			free(l2tab[slot], M_ZLFS,
			    nindir * sizeof(u_int64_t));
		if (mtab[slot] != NULL)
			free(mtab[slot], M_ZLFS,
			    nindir * sizeof(u_int64_t));
	}
	for (lf = 0; lf < nleaf; lf++) {
		if (ltab[lf] != NULL)
			free(ltab[lf], M_ZLFS, nindir * sizeof(u_int64_t));
	}
	free(ltab, M_ZLFS, nleaf * sizeof(u_int64_t *));
	free(lmod, M_ZLFS, nleaf);
	free(mtab, M_ZLFS, nindir * sizeof(u_int64_t *));
	free(mmod, M_ZLFS, nindir);
	free(tent, M_ZLFS, nindir * sizeof(u_int64_t));
	free(l2tab, M_ZLFS, nindir * sizeof(u_int64_t *));
	free(l2mod, M_ZLFS, nindir);
	free(l1ent, M_ZLFS, nindir * sizeof(u_int64_t));
	free(sient, M_ZLFS, nindir * sizeof(u_int64_t));
	free(blk, M_ZLFS, bsize);
	return error;
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
		rw_enter_write(&zmp->zm_lock);
		zmp->zm_imap[znp->zn_ino] = 0;
		rw_exit_write(&zmp->zm_lock);
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
	} else if ((zi->zi_mode & S_IFMT) == S_IFREG) {
		/*
		 * Regular file: write only the dirty blocks.  This also
		 * runs for a dirty file with an empty overlay -- a bare
		 * truncate must still drop the pointers beyond the new
		 * end and recount zi_blocks.
		 */
		error = zlfs_commit_file_blocks(zmp, znp);
		if (error != 0)
			return error;
	}
	/* Anything else: metadata-only change; the inode rewrites as is. */

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

	rw_enter_write(&zmp->zm_lock);
	zmp->zm_imap[znp->zn_ino] = lba;
	if (znp->zn_ino + 1 > zmp->zm_ninodes)
		zmp->zm_ninodes = znp->zn_ino + 1;
	rw_exit_write(&zmp->zm_lock);
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
 * snapshotted under one zm_lock hold with vnode references (LK_NOWAIT
 * vget skips dying vnodes), then ALL their vnode locks are taken up
 * front with trylocks and held until the dirty flags clear -- so no
 * write can race an overlay adoption or slip between a node's commit
 * and its flag clear, and the checkpoint always covers a consistent
 * all-or-nothing set.  Contention releases everything and retries,
 * letting the holder (e.g. an fsync waiting for zm_wlock) finish.  The
 * fsync caller's own node recurses through its rrwlock (RWL_DUPOK).
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
	struct zlfs_node **dnodes = NULL;
	int error = 0, ndirty = 0, nd = 0, di2, nl, tries;

	if (zmp->zm_rdonly)
		return EROFS;

	for (tries = 0;; tries++) {
		rw_enter_write(&zmp->zm_wlock);
		/*
		 * Snapshot the dirty nodes and take their vnode locks, all
		 * or nothing.  Count and fill run under one zm_lock hold,
		 * so a node inserted at the list head while we slept in
		 * malloc cannot displace an older dirty node (say, the
		 * fsync caller's) out of the snapshot.
		 */
		rw_enter_read(&zmp->zm_lock);
		nd = 0;
		LIST_FOREACH(znp, &zmp->zm_nodes, zn_entry) {
			if (znp->zn_dirty)
				nd++;
		}
		if (nd == 0) {
			rw_exit_read(&zmp->zm_lock);
			rw_exit_write(&zmp->zm_wlock);
			free(dnodes, M_TEMP, ndirty * sizeof(*dnodes));
			return 0;
		}
		if (dnodes == NULL || nd > ndirty) {
			rw_exit_read(&zmp->zm_lock);
			rw_exit_write(&zmp->zm_wlock);
			free(dnodes, M_TEMP, ndirty * sizeof(*dnodes));
			ndirty = nd + 8;	/* headroom for new dirtiers */
			dnodes = mallocarray(ndirty, sizeof(*dnodes),
			    M_TEMP, M_WAITOK);
			continue;
		}
		nd = 0;
		LIST_FOREACH(znp, &zmp->zm_nodes, zn_entry) {
			if (!znp->zn_dirty || nd >= ndirty)
				continue;
			/* A dying vnode is mid-reclaim; its in-core state
			 * is lost either way (dirty-reclaim caveat). */
			if (vget(znp->zn_vnode, LK_NOWAIT) != 0)
				continue;
			dnodes[nd++] = znp;
		}
		rw_exit_read(&zmp->zm_lock);

		/*
		 * Trylocks only: blocking on a vnode lock while holding
		 * zm_wlock would deadlock against an fsync that holds the
		 * vnode and waits for zm_wlock.  On contention release
		 * everything and retry shortly; the holder finishes first.
		 */
		for (nl = 0; nl < nd; nl++) {
			if (rrw_enter(&dnodes[nl]->zn_lock,
			    RW_WRITE | RW_NOSLEEP) != 0)
				break;
		}
		if (nl == nd)
			break;		/* all locked; the commit proceeds */
		while (nl-- > 0)
			rrw_exit(&dnodes[nl]->zn_lock);
		rw_exit_write(&zmp->zm_wlock);
		for (nl = 0; nl < nd; nl++)
			vrele(dnodes[nl]->zn_vnode);
		if (tries >= 9) {
			free(dnodes, M_TEMP, ndirty * sizeof(*dnodes));
			return EBUSY;
		}
		tsleep_nsec(&zmp->zm_wlock, PPAUSE, "zlfsretry",
		    10 * 1000 * 1000ULL);
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
		for (di2 = 0; di2 < nd; di2++) {
			u_int64_t nindir = ZLFS_NINDIR(zmp);
			u_int64_t fnb, fdv, ftv;
			u_int32_t b;

			znp = dnodes[di2];
			if (znp->zn_data != NULL) {
				/* Directory: rewritten whole. */
				need += howmany(znp->zn_datalen, bsize) + 2;
				continue;
			}
			/*
			 * File: one write per dirty data block plus, worst
			 * case (compaction relocation via zn_relocate, or a
			 * bare shrink), a fresh copy of EVERY indirect
			 * block the file's size implies, plus the inode
			 * block.  Anything less lets a forced full-tree
			 * rewrite start without the cleaner having run and
			 * die mid-commit on ENOSPC -- and, with the flags
			 * only cleared on success, retry forever.
			 */
			fnb = howmany(znp->zn_dinode.zi_size, bsize);
			fdv = (fnb > ZLFS_NDADDR + nindir) ?
			    fnb - ZLFS_NDADDR - nindir : 0;
			if (fdv > nindir * nindir)
				fdv = nindir * nindir;
			ftv = (fnb > ZLFS_NDADDR + nindir + nindir * nindir) ?
			    fnb - ZLFS_NDADDR - nindir - nindir * nindir : 0;
			need += 1;				/* inode */
			if (fnb > ZLFS_NDADDR)
				need += 1;			/* single */
			if (fdv > 0)
				need += 1 + howmany(fdv, nindir); /* L1+L2s */
			if (ftv > 0)
				need += 1 + howmany(ftv, nindir * nindir) +
				    howmany(ftv, nindir); /* top+mids+leaves */
			for (b = 0; b < znp->zn_ndblk; b++) {
				if (znp->zn_dblk[b] != NULL)
					need += 1;
			}
		}
		headfree = (zmp->zm_log_zend - zmp->zm_log_lba) / bpb;
		freez = zlfs_zones_freecount(zmp);
		if (freez < ZLFS_GC_MIN_FREE || freez * bpz + headfree < need)
			zlfs_clean(zmp);
	}

	/*
	 * 1. Data and inode blocks for every dirty inode, each under its
	 * vnode lock so writes cannot race the overlay adoption.
	 */
	for (di2 = 0; di2 < nd; di2++) {
		error = zlfs_commit_node(zmp, dnodes[di2]);
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
		/* zm_lock: a concurrent create may grow (replace) the map. */
		rw_enter_read(&zmp->zm_lock);
		for (i = 0; i < n; i++)
			((u_int64_t *)blk)[i] =
			    htole64(zmp->zm_imap[j * epb + i]);
		rw_exit_read(&zmp->zm_lock);
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
	for (di2 = 0; di2 < nd; di2++)
		dnodes[di2]->zn_dirty = 0;

out_blk:
	free(imap_lbas, M_ZLFS, bsize - sizeof(struct zlfs_checkpoint));
	free(blk, M_ZLFS, bsize);
out:
	/* Dirty flags cleared (or preserved on failure) under the node
	 * locks; release them, then the commit lock, then the refs. */
	for (di2 = 0; di2 < nd; di2++)
		rrw_exit(&dnodes[di2]->zn_lock);
	rw_exit_write(&zmp->zm_wlock);
	for (di2 = 0; di2 < nd; di2++)
		vrele(dnodes[di2]->zn_vnode);
	free(dnodes, M_TEMP, ndirty * sizeof(*dnodes));
	return error;
}
