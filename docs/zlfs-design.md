# ZLFS Design

ZLFS is a log-structured filesystem for host-managed zoned block
devices (NVMe ZNS, SCSI ZBC/SMR), built for OpenBSD.  Its contract
with the device is absolute: **every write lands on a zone's write
pointer, and nothing live is ever overwritten**.  Everything else in
the design follows from that constraint.

Status and hardware evidence per feature: `../functional_testing.md`.
On-disk structures: `sys/sys/zlfs.h`.  Kernel: `sys/zlfs/`.

## 1. Device layout

```
 zone 0        zone 1        zone 2 ... zone N-1
+-------------+-------------+---------------------------------------+
| superblock  | superblock  |            data zones                 |
| log (ping)  | log (pong)  |  circular log: data, inodes, inode    |
|             |             |  map, checkpoints                     |
+-------------+-------------+---------------------------------------+
  gen 0,1,2..   gen k,k+1..    ^ write head          ^ reclaimed
                               | (append only)       | (reset by GC)
```

Zones 0-1 hold the **superblock generation log**: each commit appends
a superblock with generation N+1.  When the active zone fills, the
other zone is physically reset and the log "ping-pongs" into it.  The
live superblock is always in the current zone, so the reset is
crash-safe by construction.  All other zones form one **circular data
log**.

## 2. On-disk format (v2)

All multi-byte fields are little-endian; every metadata block carries
a CRC32C over the whole block.  Reachability is a tree rooted in the
newest superblock:

```
superblock (gen N)
   └─ zs_checkpoint_lba ──> checkpoint block
                              ├─ zc_imap_nblocks + LBA array (in-block)
                              │     └─> inode-map blocks (ino -> LBA)
                              │            └─> inode blocks
                              │                  ├─ zi_db[12]   direct
                              │                  ├─ zi_ib[0] ─> single ind.
                              │                  └─ zi_ib[1] ─> L1 ─> L2 ─> data
                              └─ zc_root_ino
```

- Inode map: up to ~500 map blocks addressed from the checkpoint block
  (~256k inodes at 4 KB).  `imap[ino] == 0` means "does not exist".
- Files: 12 direct + single-indirect + double-indirect pointers
  (~1 GB at 4 KB).  Directories are flat arrays of 128-byte entries,
  capped at direct+single range (`ZLFS_MAXDIRSZ`).
- **No holes**: gaps created by writing past EOF or truncate-grow are
  materialised as zero blocks, so block counts are arithmetic.

## 3. Write path

Regular files never buffer whole contents.  Each in-core inode carries
a **sparse dirty-block overlay** (`zn_dblk[b]`, NULL = clean):

```
 write(2) ────────────────────────────────┐
                                          v
   for each affected block b:      +---------------+
     if partially covered and      | overlay buf b |  (RMW: old block
     b overlaps live data ────────>| old ⊕ new     |   read from disk)
     else zero + new bytes         +---------------+
                                          |
 read(2)/mmap fault: overlay if dirty,    |   sync/fsync
 else disk via bmap (buffer cache)        v
                                       COMMIT
```

Directories keep whole contents in core (`zn_data`) — their code
linear-scans entries; they are not mmapable.

## 4. Commit anatomy

A commit turns all dirty in-core state into a fresh log segment, then
makes it visible atomically:

```
 0. gate: reclaim dead zones if the segment might not fit (GC, §5)
 1. per dirty file: write ONLY dirty blocks to fresh LBAs
      clean blocks keep their old LBAs (no rewrite amplification);
      rebuild only the indirect blocks whose entries changed
      (single / L2 / L1, lazily loaded); all on a LOCAL inode copy —
      any failure leaves in-core state intact for retry
    per dirty dir: rewrite its blocks; dead inodes (nlink 0) are
      dropped from the map
 2. write inode-map blocks            \
 3. write checkpoint (gen N+1)         >  still invisible
 4. SYNCHRONIZE CACHE (durability)    /
 5. append superblock gen N+1  <=== THE atomic commit point
 6. only now advance in-core generation + clear dirty flags
```

A crash anywhere before step 5 leaves generation N in force; the new
blocks are unreferenced garbage.  There is no journal — the log *is*
the journal.

## 5. Garbage collection

The allocator is circular: when the head zone fills it wraps to the
next empty zone.  When free zones run low (or a segment may not fit),
the cleaner computes liveness as the **union of three sets** and
resets any written data zone reached by none:

1. **Durable**: everything reachable from the on-disk checkpoint,
   re-read from disk — the in-core map is *not* a substitute (unlink/
   rename clear entries before the next commit; a failed commit leaves
   new entries).  Erasing any of this would lose committed data.
2. **In-core map**: blocks a retry commit still references.
3. **Open vnodes**: unlinked-but-open files live in neither map.

Any read failure aborts the scan (reclaim nothing).  The log head is
never reset while it has room.  Fully dead zones are reset directly;
mixed zones go through the **copying cleaner**: when free zones run
low, the sync path (which holds no vnode locks) picks the least-live
written zone and pulls every live data block owned by any inode into
that inode's dirty overlay, marking inodes whose *metadata* sits in
the victim for a full indirect-tree rewrite (`zn_relocate`) — so the
next commit relocates everything, the zone goes dead, and a later
pass resets it.  After any
reset the device buffer cache is purged (`vinvalbuf`): ZLFS writes
bypass the cache, and reset+reuse is the only way an LBA's contents
change, so this single invalidation point keeps cached reads coherent.

## 6. Mount, discovery, recovery

```
mount ─> scan SB zones 0+1 by write pointer (conventional: forward
         scan) ─> pick highest CRC-valid generation ─> load checkpoint
         ─> load inode map ─> find circular log head = the single
         written-but-not-full data zone (else first empty; else mount
         with exhausted head and let the first commit's GC free space)
```

Crash recovery *is* mount: there is nothing to replay or fsck.  The
newest durable superblock names a checkpoint whose whole tree was
flushed before the superblock was appended.

## 7. Coherence and locking

- **mmap**: UVM fills pages via `VOP_READ` (overlay-aware) and caches
  them; every content change calls `uvm_vnp_setsize`/`uvm_vnp_uncache`
  (the ffs pattern), so mapped views track writes and truncates.
- **Locks**: `zm_wlock` serialises commits; `zm_lock` guards the node
  list and inode-map swaps; per-inode `rrwlock` vnode locks follow the
  ufs contracts (documented per-VOP in `zlfs_vnops.c`).  The write
  path no longer assumes single-threading: every `zm_imap`
  reader/writer holds `zm_lock`; a commit snapshots dirty nodes under
  `zm_lock` with vnode references and commits each under its vnode
  lock — all locks taken up front with trylocks and held until the
  dirty flags clear, so the checkpoint is an all-or-nothing set and no
  write slips between a node's commit and its flag clear; contention
  releases everything and retries after the holder finishes (avoiding
  the vnode->wlock inversion).

## 8. Why these choices

- **SB generation log, not rewrite-in-place**: ZNS has no conventional
  zones; a fixed superblock location would require overwrite.
- **Raw in-kernel writes** (`dk_zone_write_kern`, bypassing the buffer
  cache and the sd(4) write gate): the FS itself guarantees the
  write-pointer discipline, so the segment writer needs exact LBA
  control ("approach C").
- **Inode numbers are reused** (lowest free in map + in-core list) —
  a one-block map exhausted after ~500 creates otherwise.
- **Never trust in-core state for reclamation**: the GC re-reads the
  durable checkpoint because in-core state runs ahead of disk.  This
  rule was bought with three caught data-loss bugs (see the defect log
  in `../functional_testing.md` §6).
