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

disk_from_rawdev()
{
	disk=${1#/dev/r}
	case "$disk" in
	*[a-p])
		disk=${disk%?}
		;;
	*)
		die "raw device must include an OpenBSD partition letter"
		;;
	esac
	case "$disk" in
	sd[0-9]*)
		echo "$disk"
		;;
	*)
		die "SCSI ZBC smoke expects an sd(4) raw disk, got $1"
		;;
	esac
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
dmesg_tmp=${tmp}.dmesg
cleanup()
{
	rm -f "$tmp" "$dmesg_tmp"
}
trap cleanup 0 1 2 3 15

disk=$(disk_from_rawdev "$dev")

section "target evidence"
uname -a || true
sysctl hw.disknames || true
dmesg >"$dmesg_tmp" || die "could not read dmesg"
grep -E '^(scsibus|sd[0-9]|ses[0-9]|umass|uhub|mpi|mfi|mpt|mfii|ahci|nvme|vscsi|softraid)' "$dmesg_tmp" || true

section "transport preflight"
disk_line=$(grep "^${disk} at " "$dmesg_tmp" || true)
[ -n "$disk_line" ] || die "could not find $disk attachment in dmesg"
echo "$disk_line"
case "$disk_line" in
*'<NVMe,'*)
	die "$dev is an NVMe-backed sd(4) disk; use dkzone-vm-smoke.sh for NVMe ZNS"
	;;
esac
scsibus=$(echo "$disk_line" | sed -n 's/^.* at \(scsibus[0-9][0-9]*\) .*$/\1/p')
[ -n "$scsibus" ] || die "could not identify scsibus for $disk"
scsibus_line=$(grep "^${scsibus} at " "$dmesg_tmp" || true)
[ -n "$scsibus_line" ] || die "could not find $scsibus attachment in dmesg"
echo "$scsibus_line"
case "$scsibus_line" in
*" at nvme"*)
	die "$dev is attached below nvme(4); this is not SCSI ZBC validation"
	;;
esac
echo "non-NVMe sd(4) target evidence ok"

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
