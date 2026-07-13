/*	$OpenBSD$	*/

/*
 * ZLFS (Zoned Log-Structured File System)
 * Definitions and on-disk layout.
 *
 * All on-disk multi-byte fields are little-endian.  All on-disk
 * addresses are absolute device LBAs unless noted otherwise.
 *
 * Superblock placement
 * --------------------
 * ZLFS must work on devices that have no conventional (randomly
 * writable) zones at all: NVMe ZNS namespaces commonly expose only
 * sequential-write-required zones, and the raw write path only admits
 * writes at a zone's write pointer.  A fixed, rewrite-in-place
 * superblock is therefore impossible.  Instead the superblock is a
 * generation-numbered log, ping-ponged across the first two zones:
 *
 *   - Zones 0 and 1 (ZLFS_SB_ZONES) are reserved for superblocks.
 *   - Every superblock update appends one zs_block_size-sized block,
 *     carrying a monotonically increasing zs_generation and a
 *     checksum, at the active zone's write pointer.
 *   - When the active zone fills, the next superblock is written to
 *     the start of the other (empty) zone; the full zone is then
 *     reset.  At least one zone always holds a valid superblock.
 *   - Mount discovery: report zones 0 and 1; in each, scan backward
 *     from the write pointer for the newest block with a valid
 *     magic + checksum (a torn tail block simply fails the checksum
 *     and the scan continues).  The highest zs_generation wins.  On
 *     conventional zones (SMR disks where zones 0/1 are CMR, or plain
 *     disks) there is no write pointer: scan forward instead, and
 *     overwrite cyclically rather than resetting.
 *   - Bootstrap: zs_block_size is not yet known when the scan starts,
 *     but the first entry of each superblock zone always sits at the
 *     zone-start LBA and static geometry is identical in every
 *     generation, so the block size (and thus the scan stride) is
 *     learned from a zone-start entry first.  The write-before-reset
 *     ordering guarantees at least one zone-start entry is intact.
 *   - Crash recovery: if a crash lands between filling one zone and
 *     resetting the other, both zones may be full.  Recovery stays
 *     unambiguous — generations strictly increase — so mount resets
 *     the zone holding the lower generation before appending again.
 *
 * This works identically on ZNS, host-managed SMR and conventional
 * devices, and never requires an in-place overwrite.
 */

#ifndef _SYS_ZLFS_H_
#define _SYS_ZLFS_H_

#include <sys/types.h>

#define ZLFS_MAGIC	0x54BDCC01	/* "ZLFS" NEW */
#define ZLFS_VERSION	2	/* 2: multi-block inode map */

/* Zones reserved at the start of the device for the superblock log. */
#define ZLFS_SB_ZONES	2

/* Well-known inode numbers.  0 and 1 are reserved and never allocated. */
#define ZLFS_ROOT_INO	2
#define ZLFS_FIRST_INO	3	/* first allocatable inode number */

/*
 * ZLFS Superblock.
 *
 * One instance per superblock-log entry; padded with zeroes to
 * zs_block_size on disk.  zs_checksum is CRC32C over the whole
 * zs_block_size-sized block with zs_checksum taken as zero.
 * Static geometry (block size, zone size, total zones, UUID) must be
 * identical in every generation; mutable state (generation,
 * checkpoint, root inode) is what each new entry updates.
 */
struct zlfs_super {
	u_int32_t	zs_magic;
	u_int32_t	zs_version;
	u_int32_t	zs_block_size;		/* bytes, power of two */
	u_int32_t	zs_flags;
	u_int8_t	zs_uuid[16];		/* filesystem identity */

	/*
	 * zs_zone_cap_lba is the smallest per-zone capacity
	 * (dz_capacity_lba) found on the device at mkfs time; ZLFS
	 * uses every zone only up to this uniform bound.
	 */
	u_int64_t	zs_zone_size_lba;	/* device zone size */
	u_int64_t	zs_zone_cap_lba;	/* usable LBAs per zone */
	u_int64_t	zs_total_zones;

	/*
	 * zs_checkpoint_lba == 0 means no checkpoint exists yet (freshly
	 * created filesystem).  LBA 0 lies inside superblock zone 0, so
	 * it can never address a real checkpoint block.
	 */
	u_int64_t	zs_generation;		/* superblock generation */
	u_int64_t	zs_checkpoint_lba;	/* newest checkpoint block */
	u_int64_t	zs_root_ino;		/* root directory inode # */
	u_int64_t	zs_last_mount_time;	/* seconds since epoch */

	u_int64_t	zs_reserved[6];
	u_int64_t	zs_checksum;		/* CRC32C, see above */
};

/*
 * ZLFS Inode.
 * Log-structured: inodes are relocated on every modification, so an
 * inode's location is found via the inode map addressed from the
 * checkpoint, and zi_gen disambiguates stale copies found by the
 * cleaner or by fsck.
 *
 * Exactly 256 bytes so inodes pack a power-of-two per block;
 * zi_reserved is the growth area for future fields.
 */
