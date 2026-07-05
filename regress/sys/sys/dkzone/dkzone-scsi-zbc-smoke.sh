#!/bin/sh
set -eu

usage()
{
	echo "usage: $0 raw-device [start-lba]" >&2
	echo "example: $0 /dev/rsd2c 0" >&2
	echo "warning: this runs mutating zoned-device smoke tests" >&2
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

case $# in
1)
	dev=$1
	start_lba=0
	;;
2)
	dev=$1
	start_lba=$2
	;;
*)
	usage
	;;
esac

case "$dev" in
/dev/r*)
	;;
*)
	die "device must be an explicit raw /dev/r* path"
	;;
esac

case "$start_lba" in
''|*[!0-9]*)
	die "start-lba must be a non-negative decimal integer"
	;;
esac

cd "$(dirname "$0")"

tmp=${TMPDIR:-/tmp}/dkzone-scsi-zbc-smoke.$$
cleanup()
{
	rm -f "$tmp"
}
trap cleanup 0 1 2 3 15

section "target evidence"
uname -a || true
sysctl hw.disknames || true
dmesg | grep -E '^(scsibus|sd[0-9]|ses[0-9]|umass|uhub|mpi|mfi|mpt|mfii|ahci|nvme|vscsi|softraid)' || true

section "build dkzone"
./dkzone-build.sh

dkzone=./obj/dkzone
[ -x "$dkzone" ] || die "missing built dkzone helper"

section "host-managed zoned preflight"
"$dkzone" -n 0 -s "$start_lba" "$dev" >"$tmp"
cat "$tmp"
if ! grep -q '^zone_mode=4 ' "$tmp"; then
	die "target is not reported as host-managed zone_mode=4"
fi

section "canonical transport-neutral smoke"
./dkzone-vm-smoke.sh "$dev" "$start_lba"
