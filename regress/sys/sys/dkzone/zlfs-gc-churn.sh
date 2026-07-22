#!/bin/sh
set -eu

# ZLFS data-zone cleaner churn test.
#
# Exercises the garbage collector and the circular log allocator: each
# iteration writes about one zone's worth of files (32 x 2MB) and
# removes them again, and the default 150 iterations fill more zones
# than the device has (126 data zones on the QEMU ZNS test disk), so
# merely completing the loop proves dead zones were reclaimed and
# reused -- without the cleaner a permanent ENOSPC is unavoidable.  A
# keeper file written before the churn must survive both the churn and
# a remount with its contents intact.
#
# WARNING: this reformats the given disk with newfs_zlfs.

usage()
{
	echo "usage: $0 disk [mount-point [iterations [files-per-iter]]]" >&2
	echo "example: $0 sd1c /mnt/zlfs 150 32" >&2
	echo "warning: this reformats the disk with newfs_zlfs" >&2
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
mnt=${2-/mnt/zlfs}
iters=${3-150}
nfiles=${4-32}

[ -n "$disk" ] && [ $# -le 4 ] || usage

dev=/dev/$disk

[ "$(id -u)" -eq 0 ] || die "must run as root"

section "newfs + mount"
umount "$mnt" 2>/dev/null || true
# ZLFS_NEWFS_ZONES=N clamps the fs to N zones (capacity-class drives).
newfs_zlfs ${ZLFS_NEWFS_ZONES:+-z "$ZLFS_NEWFS_ZONES"} "$disk"
mkdir -p "$mnt"
mount_zlfs "$dev" "$mnt"

section "keeper file (must survive all churn)"
dd if=/dev/random of="$mnt/keeper" bs=4k count=100 2>/dev/null
sync
k0=$(cksum < "$mnt/keeper")

# Each file is ~2MB (the current single-indirect maximum), so nfiles
# of them fill about one 64MB zone per iteration; the dead blocks
# become reclaimable one commit after the removal.
section "churn: $iters x ($nfiles x 2MB write, sync, rm, sync)"
mkdir "$mnt/churn"
i=0
while [ "$i" -lt "$iters" ]; do
	j=0
	while [ "$j" -lt "$nfiles" ]; do
		out=$(dd if=/dev/zero of="$mnt/churn/f$j" bs=4k count=500 \
		    2>&1) ||
		    die "write failed at iteration $i file f$j: $out"
		j=$((j + 1))
	done
	sync
	rm "$mnt"/churn/f* ||
	    die "rm failed at iteration $i"
	sync
	i=$((i + 1))
	[ $((i % 25)) -eq 0 ] &&
	    echo "  ...$i iterations, df: $(df -k "$mnt" | tail -1)"
done
echo "  ok: $iters churn iterations, no ENOSPC"

section "keeper intact after churn?"
k1=$(cksum < "$mnt/keeper")
[ "$k0" = "$k1" ] || die "FAIL: keeper corrupted by churn"
echo "  ok: keeper unchanged"

section "keeper intact after remount?"
umount "$mnt"
mount_zlfs "$dev" "$mnt"
k2=$(cksum < "$mnt/keeper")
[ "$k0" = "$k2" ] || die "FAIL: keeper lost after remount"
echo "  ok: keeper survives remount"

section "PASS"