struct zlfs_inode {
	u_int64_t	zi_ino;		/* inode number */
	u_int64_t	zi_gen;		/* generation (vget/NFS, cleaner) */
	u_int64_t	zi_size;	/* file size in bytes */
	u_int64_t	zi_blocks;	/* number of data blocks */
	u_int32_t	zi_mode;	/* file type & permissions */
	u_int32_t	zi_uid;
	u_int32_t	zi_gid;
	u_int32_t	zi_nlink;	/* link count */
	u_int32_t	zi_flags;
	u_int32_t	zi_spare;

	/* Timestamps */
	u_int64_t	zi_atime;	/* seconds */
	u_int64_t	zi_mtime;
	u_int64_t	zi_ctime;
	u_int64_t	zi_btime;	/* birth time */
	u_int32_t	zi_atimensec;
	u_int32_t	zi_mtimensec;
	u_int32_t	zi_ctimensec;
	u_int32_t	zi_btimensec;

	/* Data block pointers (absolute LBAs) */
#define ZLFS_NDADDR	12
	u_int64_t	zi_db[ZLFS_NDADDR];	/* direct blocks */
	u_int64_t	zi_ib[3];	/* indirect, double, triple */

	u_int64_t	zi_reserved[4];
};

/*
 * Zone Summary.
 * Written at the beginning of each newly opened data zone; identifies
 * the log segment the zone belongs to.  On sequential-write-required
 * zones this block can never be rewritten, so every field holds a
 * value known at zone-open time: zz_num_blocks is the zone's data
 * capacity in zs_block_size blocks (not a live-block count).
 * zz_checksum is CRC32C over the filesystem UUID (zs_uuid) followed
 * by the structure with zz_checksum taken as zero, so summaries left
 * behind by a previous mkfs on the same device do not validate.
 */
struct zlfs_zone_summary {
	u_int32_t	zz_magic;	/* ZLFS_MAGIC */
	u_int32_t	zz_flags;
	u_int64_t	zz_seq_num;	/* segment log sequence number */
	u_int64_t	zz_zone_start_lba; /* zone this summary describes */
	u_int32_t	zz_num_blocks;	/* zone data capacity in blocks */
	u_int32_t	zz_spare;
	u_int64_t	zz_checksum;	/* UUID-seeded CRC32C, see above */
};

/*
 * On-disk directory entry.  Directories are a flat array of fixed-size
 * records so lookups and readdir need no length parsing; an unused
 * slot has zd_ino == 0.  Names are not NUL-terminated on disk; only
 * zd_namlen bytes of zd_name are valid.
 */
#define ZLFS_DIRENT_SIZE	128
#define ZLFS_MAXNAMLEN		(ZLFS_DIRENT_SIZE - 16)	/* 112 */

struct zlfs_dirent {
	u_int64_t	zd_ino;		/* inode number, 0 if unused */
	u_int8_t	zd_type;	/* DT_* from <sys/dirent.h> */
	u_int8_t	zd_namlen;	/* <= ZLFS_MAXNAMLEN */
	u_int8_t	zd_pad[6];
	char		zd_name[ZLFS_MAXNAMLEN];
};

/*
 * Checkpoint: the filesystem's mount entry point.  zs_checkpoint_lba
 * in the superblock points at the newest checkpoint block.  The
 * checkpoint locates the inode map -- a little-endian array of
 * u_int64_t device LBAs indexed by inode number, where imap[ino] == 0
 * means the inode does not exist -- and names the root inode.
 *
 * The map spans zc_imap_nblocks block-sized extents; their device LBAs
 * are a little-endian u_int64_t array that starts immediately after
 * this header inside the checkpoint block, so a checkpoint addresses
 * up to ZLFS_CKPT_NIMAP(bsize) map blocks (about 500 at a 4096-byte
 * block, or ~256000 inodes).  zc_checksum is CRC32C over the whole
 * zs_block_size-sized block with zc_checksum taken as zero.
 */
struct zlfs_checkpoint {
	u_int32_t	zc_magic;	/* ZLFS_MAGIC */
	u_int32_t	zc_version;	/* ZLFS_VERSION */
	u_int64_t	zc_generation;	/* matches the superblock generation */
	u_int64_t	zc_root_ino;
	u_int64_t	zc_imap_nblocks; /* inode-map blocks; LBAs follow */
	u_int64_t	zc_ninodes;	/* number of inode-map entries */
	u_int8_t	zc_uuid[16];	/* must match the superblock UUID */
	u_int64_t	zc_reserved[4];
	u_int64_t	zc_checksum;
};

/* Inode-map block LBAs storable in one checkpoint block. */
#define ZLFS_CKPT_NIMAP(bsize) \
	(((bsize) - sizeof(struct zlfs_checkpoint)) / sizeof(u_int64_t))

#endif /* _SYS_ZLFS_H_ */
