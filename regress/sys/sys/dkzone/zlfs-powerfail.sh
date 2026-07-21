#!/bin/sh
set -eu

# ZLFS power-fail / crash-recovery test.
#
# Uses the -o faultpoint=N mount option to simulate a power cut at each
# interesting point of the commit pipeline (the mount goes "dead": no
# further write reaches the device, exactly like a machine that lost
# power), then verifies that a clean remount recovers the expected
# state -- the previous checkpoint for a crash before the superblock
# append, the new one for a crash after it.  Two final sections inject
# real garbage at the superblock zone's write pointer from userland
# (a full-block and a HALF-block torn append) and verify discovery
# skips it and later commits stay visible.
#
#   faultpoint=1  crash at commit start (nothing written)
#   faultpoint=2  segment written, checkpoint dropped
#   faultpoint=3  checkpoint written, superblock dropped
#   faultpoint=4  full commit, then dead (post-commit writes lost)
#
# LIMITATION: the faultpoint model stops the HOST from writing; it
# cannot simulate the DEVICE losing its volatile write cache at the
# cut.  The post-superblock flush in zlfs_commit exists for exactly
# that case and is exercised here only in the sense that every stage-4
# "committed" assertion depends on it having run; proving it needs a
# power cut on real cache-backed hardware.
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
tdir=/tmp/zlfs-pf.$$
mkdir -p "$tdir"
trap 'rm -rf "$tdir"' EXIT

# The dkzone helper reports the superblock zone's write pointer for the
# torn-append sections.
dkdir=$(cd "$(dirname "$0")" && pwd)
if [ ! -x "$dkdir/obj/dkzone" ]; then
	(cd "$dkdir" && make obj >/dev/null && make >/dev/null)
fi
dkzone=$dkdir/obj/dkzone

# Write pointer (in 512-byte sectors) of superblock zone 0.
sb_wp()
{
	"$dkzone" -r all -n 1 -s 0 "$rdev" | awk '$1 == "0" { print $5; exit }'
}

section "newfs + baseline state"
umount "$mnt" 2>/dev/null || true
newfs_zlfs "$disk"
mkdir -p "$mnt"
mount_zlfs "$dev" "$mnt"
dd if=/dev/random of="$tdir/ta" bs=4k count=300 2>/dev/null
cp "$tdir/ta" "$mnt/fa"
sync
acs=$(cksum < "$mnt/fa")
umount "$mnt"
mount_zlfs "$dev" "$mnt"
[ "$(cksum < "$mnt/fa")" = "$acs" ] || die "baseline fa does not persist"
umount "$mnt"
echo "  ok: baseline file committed and persistent"

section "faultpoint=1: crash at commit start"
mount_zlfs -o faultpoint=1 "$dev" "$mnt"
dd if=/dev/random of="$mnt/fb" bs=4k count=50 2>/dev/null
sync || true
umount "$mnt"
mount_zlfs "$dev" "$mnt"
[ ! -e "$mnt/fb" ] || die "fb survived a crash before any write"
[ "$(cksum < "$mnt/fa")" = "$acs" ] || die "fa damaged by faultpoint=1"
umount "$mnt"
echo "  ok: nothing from the doomed mount is visible; fa intact"

section "faultpoint=2: segment written, checkpoint dropped"
mount_zlfs -o faultpoint=2 "$dev" "$mnt"
# Overwrite fa's first block and create a new file: both changes must
# vanish -- the data blocks reached the log but no checkpoint points
# at them.
dd if=/dev/zero of="$mnt/fa" bs=4k count=1 conv=notrunc 2>/dev/null
dd if=/dev/random of="$mnt/fc" bs=4k count=50 2>/dev/null
sync || true
umount "$mnt"
mount_zlfs "$dev" "$mnt"
[ ! -e "$mnt/fc" ] || die "fc survived a mid-segment crash"
[ "$(cksum < "$mnt/fa")" = "$acs" ] || die "fa shows uncommitted overwrite"
# The orphaned segment must not break normal use.
dd if=/dev/random of="$mnt/fd" bs=4k count=50 2>/dev/null
sync
dcs=$(cksum < "$mnt/fd")
umount "$mnt"
mount_zlfs "$dev" "$mnt"
[ "$(cksum < "$mnt/fd")" = "$dcs" ] || die "fd lost after orphan segment"
umount "$mnt"
echo "  ok: old state in force, orphan segment harmless"

