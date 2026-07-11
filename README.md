# OpenBSD Zoned Block Device Experiments

[![OpenBSD](https://img.shields.io/badge/OpenBSD-f2ca30)](https://www.openbsd.org/)
[![ZonedBlockDevice | driver](https://img.shields.io/badge/ZonedBlockDevice%20%7C%20driver-f2ca30)](https://github.com/xsub/OpenBSD-src)
[![ZBD dkzone checks](https://github.com/xsub/OpenBSD-src/actions/workflows/zbd-ci.yml/badge.svg?branch=main)](https://github.com/xsub/OpenBSD-src/actions/workflows/zbd-ci.yml)

Experimental OpenBSD source fork for native zoned block device support, focused on
host-managed SCSI ZBC / SMR disks and NVMe Zoned Namespaces, and building up to
ZLFS -- a small log-structured filesystem that runs directly on those devices.

This is research and bring-up work, not production-ready storage code.  ZLFS is
a mountable read-write filesystem today, with the limitations listed under
"ZLFS Status And Direction" below.

## Goals

- Add a small native OpenBSD ABI for zoned block discovery.
- Support read-only zone capability queries and zone reports first.
- Add explicit zone management commands before ordinary host-managed writes.
- Keep host-managed devices safe while adding tightly bounded sequential write
  experiments.
- Validate behavior with OpenBSD regress tests and QEMU-emulated zoned devices.
- Build ZLFS on top, once raw write-pointer behavior is proven end to end
  (done): a log-structured filesystem whose writes append at zone write
  pointers and commit via a generation-numbered superblock log.

## Current Status

Implemented prototype pieces:

- `sys/sys/dkzone.h`
- `DIOCGZONEINFO`
- `DIOCGZONEREPORT`
- `DIOCZONECMD` for explicit open/close/finish/reset zone management
- `sd(4)` recognition of SCSI ZBC host-managed devices
- SCSI `REPORT ZONES` translation into OpenBSD `struct dk_zone`
- non-WP write rejection for host-managed zoned disks
- regression smoke test under `regress/sys/sys/dkzone`
- initial NVMe ZNS reporting and zone management path
- QEMU/OpenBSD VM validation workflow
- experimental raw sequential write gate for one cached zone descriptor
- ZLFS on-disk format v1 (`sys/sys/zlfs.h`): little-endian, CRC32C
  checksums, a ZNS-compatible superblock generation log ping-ponged
  across zones 0-1 (no conventional zone required), plus checkpoint,
  inode-map, inode, and fixed-size directory-entry structures
- ZLFS registered with the VFS (`option ZLFS`, `vfs_init.c` typenum 20,
  `MOUNT_ZLFS`), mountable read-write
- `newfs_zlfs(8)` creates a real filesystem (superblock log, root
  directory with a sample file, inodes, inode map, checkpoint) using the
  dkzone ioctls and the validated raw sequential write path
- `mount_zlfs(8)` to mount the filesystem
- ZLFS read path (`sys/zlfs/`): superblock-log discovery, checkpoint and
  inode-map load, a per-mount vnode cache, real `lookup`/`readdir`/`read`
  over on-disk inodes, directories, and direct data blocks
- ZLFS log-structured write path: an in-kernel raw zoned-write primitive
  (`dk_zone_write_kern` in `sd(4)`, a direct `WRITE(16)` that bypasses
  the buffer cache and the host-managed write gate), an append-only log
  allocator, and `create`/`write`/`fsync`/truncate with a commit that
  flushes a fresh segment (data, inodes, inode map, checkpoint) and then
  a generation N+1 superblock as the atomic commit point
- minimal `ZBD` kernel config (`sys/arch/arm64/conf/ZBD`) covering only
  the QEMU virt machine for fast development rebuilds

Tested so far:

- ABI regression builds and runs on OpenBSD/arm64.
- Non-zoned disk reports `zone_mode=0 (none)` and skips zone reports.
- Kernel boots on OpenBSD/arm64 VM.
- QEMU NVMe ZNS attaches as an `sd(4)` disk marked `zoned`.
- NVMe ZNS `DIOCGZONEINFO` and `DIOCGZONEREPORT` work in the VM.
- `dkzone-vm-smoke.sh /dev/rsd1c 0` is the canonical QEMU ZNS VM test. It
  covers zone reports, header-only reports, paginated reports, report filters,
  protocol-dependent report filter handling, finish/reset zone management,
  single-sector and multi-sector sequential raw writes at the reported write
  pointer, cached write-pointer continuation across consecutive writes, and
  bad-write rejection.
- Post-hardening validation on the OpenBSD/arm64 VM passed `dkzone-build.sh`,
  `dkzone-write-seq.sh /dev/rsd1c 0 8`,
  `dkzone-write-seq.sh /dev/rsd1c 0 8 2`, and
  `dkzone-vm-smoke.sh /dev/rsd1c 0`.  The focused continuation check verifies
  two consecutive 8-sector raw writes after one fresh zone report and confirms
  that the reported write pointer advances to LBA 16.  The full smoke includes
  single-sector, multi-sector, cached write-pointer continuation, and bad-write
  rejection coverage.  The policy check verifies that writes fail without a
  fresh zone report and fail when they are not at the cached write pointer.
- Tail-boundary validation on the same VM passed
  `dkzone-write-boundary.sh /dev/rsd1c 0`: it filled the first 64 MB zone to
  `wp_lba=131071`, rejected a two-sector write crossing zone capacity with
  `EINVAL`, wrote the final sector, reported `condition=full` at
  `wp_lba=131072`, rejected another write at the full-zone write pointer with
  `EROFS`, and reset the zone.
- SCSI validation checks were run in the same VM: the QEMU NVMe ZNS
  disk is refused by `dkzone-scsi-zbc-smoke.sh` as NVMe-backed `sd(4)`, and
  the normal VirtIO boot disk `/dev/rsd0c` is refused before build/mutation
  because it is not marked `zoned` in `dmesg`.
- `newfs_zlfs sd1c` ran end to end on the QEMU ZNS VM: zone enumeration
  and geometry validation (128 zones of 131072 LBAs), superblock zones
  reset, the write gate armed by a fresh zone report, the generation-0
  superblock written at LBA 0 through the raw sequential write path and
  read back with a passing checksum.  A `hexdump` of the device confirmed
  the on-disk bytes field by field against `sys/sys/zlfs.h` (magic
  `0x54BDCC01`, version, block size, UUID, geometry, root inode,
  little-endian CRC32C in the final struct slot).
- The ZLFS read path was validated on the QEMU ZNS VM: `mount_zlfs`
  mounts the image, `ls -la` lists the root directory and its sample
  file, `cat` returns the file contents, `stat` reports the correct
  size and mode, and repeated directory access no longer hangs.
- The ZLFS log-structured write path was validated end to end on the
  QEMU ZNS VM: after `newfs_zlfs` and `mount_zlfs`,
  `echo ... > /mnt/zlfs/newfile` created and wrote a file, `sync`
  committed it, `cat` returned the contents, and -- critically -- after
  a full `umount` and a fresh `mount_zlfs`, `cat` still returned the
  exact contents.  This exercises the whole cycle: write, commit as a new
  segment, generation N+1 superblock append, remount, superblock-log
  discovery, checkpoint and inode-map load, and read-back.
- The next cross-transport milestone is to run the same `dkzone-vm-smoke.sh`
  flow against a SCSI ZBC or host-managed SMR target.  The
  `dkzone-scsi-zbc-smoke.sh` wrapper prints target evidence, refuses
  NVMe-backed `sd(4)` disks, verifies host-managed `zone_mode=4`, and then
  runs the canonical smoke unchanged so the userland behavior can be compared
  with the validated NVMe ZNS path.

## Test Helper

The regression helper lives at:

```sh
regress/sys/sys/dkzone
```

Run:

```sh
cd /usr/src/regress/sys/sys/dkzone
./dkzone-vm-smoke.sh /dev/rsd1c 0
./dkzone-build.sh
./obj/dkzone /dev/rsd0c
./obj/dkzone -n 64 -s 0 /dev/rsd1c
./obj/dkzone -p -n 4 -s 0 /dev/rsd1c
./obj/dkzone -r empty -n 4 -s 0 /dev/rsd1c
./obj/dkzone -m reset -l 0 /dev/rsd1c
./dkzone-report-filter.sh /dev/rsd1c 0
./dkzone-zone-management.sh /dev/rsd1c 0
./dkzone-write-seq.sh /dev/rsd1c 0
./dkzone-write-seq.sh /dev/rsd1c 0 8
./dkzone-write-seq.sh /dev/rsd1c 0 8 2
./dkzone-write-boundary.sh /dev/rsd1c 0
./dkzone-write-policy.sh /dev/rsd1c 0
./dkzone-scsi-zbc-smoke.sh /dev/rsdXc 0
```

`dkzone-vm-smoke.sh /dev/rsd1c 0` is the canonical QEMU ZNS VM smoke test and
the transport-neutral smoke flow to reuse for SCSI ZBC targets.  It rebuilds
`dkzone`, checks a single report page, verifies the header-only `-n 0` report
edge case, verifies paginated reporting, checks a protocol-dependent report
filter, then runs the report filter, finish/reset zone management, and
single-sector, multi-sector, cached write-pointer continuation, and bad-write
rejection smoke tests.
See `regress/sys/sys/dkzone/EXAMPLES.md` for a captured VM output transcript.

The zone-management helper rebuilds `dkzone` if needed, finishes the selected
zone, verifies that it reports `full`, resets it, and verifies that it reports
`empty` again:

```sh
cd /usr/src/regress/sys/sys/dkzone
./dkzone-zone-management.sh /dev/rsd1c 0
```

`dkzone-zone-management.sh` mutates the target zone, so run it only against an
explicit scratch raw device and pass a zone-start LBA.  `dkzone-zns-smoke.sh`
is kept as a compatibility wrapper for older notes and command history.

`dkzone-report-filter.sh` verifies report filtering by resetting the target
zone, checking that `-r empty` returns it, finishing it, checking that `-r full`
returns it, and resetting it again.

`dkzone-write-policy.sh` verifies the conservative host-managed write policy:
zone management through an `O_RDWR` open must work, while data writes must fail
without a fresh descriptor-bearing zone report, after a header-only zone
report, or when they are not at the current write pointer.  It resets the
selected test zone before and after the probe.

`dkzone-write-seq.sh` verifies the first experimental write primitive through
one `dkzone -S` process: reset the zone, report it to populate the kernel's
cached zone descriptor, write the requested number of sectors at the reported
write pointer, report again and verify that the write pointer advanced by that
sector count, reject a stale write below the new write pointer, then reset the
zone.  The default is one sector; pass a third argument such as `8` to exercise
a multi-sector write.  Pass a fourth argument such as `2` to issue consecutive
writes without an intervening report and verify that the kernel's cached write
pointer advances between writes.
The prototype permits only one in-flight raw zoned write against the cached
descriptor; concurrent report or zone-management operations return `EBUSY`
while that write is pending.

`dkzone-write-boundary.sh` is a heavier focused boundary test.  It resets the
selected zone, fills it until one sector remains, verifies that a two-sector
write crossing the zone capacity is rejected, writes the final sector, verifies
that the zone reports `full`, verifies that another write at the full-zone write
pointer is rejected, and then resets the zone.  The default chunk size is 4096
sectors; pass a third argument to change it.  The helper refuses zones larger
than the built-in tail-test limit so it does not accidentally write a huge
device zone during bring-up.

`dkzone-scsi-zbc-smoke.sh` is the second-transport validation wrapper.  It is
intended for a real SCSI ZBC or host-managed SMR target exposed as an OpenBSD
raw disk.  It prints `uname`, `hw.disknames`, and relevant `dmesg` attachment
lines, refuses NVMe-backed `sd(4)` disks and disks not marked `zoned` in
`dmesg` so they are not counted as second-transport validation, verifies that
the target reports host-managed `zone_mode=4`, and then runs
`dkzone-vm-smoke.sh` unchanged.  A passing run is the evidence that the ABI and
userland behavior are transport-neutral while the kernel handles SCSI ZBC and
NVMe ZNS differences internally.

With a device argument, `dkzone` prints zone capability data and, for zoned
devices, a diagnostic table of reported zone descriptors.  The `-n` option sets
the maximum number of entries requested from the kernel, and `-s` sets the
starting LBA for the report.  The `-p` option pages through reports by
advancing from the last returned zone descriptor.  The `-r` option selects a
report filter such as `all`, `empty`, `closed`, or `full`.
Use `-n 0` for a header-only report.

On a normal non-zoned disk, expected output begins with:

```text
zone_mode=0 (none) flags=0x0 zone_size_lba=0
```

On a zoned device, the helper reports each returned zone with start LBA, length,
capacity, write pointer, translated type, translated condition, translated
flags, and raw protocol values.

The `-m` option issues an explicit zone management command.  Supported commands
are `open`, `close`, `finish`, and `reset`.  Use `-l` for a single zone LBA or
`-a` for the protocol's all-zones form.

The `-S` option runs the full sequential write proof on one open raw device.
The `-W` option writes one logical sector at `-s start-lba` and expects the
write to succeed.  The kernel only allows writes on the raw partition when the
target LBA matches the cached write pointer for the last reported sequential
zone and the transfer fits within that zone.  The lowercase `-w` option probes
the rejection path and expects the write to fail with `EROFS` or `EINVAL`.

## Minimal Development Kernel

`sys/arch/arm64/conf/ZBD` is a minimal kernel config for the QEMU virt VM:
only the devices this VM uses (PL011 uart, ECAM PCIe via ACPI, virtio,
NVMe/ZNS) plus FFS, ZLFS and the debug options, which makes cold builds and
relinks several times faster than GENERIC.MP.  It also carries the arm64
link floor -- SoC uarts, acpiec, com and i2c glue that `machdep.c`,
`acpi.c` and `dsdt.c` reference unconditionally -- documented in the config.

```sh
cd /usr/src/sys/arch/arm64/compile/ZBD
make obj && make config && make clean && make -j4
doas make install && doas reboot
```

Plain `make` suffices after editing `.c` files; re-run
`make config && make clean` only after changing the config file or
`sys/conf/files`.  This kernel boots on the QEMU virt machine only.

## QEMU NVMe ZNS Target

QEMU 11 can expose an emulated NVMe Zoned Namespace. The current development VM
workflow uses an OpenBSD/arm64 guest on Apple Silicon with an optional second
NVMe ZNS test disk.

Example QEMU device shape:

```sh
-drive if=none,format=raw,file=openbsd-zns.raw,id=zns0
-device nvme,id=nvme-zns,serial=ZNS0001
-device nvme-ns,drive=zns0,nsid=1,zoned=on,zoned.zone_size=64M,zoned.zone_capacity=64M
```

Inside OpenBSD, inspect attachment with:

```sh
dmesg | grep -E 'nvme|sd[0-9]'
sysctl hw.disknames
```

## SCSI ZBC Validation Target

The next milestone is to run the same smoke suite on the SCSI ZBC path in
`sd(4)`.  A successful run should use the raw disk device for the SCSI ZBC or
host-managed SMR target:

```sh
cd /usr/src/regress/sys/sys/dkzone
./dkzone-scsi-zbc-smoke.sh /dev/rsdXc 0
```

Replace `rsdXc` with the raw device attached by the SCSI ZBC target.  The
wrapper first records target evidence and rejects NVMe-backed `sd(4)` devices
such as the QEMU ZNS VM disk, because those should keep using
`dkzone-vm-smoke.sh`.  It also rejects ordinary non-zoned `sd(4)` disks before
running mutating tests.  On a non-NVMe `sd(4)` target marked `zoned`, it
confirms host-managed `zone_mode=4`, then runs the same canonical smoke as the
QEMU ZNS path.  Expected coverage is the same as the QEMU ZNS run: report
headers, descriptor translation, pagination, report filters, finish/reset zone
management, sequential raw write at the write pointer, and bad-write
rejection.

The Apple Silicon Homebrew QEMU 11.0.1 build used for the current VM exposes
NVMe ZNS through `nvme-ns,zoned=on`, but its `scsi-hd` device does not advertise
zoned options and `scsi-block` is not available.  On that host, an emulated
SCSI ZBC raw-file target is therefore not a drop-in equivalent to the NVMe ZNS
test disk; use a real ZBC/host-managed SMR device or a different host setup
that can present SCSI ZBC semantics.

## Roadmap

1. Stabilize reporting and management ABI.
2. Keep QEMU NVMe ZNS as the canonical VM regression target.
3. Validate the same smoke flow on SCSI ZBC or host-managed SMR hardware.
4. Compare SCSI ZBC and NVMe ZNS edge cases for report filters, write-pointer
   normalization, and zone management errors.
5. Keep hardening the raw sequential write primitive now that QEMU ZNS proves:
   reset, write at WP, cached WP continuation, report WP advanced, and reject
   stale/non-WP writes.
6. Add richer regress coverage for zone report layouts and option mapping.
7. Design safe write-path semantics for host-managed sequential zones.
8. Validate zone reset/open/close/finish operations on SCSI ZBC and NVMe ZNS.
9. Evaluate filesystem and buffer-cache implications before enabling general
   writable host-managed devices.
10. Grow ZLFS from the validated raw write-pointer contract: the on-disk
    format, `newfs_zlfs`, the in-kernel zone report API, `mount_zlfs`, and
    the read and log-structured write paths are done and validated end to
    end on the VM; next are indirect blocks, subdirectories, `unlink`, a
    cleaner, and durability (see "ZLFS Status And Direction").

## ZLFS Status And Direction

ZLFS is a working, mountable, read-write log-structured filesystem, built
on top of the proven raw write-pointer gate.  The whole write cycle has
been validated end to end on the QEMU ZNS VM: create a file, `sync`, then
`umount` and remount, and the file reads back intact.

On-disk format (`sys/sys/zlfs.h`):

- All multi-byte fields are little-endian and every on-disk structure
  carries a CRC32C checksum.  The superblock is a generation-numbered log
  ping-ponged across zones 0-1 rather than a rewrite-in-place block,
  because NVMe ZNS namespaces have no conventional zones; the header
  documents append rules, mount discovery, the block-size bootstrap, and
  the both-zones-full crash recovery case.
- The checkpoint locates a block-sized inode map (inode number to device
  LBA) and the root inode.  Directories are flat arrays of fixed 128-byte
  entries.  Inodes carry direct block pointers.

Userland:

- `newfs_zlfs(8)` creates a real filesystem: it enumerates zones,
  validates geometry, resets the superblock zones, and writes a root
  directory (with a sample file), the file data, the inodes, the inode
  map, the checkpoint, and the generation-0 superblock, all through the
  gated raw write path.  `newfs_zlfs -N sd1c` is a read-only dry run.
- `mount_zlfs(8)` mounts the filesystem (read-write by default, `-o ro`
  for read-only).

Kernel (`sys/zlfs/`):

- In-kernel zone report API: `dk_zone_report_kern()` (`sys/scsi/sd.c`)
  dispatches to an optional `dev_zone_report` adapter op (NVMe ZNS in
  `sys/dev/ic/nvme.c`) or the native SCSI ZBC path, filling `struct
  dk_zone` arrays into kernel buffers.  The `DIOCGZONEREPORT` ioctl and
  the filesystem share the same report core.
- Mount: superblock-log discovery, checkpoint and inode-map load, and
  per-zone allocator state, all validated against device geometry with
  every on-disk field bounded before use.
- Read: a per-mount vnode cache, `lookup`/`readdir`/`read` over on-disk
  inodes, directories, and direct data blocks.
- Write: a raw zoned-write primitive (`dk_zone_write_kern`, a direct
  `WRITE(16)` bypassing the buffer cache and the host-managed write gate),
  an append-only log allocator, and `create`/`write`/`fsync`/truncate.
  A commit flushes all dirty inodes as a fresh log segment (data, inode
  blocks, a new inode map, a new checkpoint) and then appends a
  generation N+1 superblock, which is the atomic commit point: nothing
  live is overwritten, so a crash before that append leaves the previous
  checkpoint in force and merely orphans the new blocks.

Current limitations (documented in the code; each is a natural next step):

- Direct blocks only (files up to `ZLFS_NDADDR` blocks); no indirect
  blocks yet.
- Single-block directories; a flat root directory only, no subdirectories.
- No `unlink`/`rename` yet.
- No garbage collector: orphaned and superseded blocks are not reclaimed,
  and a filled superblock zone is not reset.
- No cache flush (`SYNCHRONIZE CACHE`/FUA) before the superblock append,
  so power-loss durability is not yet guaranteed against a write-back
  device cache.
- The commit path is not yet safe against concurrent vnode operations.

Remaining sequence toward a general-purpose filesystem:

1. Indirect blocks (larger files) and multi-block directories.
2. `mkdir`/subdirectories, then `unlink`/`rename`.
3. A cleaner/garbage collector and superblock-zone reset.
4. A cache flush before the superblock append for real durability.
5. Concurrency-safe commit and buffer-cache integration.

## Non-Goals For The Current Prototype

- Not production-ready: no garbage collection, limited file and directory
  sizes, and durability caveats above.
- No filesystem-level zoned allocation policy beyond append-at-write-pointer.
- No promise of on-disk format or ABI stability before review.
- No attempt to support drive-managed SMR specially; those already appear as normal disks.

## Safety Model

Host-managed zoned devices are exposed conservatively. The first milestone is
discovery, reporting, and explicit zone management. Ordinary writable support
must preserve sequential-write rules before it can be enabled safely.

## References

- SCSI Zoned Block Commands, ZBC
- ATA Zoned Device ATA Commands, ZAC
- NVMe Zoned Namespaces, ZNS
- Linux zoned block layer
- libzbc
- libzbd
- QEMU NVMe ZNS emulation
