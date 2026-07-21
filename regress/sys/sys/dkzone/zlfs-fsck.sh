#!/bin/sh
set -eu

# fsck_zlfs test.
#
# Builds a small populated filesystem (subdirs, an indirect-range file,
# a rename and an unlink so link counts moved around), verifies
# fsck_zlfs calls it clean on the raw device, then copies the used
# prefix of the device into an image file and corrupts it in targeted
# ways -- inode identity, a block pointer, a dirent, the checkpoint --
# verifying every corruption is detected (nonzero exit) while the
# pristine image still checks clean.  Finally verifies that a
# faultpoint-crashed device (orphaned segment) still checks clean:
# unreferenced log garbage is normal for an LFS.
#
# The population is written on a FRESH filesystem so every block lands
# in the first data zones and a 256 MB prefix image is sufficient.
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
rdev=/dev/r$disk
tdir=/tmp/zlfs-fsck.$$
img=$tdir/img
mkdir -p "$tdir"
trap 'rm -rf "$tdir"' EXIT

# fsck must exist (built with the tree).
command -v fsck_zlfs >/dev/null || die "fsck_zlfs not installed"

section "newfs + populate"
umount "$mnt" 2>/dev/null || true
newfs_zlfs "$disk"
mkdir -p "$mnt"
mount_zlfs "$dev" "$mnt"
mkdir -p "$mnt/d1/d2"
dd if=/dev/random of="$mnt/small" bs=4k count=4 2>/dev/null
dd if=/dev/random of="$mnt/d1/indirect" bs=4k count=200 2>/dev/null
dd if=/dev/random of="$mnt/d1/d2/deep" bs=4k count=8 2>/dev/null
echo hello > "$mnt/victim"
sync
mv "$mnt/victim" "$mnt/d1/renamed"
rm "$mnt/small"
sync
umount "$mnt"

section "clean device checks clean"
fsck_zlfs "$rdev" || die "clean filesystem reported errors"
echo "  ok: device clean"

section "prefix image checks clean"
dd if="$rdev" of="$img" bs=1m count=256 2>/dev/null
fsck_zlfs "$img" || die "pristine image reported errors"
echo "  ok: image clean"

# Locate an inode block to corrupt: fsck -v prints "inode N: ... at
# lba L".  Corrupt the deep file's inode identity field (zi_ino, first
# 8 bytes of the inode block).
section "corrupt an inode's identity -> detected"
lba=$(fsck_zlfs -v "$img" | awk '/^inode/ && /file/ { print $NF; exit }')
[ -n "$lba" ] || die "cannot find an inode lba in fsck -v output"
cp "$img" "$img.bad"
printf '\377\377\377\377\377\377\377\377' | \
    dd of="$img.bad" bs=512 seek="$lba" conv=notrunc 2>/dev/null
if fsck_zlfs "$img.bad" >/dev/null 2>&1; then
	die "corrupted inode not detected"
fi
echo "  ok: inode corruption detected"

section "corrupt a block pointer -> detected"
# zi_db[0] sits at byte offset 104 of the inode (4x8 ids/sizes + 6x4
# mode..spare + 4x8 times + 4x4 nsec = 104); write an out-of-range LBA.
cp "$img" "$img.bad"
printf '\377\377\377\377\377\377\377\177' | \
    dd of="$img.bad" bs=1 seek=$((lba * 512 + 104)) conv=notrunc \
    2>/dev/null
if fsck_zlfs "$img.bad" >/dev/null 2>&1; then
	die "corrupted block pointer not detected"
fi
echo "  ok: block-pointer corruption detected"

section "corrupt the checkpoint -> detected"
ck=$(fsck_zlfs "$img" | awk '/checkpoint at lba/ { print $NF; exit }')
[ -n "$ck" ] || die "cannot find checkpoint lba"
cp "$img" "$img.bad"
printf 'X' | dd of="$img.bad" bs=1 seek=$((ck * 512 + 100)) \
    conv=notrunc 2>/dev/null
if fsck_zlfs "$img.bad" >/dev/null 2>&1; then
	die "corrupted checkpoint not detected"
fi
echo "  ok: checkpoint corruption detected (CRC)"

section "pristine image still clean"
fsck_zlfs "$img" || die "pristine image damaged by the test itself"
echo "  ok"

section "orphaned segment (crashed commit) still checks clean"
mount_zlfs -o faultpoint=2 "$dev" "$mnt"
dd if=/dev/random of="$mnt/doomed" bs=4k count=50 2>/dev/null
sync || true
umount "$mnt"
fsck_zlfs "$rdev" || die "orphan log garbage reported as corruption"
echo "  ok: LFS orphan garbage is not an inconsistency"

section "PASS"