section "faultpoint=3: checkpoint written, superblock dropped"
mount_zlfs -o faultpoint=3 "$dev" "$mnt"
dd if=/dev/zero of="$mnt/fa" bs=4k count=1 conv=notrunc 2>/dev/null
dd if=/dev/random of="$mnt/fe" bs=4k count=50 2>/dev/null
sync || true
umount "$mnt"
mount_zlfs "$dev" "$mnt"
[ ! -e "$mnt/fe" ] || die "fe survived a pre-superblock crash"
[ "$(cksum < "$mnt/fa")" = "$acs" ] || die "fa shows dropped-SB overwrite"
umount "$mnt"
echo "  ok: a checkpoint without its superblock changes nothing"

section "faultpoint=4: full commit, then dead"
mount_zlfs -o faultpoint=4 "$dev" "$mnt"
printf 'COMMITTED' | dd of="$mnt/fa" bs=1 seek=100 conv=notrunc 2>/dev/null
sync || true
# The mount is now dead: this write fails with EIO (a dead mount
# rejects writes rather than buffer them forever) and must not exist
# after the remount either way.
dd if=/dev/random of="$mnt/fg" bs=4k count=50 2>/dev/null || true
sync || true
umount "$mnt"
mount_zlfs "$dev" "$mnt"
newacs=$(cksum < "$mnt/fa")
[ "$newacs" != "$acs" ] || die "committed splice missing after crash"
mark=$(dd if="$mnt/fa" bs=1 skip=100 count=9 2>/dev/null | cksum)
want=$(printf 'COMMITTED' | cksum)
[ "$mark" = "$want" ] || die "fa splice content wrong"
[ ! -e "$mnt/fg" ] || die "post-commit write survived the crash"
umount "$mnt"
echo "  ok: superblock append is exactly the commit point"

section "torn full-block superblock append (garbage at the SB wp)"
wp=$(sb_wp)
[ -n "$wp" ] || die "cannot read SB zone write pointer"
dd if=/dev/random of="$rdev" bs=512 seek="$wp" count=8 2>/dev/null
mount_zlfs "$dev" "$mnt"
[ "$(cksum < "$mnt/fa")" = "$newacs" ] || die "fa lost to torn SB block"
[ "$(cksum < "$mnt/fd")" = "$dcs" ] || die "fd lost to torn SB block"
dd if=/dev/random of="$mnt/fh" bs=4k count=50 2>/dev/null
sync
hcs=$(cksum < "$mnt/fh")
umount "$mnt"
mount_zlfs "$dev" "$mnt"
[ "$(cksum < "$mnt/fh")" = "$hcs" ] || die "commit after torn SB invisible"
umount "$mnt"
echo "  ok: discovery skips the garbage generation"

section "torn HALF-block superblock append (unaligned SB wp)"
wp=$(sb_wp)
[ -n "$wp" ] || die "cannot read SB zone write pointer"
dd if=/dev/random of="$rdev" bs=512 seek="$wp" count=4 2>/dev/null
mount_zlfs "$dev" "$mnt"
[ "$(cksum < "$mnt/fh")" = "$hcs" ] || die "fh lost to half-block tear"
dd if=/dev/random of="$mnt/fi" bs=4k count=50 2>/dev/null
sync
ics=$(cksum < "$mnt/fi")
umount "$mnt"
mount_zlfs "$dev" "$mnt"
[ "$(cksum < "$mnt/fi")" = "$ics" ] || \
    die "commit after unaligned wp invisible (torn SB zone not retired)"
[ "$(cksum < "$mnt/fa")" = "$newacs" ] || die "fa lost after unaligned wp"
umount "$mnt"
echo "  ok: torn SB zone retired; the log ping-pongs to the other zone"

section "PASS"
