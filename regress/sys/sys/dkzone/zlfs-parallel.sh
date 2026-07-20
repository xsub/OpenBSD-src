#!/bin/sh
set -eu

# ZLFS concurrent-writers stress test.
#
# Runs several writers in parallel, each repeatedly rewriting and
# syncing its OWN file (mixed sizes spanning direct, single- and
# double-indirect ranges; two also splice mid-file, exercising the
# read-modify-write overlay), while a background loop churns junk to
# force garbage collection and zone compaction at the same time.  The
# concurrent sync(2)s contend in the commit's per-inode trylock/retry
# path and the compactor runs from the sync path under live load.
#
# Each writer's file is compared byte-for-byte against a fixed template
# after the run and again after a remount; a keeper file written up
# front guards against the cleaner eating live data under contention.
#
# Sized for the ~8 GB QEMU ZNS test device; on a much smaller disk the
# concurrent churn can win the space race and a writer may see a
# transient ENOSPC before GC catches up.
#
# WARNING: this reformats the given disk with newfs_zlfs.

usage()
{
	echo "usage: $0 disk [mount-point [writers [iters]]]" >&2
	echo "example: $0 sd1c /mnt/zlfs 8 40" >&2
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
writers=${3-8}
iters=${4-40}

[ -n "$disk" ] && [ $# -le 4 ] || usage
[ "$(id -u)" -eq 0 ] || die "must run as root"

dev=/dev/$disk
tdir=/tmp/zlfs-par.$$
mkdir -p "$tdir"
trap 'rm -rf "$tdir"' EXIT

# Per-writer template block counts (4 KB blocks): a spread across the
# direct (<12), single-indirect (<524) and double-indirect ranges, so
# the concurrent commits build and rewrite indirect trees too.
sizes="4 20 200 600 1500 40 300 4096"

section "newfs + mount ($writers writers, $iters iters each)"
umount "$mnt" 2>/dev/null || true
newfs_zlfs "$disk"
mkdir -p "$mnt"
mount_zlfs "$dev" "$mnt"

section "keeper file (must survive the whole run)"
dd if=/dev/random of="$mnt/keeper" bs=4k count=100 2>/dev/null
sync
kcsum=$(cksum < "$mnt/keeper")

section "build $writers templates"
w=0
while [ "$w" -lt "$writers" ]; do
	cnt=$(echo "$sizes" | cut -d' ' -f$((w + 1)))
	[ -n "$cnt" ] || cnt=64
	dd if=/dev/random of="$tdir/t$w" bs=4k count="$cnt" 2>/dev/null
	w=$((w + 1))
done

# A background junk churn to keep GC and compaction busy under load.
# Create the run flag BEFORE forking so the child cannot test it first
# and exit immediately (which would silently drop the GC pressure).
mkdir "$mnt/churn"
: > "$tdir/churn_on"
(
	while [ -f "$tdir/churn_on" ]; do
		dd if=/dev/zero of="$mnt/churn/j" bs=4k count=12000 \
		    2>/dev/null || true
		sync 2>/dev/null || true
		rm -f "$mnt/churn/j"
		sync 2>/dev/null || true
	done
) &
churn_pid=$!

section "launch $writers concurrent writers"
pids=""
w=0
while [ "$w" -lt "$writers" ]; do
	(
		i=0
		while [ "$i" -lt "$iters" ]; do
			cp "$tdir/t$w" "$mnt/f$w"
			# Half the writers splice mid-file (RMW), then
			# restore, so the file always ends == template.
			if [ $((w % 2)) -eq 1 ]; then
				printf 'XYZ' | dd of="$mnt/f$w" bs=1 seek=17 \
				    conv=notrunc 2>/dev/null
				cp "$tdir/t$w" "$mnt/f$w"
			fi
			sync
			i=$((i + 1))
		done
	) &
	pids="$pids $!"
	w=$((w + 1))
done

section "wait for writers"
fail=0
for p in $pids; do
	wait "$p" || fail=1
done
rm -f "$tdir/churn_on"
wait "$churn_pid" 2>/dev/null || true
rm -f "$mnt/churn/j"
rmdir "$mnt/churn" 2>/dev/null || true
sync
[ "$fail" -eq 0 ] || die "a writer exited non-zero"
echo "  ok: all $writers writers finished, no ENOSPC/errors"

section "every file matches its template?"
w=0
while [ "$w" -lt "$writers" ]; do
	cmp "$tdir/t$w" "$mnt/f$w" || die "f$w differs from its template"
	w=$((w + 1))
done
[ "$(cksum < "$mnt/keeper")" = "$kcsum" ] || die "keeper changed under load"
echo "  ok: all $writers files and the keeper intact"

section "everything intact after remount?"
umount "$mnt"
mount_zlfs "$dev" "$mnt"
w=0
while [ "$w" -lt "$writers" ]; do
	cmp "$tdir/t$w" "$mnt/f$w" || die "f$w differs after remount"
	w=$((w + 1))
done
[ "$(cksum < "$mnt/keeper")" = "$kcsum" ] || die "keeper lost after remount"
echo "  ok: all files and the keeper survive remount"

section "PASS"
