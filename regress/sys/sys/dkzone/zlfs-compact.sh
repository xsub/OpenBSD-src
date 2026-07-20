#!/bin/sh
set -eu

# ZLFS copying-cleaner (compaction) test.
#
# Builds many zones that each mix a small live "pin" file with large
# dead junk, then churns far more junk than the free-zone budget.
# Without compaction the mixed zones can never be reclaimed and the
# filesystem hits ENOSPC; with it, the sync path relocates the pins so
# whole zones go dead and reset.  The pins are checksum-verified after
# the churn and after a remount.
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

section "newfs + mount"
umount "$mnt" 2>/dev/null || true
newfs_zlfs "$disk"
mkdir -p "$mnt"
mount_zlfs "$dev" "$mnt"

# Lay down 40 zones, each = one 256 KB pin (kept) + ~63 MB junk (deleted
# right after), so every zone ends up sparsely live: exactly what only a
# copying cleaner can reclaim.
section "seed 40 mixed zones (pin + junk each)"
mkdir "$mnt/pins"
i=0
while [ "$i" -lt 40 ]; do
	dd if=/dev/random of="$mnt/pins/p$i" bs=4k count=64 2>/dev/null
	dd if=/dev/zero of="$mnt/junk" bs=4k count=16000 2>/dev/null
	sync
	rm "$mnt/junk"
	sync
	i=$((i + 1))
done
sum0=$(cksum "$mnt"/pins/p* | cksum)
echo "  ok: 40 pins laid down amid dead junk"

# Now churn ~6 GB more junk.  The free budget is a handful of zones, so
# completing this requires the pins to be compacted out of their mixed
# zones and those zones reclaimed.
section "churn 100 x 63 MB junk (needs compaction to survive)"
i=0
while [ "$i" -lt 100 ]; do
	dd if=/dev/zero of="$mnt/junk" bs=4k count=16000 2>/dev/null
	sync
	rm "$mnt/junk"
	sync
	i=$((i + 1))
	[ $((i % 25)) -eq 0 ] &&
	    echo "  ...$i, df: $(df -k "$mnt" | tail -1)"
done
echo "  ok: 100 churn iterations, no ENOSPC -- compaction kept up"

section "pins intact after churn?"
sum1=$(cksum "$mnt"/pins/p* | cksum)
[ "$sum0" = "$sum1" ] || die "a pin changed under compaction"
echo "  ok: all 40 pins unchanged"

section "pins intact after remount?"
umount "$mnt"
mount_zlfs "$dev" "$mnt"
sum2=$(cksum "$mnt"/pins/p* | cksum)
[ "$sum0" = "$sum2" ] || die "a pin lost after remount"
echo "  ok: all 40 pins survive remount"

section "PASS"
