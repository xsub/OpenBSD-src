#!/bin/sh
set -eu

# ZLFS double-indirect test: a 16 MB file (4096 blocks -- direct +
# single indirect + 7 L2 blocks of the double-indirect tree), a splice
# at 10 MB (RMW of one double-range block; only that block, its L2, L1
# and the inode may be rewritten), verified with cmp against an FFS
# template, again after a remount.
#
# WARNING: this reformats the given disk with newfs_zlfs.

usage()
{
	echo "usage: $0 disk [mount-point]" >&2
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

[ -n "$disk" ] && [ $# -le 2 ] || usage
[ "$(id -u)" -eq 0 ] || die "must run as root"

dev=/dev/$disk
tmpl=/tmp/zlfs-big-tmpl

section "newfs + mount"
umount "$mnt" 2>/dev/null || true
newfs_zlfs "$disk"
mkdir -p "$mnt"
mount_zlfs "$dev" "$mnt"

section "16 MB random file (4096 blocks, double indirect)"
dd if=/dev/random of="$tmpl" bs=64k count=256 2>/dev/null
cp "$tmpl" "$mnt/big"
sync
cmp "$tmpl" "$mnt/big" || die "initial contents differ"
echo "  ok: 16 MB written and committed"

section "splice at 10 MB (RMW deep in the double-indirect range)"
printf 'DOUBLE-IND' | dd of="$mnt/big" bs=1 seek=10485770 conv=notrunc 2>/dev/null
printf 'DOUBLE-IND' | dd of="$tmpl" bs=1 seek=10485770 conv=notrunc 2>/dev/null
sync
cmp "$tmpl" "$mnt/big" || die "splice differs"
echo "  ok: splice committed, rest intact"

section "intact after remount?"
umount "$mnt"
mount_zlfs "$dev" "$mnt"
cmp "$tmpl" "$mnt/big" || die "contents differ after remount"
echo "  ok: byte-for-byte match after remount"

rm -f "$tmpl"
section "PASS"
