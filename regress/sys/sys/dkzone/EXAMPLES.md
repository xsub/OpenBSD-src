# dkzone Examples

This file captures representative `dkzone` output from the OpenBSD/arm64 VM
used for zoned block bring-up.  It is not a golden test transcript; compiler
command lines and device numbers may vary.  The important part is the semantic
shape of the output and the final `ok`.

## Canonical QEMU NVMe ZNS VM Smoke

Environment:

- OpenBSD/arm64 VM
- QEMU 11 NVMe Zoned Namespace attached as `/dev/rsd1c`
- 8 GB raw namespace
- 64 MB zones
- `zone_size_lba=131072`
- 128 reported zones
- OpenBSD 7.9-current-ZBD-dev kernel after the raw sequential write milestone

Run:

```sh
cd /home/src/OpenBSD-src
git pull --ff-only
cd regress/sys/sys/dkzone
./dkzone-build.sh
./dkzone-vm-smoke.sh /dev/rsd1c 0
```

Expected high-level coverage in the captured run below:

- build and basic regress run
- single-page zone report
- header-only `-n 0` zone report
- paginated report enumeration
- protocol-dependent unsupported report filter returning `EOPNOTSUPP`
- report filters for `empty` and `full`
- finish/reset zone management
- one-sector sequential raw write at the reported write pointer
- report verification that the write pointer advanced by one LBA
- stale/non-WP host-managed data write rejection with `EINVAL` or `EROFS`

Example output:

```text
OpenBSD 7.9-current-ZBD-dev (GENERIC.MP) #0
uname: OpenBSD varm.puffyclouds.click 7.9 GENERIC.MP#0-ZBD-dev arm64

== build dkzone ==
==== run-regress-dkzone ====
./dkzone

== single-page report ==
zone_mode=4 (host-managed) flags=0x3e zone_size_lba=131072
max_open=0 max_active=0 optimal_open=0 optimal_nonseq=0 max_seq=0
report=0 (all) same=0 (all-different) start_lba=0 max_lba=16777215 entries_filled=4 entries_available=4
  idx            start_lba           length_lba         capacity_lba               wp_lba type            condition            flags   raw_type   raw_cond  raw_flags
    0                    0               131072               131072                    0 seq-required    empty           0x00000000 0x00000002 0x00000010 0x00000000
    1               131072               131072               131072               131072 seq-required    empty           0x00000000 0x00000002 0x00000010 0x00000000
    2               262144               131072               131072               262144 seq-required    empty           0x00000000 0x00000002 0x00000010 0x00000000
    3               393216               131072               131072               393216 seq-required    empty           0x00000000 0x00000002 0x00000010 0x00000000

== header-only report ==
zone_mode=4 (host-managed) flags=0x3e zone_size_lba=131072
max_open=0 max_active=0 optimal_open=0 optimal_nonseq=0 max_seq=0
report=0 (all) same=0 (all-different) start_lba=0 max_lba=16777215 entries_filled=0 entries_available=0

== paginated report ==
pagination pages=32 zones=128 last_start_lba=16646144 ok

== protocol-dependent report filter ==
dkzone: DIOCGZONEREPORT /dev/rsd1c: Operation not supported
zone_mode=4 (host-managed) flags=0x3e zone_size_lba=131072
max_open=0 max_active=0 optimal_open=0 optimal_nonseq=0 max_seq=0
reset report filter unsupported ok

== report filters ==
== reset zone before report filter probe ==
zone_reset lba=0 flags=0x0 ok
== report empty zones ==
zone_mode=4 (host-managed) flags=0x3e zone_size_lba=131072
max_open=0 max_active=0 optimal_open=0 optimal_nonseq=0 max_seq=0
report=1 (empty) same=0 (all-different) start_lba=0 max_lba=16777215 entries_filled=4 entries_available=4
  idx            start_lba           length_lba         capacity_lba               wp_lba type            condition            flags   raw_type   raw_cond  raw_flags
    0                    0               131072               131072                    0 seq-required    empty           0x00000000 0x00000002 0x00000010 0x00000000
    1               131072               131072               131072               131072 seq-required    empty           0x00000000 0x00000002 0x00000010 0x00000000
    2               262144               131072               131072               262144 seq-required    empty           0x00000000 0x00000002 0x00000010 0x00000000
    3               393216               131072               131072               393216 seq-required    empty           0x00000000 0x00000002 0x00000010 0x00000000
== finish zone ==
zone_finish lba=0 flags=0x0 ok
== report full zones ==
zone_mode=4 (host-managed) flags=0x3e zone_size_lba=131072
max_open=0 max_active=0 optimal_open=0 optimal_nonseq=0 max_seq=0
report=5 (full) same=0 (all-different) start_lba=0 max_lba=16777215 entries_filled=1 entries_available=1
  idx            start_lba           length_lba         capacity_lba               wp_lba type            condition            flags   raw_type   raw_cond  raw_flags
    0                    0               131072               131072               131072 seq-required    full            0x00000000 0x00000002 0x000000e0 0x00000000
== reset zone after report filter probe ==
zone_reset lba=0 flags=0x0 ok
ok

== zone management ==
== initial report ==
zone_mode=4 (host-managed) flags=0x3e zone_size_lba=131072
max_open=0 max_active=0 optimal_open=0 optimal_nonseq=0 max_seq=0
report=0 (all) same=0 (all-different) start_lba=0 max_lba=16777215 entries_filled=4 entries_available=4
  idx            start_lba           length_lba         capacity_lba               wp_lba type            condition            flags   raw_type   raw_cond  raw_flags
    0                    0               131072               131072                    0 seq-required    empty           0x00000000 0x00000002 0x00000010 0x00000000
    1               131072               131072               131072               131072 seq-required    empty           0x00000000 0x00000002 0x00000010 0x00000000
    2               262144               131072               131072               262144 seq-required    empty           0x00000000 0x00000002 0x00000010 0x00000000
    3               393216               131072               131072               393216 seq-required    empty           0x00000000 0x00000002 0x00000010 0x00000000
== finish zone ==
zone_finish lba=0 flags=0x0 ok
zone_mode=4 (host-managed) flags=0x3e zone_size_lba=131072
max_open=0 max_active=0 optimal_open=0 optimal_nonseq=0 max_seq=0
report=0 (all) same=0 (all-different) start_lba=0 max_lba=16777215 entries_filled=4 entries_available=4
  idx            start_lba           length_lba         capacity_lba               wp_lba type            condition            flags   raw_type   raw_cond  raw_flags
    0                    0               131072               131072               131072 seq-required    full            0x00000000 0x00000002 0x000000e0 0x00000000
    1               131072               131072               131072               131072 seq-required    empty           0x00000000 0x00000002 0x00000010 0x00000000
    2               262144               131072               131072               262144 seq-required    empty           0x00000000 0x00000002 0x00000010 0x00000000
    3               393216               131072               131072               393216 seq-required    empty           0x00000000 0x00000002 0x00000010 0x00000000
== reset zone ==
zone_reset lba=0 flags=0x0 ok
zone_mode=4 (host-managed) flags=0x3e zone_size_lba=131072
max_open=0 max_active=0 optimal_open=0 optimal_nonseq=0 max_seq=0
report=0 (all) same=0 (all-different) start_lba=0 max_lba=16777215 entries_filled=4 entries_available=4
  idx            start_lba           length_lba         capacity_lba               wp_lba type            condition            flags   raw_type   raw_cond  raw_flags
    0                    0               131072               131072                    0 seq-required    empty           0x00000000 0x00000002 0x00000010 0x00000000
    1               131072               131072               131072               131072 seq-required    empty           0x00000000 0x00000002 0x00000010 0x00000000
    2               262144               131072               131072               262144 seq-required    empty           0x00000000 0x00000002 0x00000010 0x00000000
    3               393216               131072               131072               393216 seq-required    empty           0x00000000 0x00000002 0x00000010 0x00000000
ok

== sequential raw write ==
== reset zone before sequential write probe ==
zone_reset lba=0 flags=0x0 ok
== report reset write pointer ==
report_one start_lba=0 wp_lba=0 condition=empty
== write one sector at write pointer ==
sequential_write lba=0 bytes=512 sectors=1 ok
== report advanced write pointer ==
report_one start_lba=0 wp_lba=1 condition=implicit-open
== reject stale write below write pointer ==
ordinary_write lba=0 error=EINVAL ok
== reset zone after sequential write probe ==
zone_reset lba=0 flags=0x0 ok
ok

== write policy ==
== reset zone before write-policy probe ==
zone_reset lba=0 flags=0x0 ok
== expect stale/non-WP ordinary write to fail ==
ordinary_write lba=1 error=EROFS ok
== reset zone after write-policy probe ==
zone_reset lba=0 flags=0x0 ok
ok

ok
```
