#!/bin/sh
set -eu

# ZLFS per-block dirty tracking test.
#
# Mirrors every operation on a template file on the local filesystem
# and uses cmp(1) as the oracle: a 1 MB random file (indirect blocks),
# a 10-byte splice into the middle (read-modify-write of one block; the
# commit must rewrite only that block and keep every other LBA), an
# append, and a shrink-then-grow truncate whose reappearing range must
# read back as zeroes.  Everything is compared again after a remount.
#
# WARNING: this reformats the given disk with newfs_zlfs.

usage()
{
	echo "usage: $0 disk [mount-point]" >&2
	echo "example: $0 sd1c /mnt/zlfs" >&2
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
tmpl=/tmp/zlfs-partial-tmpl

section "newfs + mount"
umount "$mnt" 2>/dev/null || true
# ZLFS_NEWFS_ZONES=N clamps the fs to N zones (capacity-class drives).
newfs_zlfs ${ZLFS_NEWFS_ZONES:+-z "$ZLFS_NEWFS_ZONES"} "$disk"
mkdir -p "$mnt"
mount_zlfs "$dev" "$mnt"

section "1 MB random file (250 blocks, uses the indirect block)"
dd if=/dev/random of="$tmpl" bs=4k count=250 2>/dev/null
cp "$tmpl" "$mnt/f"
sync
cmp "$tmpl" "$mnt/f" || die "initial contents differ"
echo "  ok: file written and committed"

section "10-byte splice mid-file (RMW of one block)"
printf 'SPLICE-42!' | dd of="$mnt/f" bs=1 seek=409723 conv=notrunc 2>/dev/null
printf 'SPLICE-42!' | dd of="$tmpl" bs=1 seek=409723 conv=notrunc 2>/dev/null
sync
cmp "$tmpl" "$mnt/f" || die "splice differs"
echo "  ok: splice committed, rest of file intact"

section "append across the old EOF block"
dd if=/dev/random of=/tmp/zlfs-partial-app bs=1k count=9 2>/dev/null
cat /tmp/zlfs-partial-app >> "$mnt/f"
cat /tmp/zlfs-partial-app >> "$tmpl"
sync
cmp "$tmpl" "$mnt/f" || die "append differs"
echo "  ok: append committed"

section "truncate shrink then grow (reappearing range must be zero)"
# OpenBSD has no truncate(1); perl's truncate is ftruncate(2) directly.
trunc()
{
	perl -e 'truncate $ARGV[0], $ARGV[1] or die "truncate: $!\n"' \
	    "$1" "$2"
}
trunc "$mnt/f" 500000
trunc "$tmpl" 500000
trunc "$mnt/f" 700000
trunc "$tmpl" 700000
sync
cmp "$tmpl" "$mnt/f" || die "truncate shrink/grow differs"
echo "  ok: truncated range reads as zeroes"

section "everything intact after remount?"
umount "$mnt"
mount_zlfs "$dev" "$mnt"
cmp "$tmpl" "$mnt/f" || die "contents differ after remount"
echo "  ok: full byte-for-byte match after remount"

rm -f "$tmpl" /tmp/zlfs-partial-app
section "PASS"
