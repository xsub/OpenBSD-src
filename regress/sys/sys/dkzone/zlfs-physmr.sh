#!/bin/sh
set -eu
# A failing dkzone/newfs in a pipeline must abort, not be masked by a
# later tee/sed exit status.
(set -o pipefail) 2>/dev/null && set -o pipefail

# ZLFS physical-SMR first-contact smoke test.
#
# The first thing to run when a real host-managed SMR drive (e.g. a
# WDC 26 TB behind a SAS HBA, whose SATL presents SCSI ZBC) is attached:
# read-only checks first, escalating to a small clamped filesystem.
#
#   1. zone report sanity: uniform size, sequential-required type,
#      sane write pointers -- printed so the human can eyeball the
#      geometry (zone size, capacity, count) before anything writes.
#   2. the sd(4) write gate: a non-write-pointer write must be
#      rejected; this proves the zoned safety layer engaged.
#   3. newfs_zlfs -z N: a small filesystem in the first N zones only.
#   4. mount, basic namespace + data round trip, fsync, remount.
#   5. a wrap-sized churn inside the clamp: forces zone switching,
#      the cleaner and at least one reclaim on REAL hardware.
#
# Everything after step 2 destroys data in the first N zones ONLY;
# the rest of the drive is never touched.
#
# usage: zlfs-physmr.sh disk [zones [mount-point]]
#   e.g.: zlfs-physmr.sh sd2c 32 /mnt/zlfs
# At 256 MiB zones, 32 zones = 8 GB -- the same scale every QEMU
# suite runs at, so the full regress set is meaningful afterwards:
#   ZLFS_NEWFS_ZONES=32 sh zlfs-triple.sh sd2c   (etc.)

usage()
{
	echo "usage: $0 disk [zones [mount-point]]" >&2
	echo "warning: this reformats the first N zones with newfs_zlfs" >&2
	exit 1
}

die()
{
	echo "$0: $*" >&2
	exit 1
}

section()
{
	echo
	echo "== $* =="
}

disk=${1-}
zones=${2-32}
mnt=${3-/mnt/zlfs}

[ -n "$disk" ] && [ $# -le 3 ] || usage
[ "$(id -u)" -eq 0 ] || die "must run as root"

dev=/dev/$disk
rdev=/dev/r$disk
tdir=/tmp/zlfs-phys.$$
mkdir -p "$tdir"
trap 'rm -rf "$tdir"' EXIT

# The dkzone report tool ships next to this script (columns:
# idx start_lba length_lba capacity_lba wp_lba type condition ...).
dkdir=$(cd "$(dirname "$0")" && pwd)
if [ ! -x "$dkdir/obj/dkzone" ]; then
	(cd "$dkdir" && make obj >/dev/null && make >/dev/null) || \
	    die "cannot build dkzone"
fi
dkzone=$dkdir/obj/dkzone

section "zone report: first zones + geometry sanity (read-only)"
"$dkzone" -r all -n 8 -s 0 "$rdev" | tee "$tdir/rep" || \
    die "zone report failed"
nsz=$(awk '$1 ~ /^[0-9]+$/ { print $3 }' "$tdir/rep" | sort -u | wc -l)
[ "$nsz" -eq 1 ] || die "zone sizes not uniform in the sample"
awk '$1 ~ /^[0-9]+$/ && $6 !~ /seq/ { exit 1 }' "$tdir/rep" || \
    die "non-sequential zone in the sample (conventional zones?)"
echo "  ok: uniform sequential zones (eyeball size/capacity above;"
echo "      the clamp below only ever touches the first $zones zones)"

section "write gate engaged? (a non-wp write must be rejected)"
# Use dkzone's own write probe: it opens the raw device, derives the
# sector size itself, writes at the given LBA, and reports "ok" ONLY
# when the write is refused with EROFS/EINVAL -- the sd(4) host-managed
# gate (sd_zoned_write_prepare) rejecting a non-write-pointer write.
# A raw dd here would (a) hardcode 512-byte sectors, (b) count ANY
# failure as success -- including the drive's own EIO if the gate were
# bypassed -- and (c) leave a stray write behind.  We target zone 2's
# start with zone 0 primed in the report cache, so the LBA can never
# equal the cached write pointer.
zstart=$(awk '$1 == "2" { print $2; exit }' "$tdir/rep")
[ -n "$zstart" ] || die "cannot parse zone 2 start from the report"
if ! "$dkzone" -w -s "$zstart" "$rdev" 2>"$tdir/gate"; then
	cat "$tdir/gate" >&2
	die "write gate did NOT reject a non-wp write (gate inactive?)"
fi
echo "  ok: non-wp write rejected by the sd(4) write gate"

section "newfs_zlfs -z $zones (first $zones zones only)"
umount "$mnt" 2>/dev/null || true
# Capture the geometry newfs actually used; if the device has fewer
# than $zones zones newfs occupies all of them, so trust its count,
# not our argument, for the churn sizing below.
newfs_zlfs -z "$zones" "$disk" > "$tdir/newfs" || die "newfs failed"
cat "$tdir/newfs"
nzones=$(sed -n 's/^[^ ]*: \([0-9]*\) zones of .*/\1/p' "$tdir/newfs")
zlbas=$(sed -n 's/.* zones of \([0-9]*\) LBAs .*/\1/p' "$tdir/newfs")
secsz=$(sed -n 's/.*(\([0-9]*\)-byte sectors).*/\1/p' "$tdir/newfs")
zcap=$(sed -n 's/.*capacity \([0-9]*\) LBAs.*/\1/p' "$tdir/newfs")
for v in "$nzones" "$zlbas" "$secsz" "$zcap"; do
	case "$v" in ''|*[!0-9]*) die "cannot parse newfs geometry";; esac
