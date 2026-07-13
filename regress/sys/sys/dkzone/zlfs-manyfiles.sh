#!/bin/sh
set -eu

# ZLFS multi-block inode map test.
#
# Creates more files than a single-block inode map can hold (512
# entries at a 4096-byte block), so the checkpoint must reference
# multiple map blocks (format v2), and verifies the whole population
# survives a remount.
#
# WARNING: this reformats the given disk with newfs_zlfs.

usage()
{
	echo "usage: $0 disk [mount-point [count]]" >&2
	echo "example: $0 sd1c /mnt/zlfs 700" >&2
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
count=${3-700}

[ -n "$disk" ] && [ $# -le 3 ] || usage
[ "$(id -u)" -eq 0 ] || die "must run as root"

dev=/dev/$disk

section "newfs + mount"
umount "$mnt" 2>/dev/null || true
newfs_zlfs "$disk"
mkdir -p "$mnt"
mount_zlfs "$dev" "$mnt"

section "create $count files (over the old 512-inode ceiling)"
mkdir "$mnt/many"
i=0
while [ "$i" -lt "$count" ]; do
	echo "payload-$i" > "$mnt/many/f$i" ||
	    die "create failed at file $i"
	i=$((i + 1))
done
sync
n=$(ls "$mnt/many" | wc -l | tr -d ' ')
[ "$n" -eq "$count" ] || die "expected $count files, found $n"
echo "  ok: $count files created and committed"

section "population survives remount?"
umount "$mnt"
mount_zlfs "$dev" "$mnt"
n=$(ls "$mnt/many" | wc -l | tr -d ' ')
[ "$n" -eq "$count" ] || die "after remount: expected $count, found $n"
[ "$(cat "$mnt/many/f0")" = "payload-0" ] || die "f0 corrupted"
mid=$((count / 2))
[ "$(cat "$mnt/many/f$mid")" = "payload-$mid" ] || die "f$mid corrupted"
last=$((count - 1))
[ "$(cat "$mnt/many/f$last")" = "payload-$last" ] || die "f$last corrupted"
echo "  ok: $count files and sampled contents intact after remount"

section "cleanup: remove all, rmdir, remount"
rm "$mnt"/many/f*
rmdir "$mnt/many"
sync
umount "$mnt"
mount_zlfs "$dev" "$mnt"
[ ! -d "$mnt/many" ] || die "directory resurrected after remount"
echo "  ok: removal persisted"

section "PASS"
