#!/bin/sh
set -eu

# ZLFS triple-indirect test.
#
# Builds a file well past the double-indirect boundary (~1.026 GB at a
# 4 KB block size), so its tail lives in the triple tree (top -> mid ->
# leaf -> data), then exercises every path that must understand it:
# streaming read (cksum), a mid-file RMW splice deep in the triple
# range, remount (bmap from disk with no overlay), truncate back below
# the boundary (drops the whole tree), regrow into it with junk churn
# running concurrently, and a GC/compaction storm with the file live.
#
# The file is an exact repetition of a 4 MB random template, so every
# expected checksum is computed by streaming the template N times --
# nothing large is ever stored outside the filesystem under test.
#
# Sized for the ~8 GB QEMU ZNS test device.
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

# Stream N copies of a file (for expected-checksum computation).
reps()
{
	rn=$1; rf=$2; ri=0
	while [ "$ri" -lt "$rn" ]; do
		cat "$rf"
		ri=$((ri + 1))
	done
}

# Append N copies of the template to the big file, syncing every 8
# appends so the dirty overlay never holds more than 32 MB.
grow()
{
	gn=$1; gi=0
	while [ "$gi" -lt "$gn" ]; do
		cat "$t" >> "$mnt/big"
		gi=$((gi + 1))
		if [ $((gi % 8)) -eq 0 ]; then
			sync
		fi
	done
	sync
}

disk=${1-}
mnt=${2-/mnt/zlfs}

[ -n "$disk" ] && [ $# -le 2 ] || usage
[ "$(id -u)" -eq 0 ] || die "must run as root"

dev=/dev/$disk
tdir=/tmp/zlfs-tri.$$
churn_pid=
mkdir -p "$tdir"
# Removing $tdir also removes the churn_on flag, so the churn subshell
# (if any) exits by itself; the kill just shortens the race.
trap 'if [ -n "$churn_pid" ]; then kill "$churn_pid" 2>/dev/null || true; fi; rm -rf "$tdir"' EXIT

# Geometry (4 KB blocks, 512 entries per indirect block): the triple
# range starts at (12 + 512 + 512*512) * 4096 = 1075888128 bytes
# (~1.0 GiB); everything past it maps through zi_ib[2].
tsize=4194304			# template: 4 MB = 1024 blocks
boundary=1075888128
n1=296				# build:  296 * 4 MB = 1.24 GB (past boundary)
n2=150				# truncate: 150 * 4 MB = 629 MB (below boundary)
n3=310				# regrow: 310 * 4 MB = 1.30 GB (past it again)

section "newfs + mount"
umount "$mnt" 2>/dev/null || true
newfs_zlfs "$disk"
mkdir -p "$mnt"
mount_zlfs "$dev" "$mnt"

section "keeper file (must survive the whole run)"
dd if=/dev/random of="$mnt/keeper" bs=4k count=100 2>/dev/null
sync
kcsum=$(cksum < "$mnt/keeper")

section "build the 4 MB template + expected checksums"
t=$tdir/t
dd if=/dev/random of="$t" bs=4k count=1024 2>/dev/null
exp1=$(reps "$n1" "$t" | cksum)
exp2=$(reps "$n2" "$t" | cksum)
exp3=$(reps "$n3" "$t" | cksum)

section "grow big to $n1 x 4 MB (crosses into the triple tree)"
grow "$n1"
df -k "$mnt" | tail -1

section "full read through the triple tree matches?"
[ "$(cksum < "$mnt/big")" = "$exp1" ] || die "big differs after build"
echo "  ok: 1.24 GB checksum matches"

section "RMW splice deep in the triple range"
# An unaligned offset ~100 MB past the boundary: block RMW through a
# leaf of the triple tree.
soff=$((boundary + 104857617))
printf 'TRI' | dd of="$mnt/big" bs=1 seek="$soff" conv=notrunc 2>/dev/null
sync
got=$(dd if="$mnt/big" bs=1 skip="$soff" count=3 2>/dev/null | cksum)
want=$(printf 'TRI' | cksum)
[ "$got" = "$want" ] || die "splice readback wrong"
# Restore the original bytes (the template's, at the same phase).
dd if="$t" bs=1 skip=$((soff % tsize)) count=3 2>/dev/null | \
    dd of="$mnt/big" bs=1 seek="$soff" conv=notrunc 2>/dev/null
sync
[ "$(cksum < "$mnt/big")" = "$exp1" ] || die "big differs after restore"
echo "  ok: splice + restore, checksum intact"

section "intact after remount (bmap from disk, no overlay)?"
umount "$mnt"
mount_zlfs "$dev" "$mnt"
[ "$(cksum < "$mnt/big")" = "$exp1" ] || die "big differs after remount"
[ "$(cksum < "$mnt/keeper")" = "$kcsum" ] || die "keeper lost after remount"
echo "  ok: survives remount"

section "truncate below the boundary (drops the whole triple tree)"
perl -e "truncate('$mnt/big', $((n2 * tsize))) or die"
sync
[ "$(cksum < "$mnt/big")" = "$exp2" ] || die "big differs after truncate"
echo "  ok: 629 MB prefix intact, triple tree orphaned"

section "regrow into the triple range with churn running"
# Junk churn interleaves with the regrowth, so the zones written now
# mix live big-file blocks (and triple metadata) with dead junk --
# exactly what the copying cleaner must later untangle.
mkdir -p "$mnt/churn"
: > "$tdir/churn_on"
(
	while [ -f "$tdir/churn_on" ]; do
		dd if=/dev/zero of="$mnt/churn/j" bs=4k count=4000 \
		    2>/dev/null || true
		sync 2>/dev/null || true
		rm -f "$mnt/churn/j"
		sync 2>/dev/null || true
	done
) &
churn_pid=$!
grow $((n3 - n2))
rm -f "$tdir/churn_on"
wait "$churn_pid" 2>/dev/null || true
rm -f "$mnt/churn/j"
rmdir "$mnt/churn" 2>/dev/null || true
sync
[ "$(cksum < "$mnt/big")" = "$exp3" ] || die "big differs after regrow"
echo "  ok: rebuilt triple tree amid churn, checksum matches"

section "GC/compaction storm with the triple file live"
# Far more junk than the free-zone budget: the cleaner and compactor
# must reclaim and relocate around 1.3 GB of live triple-tree state.
i=0
while [ "$i" -lt 60 ]; do
	dd if=/dev/zero of="$mnt/junk" bs=4k count=16000 2>/dev/null
	sync
	rm "$mnt/junk"
	sync
	i=$((i + 1))
done
df -k "$mnt" | tail -1
[ "$(cksum < "$mnt/big")" = "$exp3" ] || die "big eaten by GC"
[ "$(cksum < "$mnt/keeper")" = "$kcsum" ] || die "keeper eaten by GC"
echo "  ok: 60 churn iterations, big + keeper intact"

section "final remount"
umount "$mnt"
mount_zlfs "$dev" "$mnt"
[ "$(cksum < "$mnt/big")" = "$exp3" ] || die "big differs after final remount"
[ "$(cksum < "$mnt/keeper")" = "$kcsum" ] || die "keeper lost at the end"
echo "  ok: everything survives the final remount"

section "PASS"