done
[ "$nzones" -le "$zones" ] || die "newfs used more zones than requested?"
mkdir -p "$mnt"
mount_zlfs "$dev" "$mnt"

section "basic round trip + fsync + remount"
dd if=/dev/random of="$tdir/t" bs=4k count=1024 2>/dev/null
cp "$tdir/t" "$mnt/f"
mkdir -p "$mnt/d/e"
cp "$tdir/t" "$mnt/d/e/g"
sync
cmp "$tdir/t" "$mnt/f" || die "f differs before remount"
umount "$mnt"
mount_zlfs "$dev" "$mnt"
cmp "$tdir/t" "$mnt/f" || die "f differs after remount"
cmp "$tdir/t" "$mnt/d/e/g" || die "d/e/g differs after remount"
kcsum=$(cksum < "$mnt/f")
echo "  ok: data + namespace survive a remount on real hardware"

section "wrap churn inside the clamp (zone switch + cleaner + reclaim)"
# Write ~1.5x the ACTUAL clamped data capacity in ~32 MB junk files so
# the log must switch zones, wrap, and the cleaner must reclaim on the
# way.  Capacity is (data zones) x (per-zone capacity) x sector size,
# all from newfs's own geometry -- correct on 512e and 4Kn alike.
dzones=$((nzones - 2))				# minus the two SB zones
[ "$dzones" -ge 1 ] || die "no data zones after the clamp"
total=$((zcap * secsz * dzones * 3 / 2))
iters=$((total / (32 * 1024 * 1024)))
[ "$iters" -gt 0 ] || iters=8
echo "  ($iters x 32 MB junk cycles for ~$((total / 1048576)) MB)"
i=0
while [ "$i" -lt "$iters" ]; do
	if ! dd if=/dev/zero of="$mnt/junk" bs=4k count=8000 2>"$tdir/err"; then
		cat "$tdir/err" >&2
		die "churn write failed at iteration $i"
	fi
	sync
	rm "$mnt/junk"
	sync
	i=$((i + 1))
done
df -k "$mnt" | tail -1
[ "$(cksum < "$mnt/f")" = "$kcsum" ] || die "f eaten during wrap churn"
echo "  ok: wrapped the clamped log ~1.5x, keeper intact"

section "final remount"
umount "$mnt"
mount_zlfs "$dev" "$mnt"
[ "$(cksum < "$mnt/f")" = "$kcsum" ] || die "f lost at the end"
umount "$mnt"
echo "  ok"

section "PASS"
echo "next: ZLFS_NEWFS_ZONES=$zones sh $(dirname "$0")/zlfs-triple.sh $disk"
