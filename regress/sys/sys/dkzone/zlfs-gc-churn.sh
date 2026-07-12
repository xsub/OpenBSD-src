#!/bin/sh
set -eu

# ZLFS data-zone cleaner churn test.
#
# Exercises the garbage collector and the circular log allocator: the
# churn loop writes and removes far more data than one linear pass over
# the data zones can hold.  Without the cleaner the filesystem returns
# a permanent ENOSPC partway through; with it the loop must complete,
# and a keeper file written before the churn must survive both the
# churn and a remount with its contents intact.
#
# WARNING: this reformats the given disk with newfs_zlfs.

usage()
{
	echo "usage: $0 disk [mount-point [iterations]]" >&2
	echo "example: $0 sd1c /mnt/zlfs 300" >&2
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

case $# in
1)
	disk=$1
	mnt=/mnt/zlfs
	iters=300
	;;
2)
	disk=$1
	mnt=$2
	iters=300
	;;
3)
	disk=$1
	mnt=$2
	iters=$3
	;;
*)
	usage
	;;
esac

dev=/dev/$disk

[ "$(id -u)" -eq 0 ] || die "must run as root"

section "newfs + mount"
umount "$mnt" 2>/dev/null || true
newfs_zlfs "$disk"
mkdir -p "$mnt"
mount_zlfs "$dev" "$mnt"

section "keeper file (must survive all churn)"
dd if=/dev/random of="$mnt/keeper" bs=4k count=100 2>/dev/null
sync
k0=$(cksum < "$mnt/keeper")

# Each iteration writes ~2MB (the current single-indirect maximum) and
# removes it after a sync, so the dead blocks become reclaimable one
# commit later.  The default 300 iterations churn far beyond one linear
# pass over the data zones, proving wrap-around and reclaim.
section "churn: $iters x (write 2MB, sync, rm, sync)"
i=0
while [ "$i" -lt "$iters" ]; do
	dd if=/dev/zero of="$mnt/churn" bs=4k count=500 2>/dev/null
	sync
	rm "$mnt/churn"
	sync
	i=$((i + 1))
	[ $((i % 50)) -eq 0 ] &&
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
