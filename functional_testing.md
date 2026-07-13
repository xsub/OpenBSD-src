# ZLFS Functional Testing Status

Detailed, running log of what works, what is being tested, and what is
deferred.  Companion to the roadmap prose in `README.md`; this file
carries the concrete evidence: commits, kernel builds, test runs, and
the defects each verification round caught.

Update this file with every phase: move rows between tables as their
status changes, and append to the defect log (§6) whenever a
verification round or a VM run finds something.

Environment: QEMU arm64 VM (`-ZBD-dev` kernels), NVMe ZNS disk `sd1`:
128 zones x 131072 LBAs (512 B), 64 MB/zone, 8 GB total; ZLFS block
size 4096; superblock (SB) zones 0-1, 126 data zones.

## 1. Working — VM-validated

| Feature | Commits | Evidence (kernel / date / test) |
|---|---|---|
| In-kernel zone report (`dk_zone_report_kern`, NVMe ZNS adapter op + SCSI ZBC) | early series | used by every mount; exercised in all runs below |
| `newfs_zlfs(8)`: zone enumeration, geometry validation, SB-zone reset, root dir + sample file + inodes + imap + checkpoint + gen-0 SB | early series | every test run starts with it; output cross-checked (checkpoint at LBA 262192) |
| Read-only mount + superblock-log discovery (CRC32C generation scan) | through `17d4d32efda` | ZBD#5, 2026-07-11: mount, `ls -la`, `cat welcome` (81 B), `stat` correct |
| Lookup hang fix (parent unlock + `PDIRUNLOCK`, cd9660 pattern) | `9230a13323d` | ZBD#5+: repeated `ls` no longer wedges; regression-checked in all later runs |
| Write path: `create`/`write`/`fsync`/truncate, commit engine (segment -> gen N+1 SB) | through `7f3eb177dc1` | ZBD#9, 2026-07-11: `echo hi > f; sync; umount; mount; cat f` exact round trip |
| Subdirectories (`mkdir`/`rmdir`), `unlink`, real `..` (ufs lock dance), dir link counts | `c13d9428e1f` | ZBD, 2026-07-12: bat.sh 6/6 — mkdir/rm/rmdir + persistence across remount |
| Durable commit: `SYNCHRONIZE CACHE` (`dk_zone_flush_kern`) before SB append | `c13d9428e1f` | same run; every later sync/umount/remount cycle exercises it |
| Single indirect blocks (files to ~2 MB) + dynamically sized in-core buffer | `f28bb160d08` | same run: 800 KB file (>48 KB forces indirect) cksum-intact across remount |
| `rename`: files, same-dir, into subdir, overwrite; exact 4-vnode disposal | `58cd1c130b5` | same run: `mv` file into subdir, source gone, same-dir rename ok |
| `rename`: directory across parents (`..` reparent, subtree-cycle rejection, nlink transfer) | `6fc9812d141` | same run: `mv d e/` works, cycle move rejected, persists across remount |
| Circular allocator + uncached (`B_INVAL`) reads — non-wrap operation | `16103441ec6` | ZBD#11, 2026-07-12: churn v1 (300 x 2 MB write/rm) — no regression, keeper file cksum-intact after churn and remount |
| Data-zone garbage collection + log wrap-around, END TO END | `16103441ec6` | ZBD#13, 2026-07-13: churn v2 full PASS — 150/150 zone-fills on a 126-zone device, no ENOSPC; df sawtooth 48/65/81/**98% @ i100** then **18% @ i125** (~6.6 GB reclaimed in one cleaner pass); keeper cksum intact after churn AND after remount |
| Inode-number reuse in `zlfs_ialloc` | `07d27328072` | same run: 4800 creates against the 512-entry map (32 live at a time), sailed past the old ~i16 exhaustion point |
| Wrap-aware `zlfs_log_init` (circular head discovery; full fs still mounts) | `2570934d9db` | same run: the final remount found the mid-device head after a full wrap — previously ENOSPC (see §6) |

## 2. In testing — pushed, awaiting VM evidence

