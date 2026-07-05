#!/bin/sh
set -eu

usage()
{
	echo "usage: $0 raw-device [start-lba]" >&2
	echo "example: $0 /dev/rsd1c 0" >&2
	echo "warning: this finishes and resets the target zone" >&2
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

report_filter()
{
	"$dkzone" -r "$1" -n 4 -s "$start_lba" "$dev"
}

zone_field()
{
	field=$1
	awk -v field="$field" '
		$1 == "0" {
			print $field
			exit
		}
	'
}

assert_first_zone()
{
	filter=$1
	expected=$2

	output=$(report_filter "$filter")
	echo "$output"

	start=$(printf '%s\n' "$output" | zone_field 2)
	[ -n "$start" ] || die "could not parse first $filter zone start LBA"
	[ "$start" = "$start_lba" ] ||
		die "expected first $filter zone at $start_lba, got $start"

	condition=$(printf '%s\n' "$output" | zone_field 7)
	[ -n "$condition" ] || die "could not parse first $filter zone condition"
	[ "$condition" = "$expected" ] ||
		die "expected first $filter zone condition $expected, got $condition"
}

echo "== reset zone before report filter probe =="
"$dkzone" -m reset -l "$start_lba" "$dev"
cleanup=1

echo "== report empty zones =="
assert_first_zone empty empty

echo "== finish zone =="
"$dkzone" -m finish -l "$start_lba" "$dev"

echo "== report full zones =="
assert_first_zone full full

echo "== reset zone after report filter probe =="
"$dkzone" -m reset -l "$start_lba" "$dev"
cleanup=0

echo "ok"
