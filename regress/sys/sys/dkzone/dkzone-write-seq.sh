#!/bin/sh
set -eu

usage()
{
	echo "usage: $0 raw-device [start-lba [sectors]]" >&2
	echo "example: $0 /dev/rsd1c 0 8" >&2
	echo "warning: this resets the target zone and writes sectors" >&2
	exit 1
}

die()
{
	echo "$0: $*" >&2
	exit 1
}

case $# in
1)
	dev=$1
	start_lba=0
	sectors=1
	;;
2)
	dev=$1
	start_lba=$2
	sectors=1
	;;
3)
	dev=$1
	start_lba=$2
	sectors=$3
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
case "$sectors" in
''|*[!0-9]*)
	die "sectors must be a positive decimal integer"
	;;
0)
	die "sectors must be greater than zero"
	;;
esac

cd "$(dirname "$0")"

if [ ! -x ./obj/dkzone ]; then
	echo "building dkzone helper"
	make obj >/dev/null
	make >/dev/null
fi

dkzone=./obj/dkzone
cleanup=0

cleanup_zone()
{
	if [ "$cleanup" -eq 1 ]; then
		"$dkzone" -m reset -l "$start_lba" "$dev" >/dev/null 2>&1 ||
		    true
	fi
}

trap cleanup_zone 0 1 2 3 15

cleanup=1
"$dkzone" -S -c "$sectors" -s "$start_lba" "$dev"
cleanup=0