| Feature | Commit | Verified so far | Missing evidence |
|---|---|---|---|
| SB-zone recycling (reset stale SB zone on ping-pong) | `422cb631771` | 2 adversarial rounds (caught FWRITE and wp-tracking bugs, then pass) | never exercised on VM: one SB zone holds 16384 superblocks (131072 LBAs / 8 per SB), so the first ping-pong needs ~16k commits; churn v2 produces ~300 — a dedicated many-commit test or a smaller-zone QEMU profile is needed |

## 3. Under analysis / known gaps

| Item | Concrete detail | Impact | Plan |
|---|---|---|---|
| Unlinked-but-open file defers reclaim | zone pinned by pass 3 until the fd closes AND a later gated commit runs (`zlfs_commit` returns early when nothing is dirty) | space-liveness only, no data loss | acceptable for bring-up; revisit with the copying cleaner |
| Corrupt-metadata hardening | a self-referential indirect entry (entry LBA == `zi_ib[0]`) would sleep forever in `getblk` (B_BUSY held by caller) | unreachable with well-formed metadata (allocator hands out distinct LBAs) | add bounds/identity checks on indirect entries when fsck-style validation is designed |
| `statfs` free-space approximation | `f_bfree` counts EMPTY-condition zones only; mixed zones count as used; live_bytes is a zero/nonzero flag, inflated by duplicate marking | cosmetic accounting | proper per-zone byte accounting with the copying cleaner |
| `zst_live_bytes` semantics | only tested against 0 in the reset loop; not a true byte count | none today; trap for future code | rename or fix when the copying cleaner needs real counts |
| Inode map is a single block | hard cap of 512 inode numbers (510 usable files/dirs alive at once); numbers are now reused after removal, but the live-count ceiling stands | churning workloads fine; wide trees will hit ENOSPC at 510 live inodes | multi-block inode map (format change) |

## 4. Later (roadmap order)

| Item | Why deferred | Prerequisite |
|---|---|---|
| Double/triple indirect blocks (`zi_ib[1]`, `zi_ib[2]` — format-reserved, cleaner refuses to reclaim if it ever sees them set) | file cap of ~2 MB acceptable for bring-up; touches commit, bmap, load paths | none; natural next format step |
| Block-level buffer-cache integration | removes both the whole-file in-core buffering (RAM cap on file size) and the `B_INVAL` no-cache tax; large rework of read/write/commit | design for cache coherence with raw zoned writes and zone resets |
| Copying cleaner (compact mixed live/dead zones) | today only fully-dead zones are reclaimed; churn leaves live blocks pinning zones | segment-liveness accounting (real `zst_live_bytes`) |
| Concurrency-safe commit | commit walks `zm_nodes` without `zm_lock` (documented single-threaded assumption); lock-order rework is the highest blind risk | stable feature set; likely needs VM stress harness first |

## 5. Test inventory

| Script | Location | What it proves |
|---|---|---|
| `dkzone-vm-smoke.sh` and siblings | `regress/sys/sys/dkzone/` | raw zone ABI: report, write gate, zone management on SCSI ZBC + NVMe ZNS |
| bat.sh (ad-hoc, VM `/tmp`) | not in repo | six-phase functional pass: files, subdirs, all rename shapes, unlink/rmdir, indirect-block file, remount persistence (6/6 on 2026-07-12) |
| `zlfs-gc-churn.sh` | `regress/sys/sys/dkzone/` | GC + wrap: 150 zone-fills on a 126-zone device cannot complete without reclaim; keeper file guards against the cleaner eating live data |

## 6. Defect log — what verification and testing caught, per phase

Methodology: every phase runs 2-4 parallel adversarial review agents
(distinct lenses: VOP contracts, crash consistency, allocator math,
compile) against the live tree before push; VM runs validate after
push.  "Round N" = one such multi-agent pass.

