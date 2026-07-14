#!/bin/sh
set -eu

# ZLFS superblock-zone recycling test.
#
# A superblock zone normally holds ~16k superblocks, so the ping-pong
# reset that recycles the two SB zones would need ~16k commits to
# trigger.  Mounting with -o sbcap=N clamps each SB zone to N
# superblocks, so every N commits the log switches zones, resetting the
# stale one.  With sbcap=4 and 40 syncs the zones recycle ~10 times;
# a marker file rewritten before every sync proves each generation
# lands, and a remount proves discovery still finds the newest
# superblock among recycled zones.
#
# WARNING: this reformats the given disk with newfs_zlfs.

usage()
{
	echo "usage: $0 disk [mount-point [syncs [sbcap]]]" >&2
	echo "example: $0 sd1c /mnt/zlfs 40 4" >&2
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
syncs=${3-40}
sbcap=${4-4}

[ -n "$disk" ] && [ $# -le 4 ] || usage
[ "$(id -u)" -eq 0 ] || die "must run as root"

dev=/dev/$disk

section "newfs + mount -o sbcap=$sbcap"
umount "$mnt" 2>/dev/null || true
newfs_zlfs "$disk"
mkdir -p "$mnt"
mount_zlfs -o "sbcap=$sbcap" "$dev" "$mnt"

section "$syncs commits at $sbcap superblocks per zone (~$((syncs / sbcap)) recycles)"
i=1
while [ "$i" -le "$syncs" ]; do
	echo "gen-$i" > "$mnt/marker"
	sync
	[ "$(cat "$mnt/marker")" = "gen-$i" ] ||
	    die "marker wrong after sync $i"
	i=$((i + 1))
done
echo "  ok: $syncs generations committed across recycled SB zones"

section "newest generation found after remount?"
umount "$mnt"
mount_zlfs "$dev" "$mnt"
[ "$(cat "$mnt/marker")" = "gen-$syncs" ] ||
    die "marker lost or stale after remount: $(cat "$mnt/marker")"
echo "  ok: discovery found gen-$syncs among recycled zones"

section "recycling continues after an unclamped remount + more commits"
echo "extra" > "$mnt/marker2"
sync
umount "$mnt"
mount_zlfs -o "sbcap=$sbcap" "$dev" "$mnt"
i=1
while [ "$i" -le "$sbcap" ]; do
	echo "post-$i" > "$mnt/marker2"
	sync
	i=$((i + 1))
done
[ "$(cat "$mnt/marker2")" = "post-$sbcap" ] || die "marker2 wrong"
umount "$mnt"
mount_zlfs "$dev" "$mnt"
[ "$(cat "$mnt/marker2")" = "post-$sbcap" ] || die "marker2 lost after remount"
[ "$(cat "$mnt/marker")" = "gen-$syncs" ] || die "marker regressed"
echo "  ok: post-remount commits and recycling intact"

section "PASS"
