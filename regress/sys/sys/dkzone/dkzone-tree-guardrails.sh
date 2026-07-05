#!/bin/sh
set -eu

cd "$(dirname "$0")/../../../.."

fail=0
kernel_paths="sys/sys/dkio.h sys/sys/dkzone.h sys/sys/mount.h
    sys/scsi/sd.c sys/dev/ic/nvme.c sys/conf/files"

bad_path()
{
	if [ -e "$1" ]; then
		echo "unexpected path present: $1" >&2
		fail=1
	fi
}

bad_grep()
{
	pattern=$1
	shift

	for path in "$@"; do
		[ -e "$path" ] || continue
		if grep -R -n "$pattern" "$path"; then
			echo "unexpected pattern '$pattern' under $path" >&2
			fail=1
		fi
	done
}

need_grep()
{
	pattern=$1
	path=$2

	if ! grep -q "$pattern" "$path"; then
		echo "missing expected pattern '$pattern' in $path" >&2
		fail=1
	fi
}

bad_path sys/zlfs
bad_path sys/sys/zlfs.h
bad_path sys/kern/vfs_conf.c.tmp

bad_grep 'DIOCZONEMANAGE' $kernel_paths
bad_grep 'dk_zone_manage' $kernel_paths
bad_grep 'DK_ZONE_MANAGE_' $kernel_paths
bad_grep 'MOUNT_ZLFS' $kernel_paths
bad_grep 'zlfs_vfsops' $kernel_paths

need_grep 'DIOCZONECMD' sys/sys/dkio.h
need_grep 'struct dk_zone_op' sys/sys/dkzone.h

if [ "$fail" -ne 0 ]; then
	echo "ZBD tree guardrails failed" >&2
	exit 1
fi

echo "ZBD tree guardrails ok"