| Phase | Defect | Severity | Found by | Fix |
|---|---|---|---|---|
| read path | `<sys/lock.h>` missing; `htolem64` is kernel-only | build breaks | VM build | include; `htole64`+`memcpy` |
| read path | `ls` hang on 2nd access: lookup returned child without unlocking parent / setting `PDIRUNLOCK`; leaked rrwlock wedged root | blocker | VM behavior + 3-agent workflow | cd9660-pattern unlock dance (`9230a13323d`) |
| write path | `EJUSTRETURN` without `SAVENAME` -> double `pool_put` panic on create | panic | VM | set `SAVENAME` on create path |
| write path | NULL `zm_imap` on checkpoint-less RW mount; orphan inode on create error; generation desync on failed commit | blockers | verify round | allocate in `log_init`; `zlfs_idrop`; snapshot `newgen`, apply after success |
| hierarchy | `zlfs_mkdir`/`zlfs_rmdir` never `vput(dvp)` — VOP_MKDIR/RMDIR wrappers do not release the parent (unlike CREATE/REMOVE); every mkdir leaked a locked vnode ref | blocker | 2 independent verify agents (both cited `domkdirat`, `ufs_mkdir`) | `vput(dvp)` on all paths (`c13d9428e1f`) |
| rename | target destroyed before source reattach: ENOSPC mid-rename lost the target (POSIX: failed rename leaves both names) | bug | verify round | all-or-nothing reorder: load + reserve slot before first mutation (`58cd1c130b5`) |
| SB recycle | NVMe reset dead: pass-through sent flag `0`, nvme ioctl demands `FWRITE` -> every reset EBADF, no ZBC fallback (only on ENOTTY) | bug (feature inert on primary target) | verify round | pass `FWRITE` (`422cb631771`) |
| SB recycle | `zst_wp_lba` never advanced on SB append -> 3rd ping-pong sees a full zone as empty, skips reset, write into full zone fails forever after | bug | verify round (3-cycle trace) | track wp on every append (`422cb631771`) |
| data GC round 1 | live set from in-core `zm_imap`, which diverges from the durable checkpoint (unlink/rename zero entries pre-commit; failed commit leaves new entries, never rolled back) -> cleaner could reset zones crash-recovery still needs | blocker (silent loss of committed data) | 2 independent verify agents | re-read durable imap block from disk (`zlfs_read_dinode_at`, no imap lookup); union with in-core set |
| data GC round 2 | buffer cache incoherent with raw writes/resets: after zone recycle + LBA reuse, `bread` served stale pre-reset blocks — could resurrect round-1 data loss via a stale "durable imap" read | blocker | 2 independent verify agents | `B_INVAL` on every ZLFS bread — nothing cached (`16103441ec6`) |
| data GC round 2 | unlinked-but-open file in neither imap after its removal commits; open fd then reads from a resettable zone | bug (cross-file data exposure) | verify round | pass 3: mark blocks of every in-core vnode |
| data GC round 2 | commit gate double-counted a virgin log-head zone (in `freecount` AND `headfree`) -> cleaner skipped, spurious mid-commit ENOSPC | bug | verify round | `freecount` skips `zm_log_zidx` |
| data GC test | churn v1 (300 x 2 MB) touched ~10/126 zones — passed without exercising cleaner or wrap (df: linear growth to 8%, no sawtooth) | test gap | VM run analysis | churn v2: 32 x 2 MB per iteration (~1 zone), 150 iterations (`566048ccae4`) |
| inode allocation | inode numbers never reused: `zm_next_ino` only grows, map is one block (512 entries) -> churn v2 hit ENOSPC in `create` at ~iteration 16 (ino 512 = 4 + 16 x 32); churn v1 passed only because 300 single-file creates stayed at ino 303 | blocker for churning workloads | churn v2 on VM (ZBD#11, 2026-07-12) | `zlfs_ialloc` claims the lowest ino absent from both the map and the in-core list, under `zm_lock`; ENOSPC path disposes the fresh vnode through the dead-inode path |
| GC test tooling | script died silently: `dd 2>/dev/null` swallowed the ENOSPC message and `set -e` aborted with no output after the section header | test-tooling gap | VM run | capture dd stderr and `die` with iteration/file context |
| remount after wrap | `zlfs_log_init` was still linear-log: scanned down for the HIGHEST written zone, and when that zone was the last one and full returned ENOSPC — after a circular wrap the true head (the one written-not-full zone) sits mid-device, so umount+mount of a wrapped filesystem always failed; also a merely-full filesystem refused to mount at all | blocker for any wrapped fs | churn v2 rerun on VM (2026-07-13): runtime PASS, then `mount_zlfs: ... No space left on device` on the final remount check | scan for the single written-not-full zone (room >= 1 block); else first empty zone; else mount with an exhausted head so the first commit's cleaner can free space (writes ENOSPC until then, reads fine) |
