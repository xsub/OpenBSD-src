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

report()
{
	"$dkzone" -n 4 -s "$start_lba" "$dev"
}

zone_line()
{
	awk '
		$1 == "0" {
			print
			exit
		}
	'
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

zone_size()
{
	awk '
		/^zone_mode=/ {
			for (i = 1; i <= NF; i++) {
				if ($i ~ /^zone_size_lba=/) {
					sub(/^zone_size_lba=/, "", $i)
					print $i
					exit
				}
			}
		}
	'
}

echo "== initial report =="
initial=$(report)
echo "$initial"

size=$(printf '%s\n' "$initial" | zone_size)
[ -n "$size" ] || die "could not parse zone_size_lba"
finish_wp=$((start_lba + size))

initial_start=$(printf '%s\n' "$initial" | zone_field 2)
[ -n "$initial_start" ] || die "could not parse first zone start LBA"
[ "$initial_start" = "$start_lba" ] ||
	die "start-lba must name a zone start; report returned $initial_start"

initial_cond=$(printf '%s\n' "$initial" | zone_field 7)
[ -n "$initial_cond" ] || die "could not parse first zone condition"

if [ "$initial_cond" != "empty" ]; then
	echo "first zone is $initial_cond, resetting before smoke test"
	"$dkzone" -m reset -l "$start_lba" "$dev"
fi

echo "== finish zone =="
"$dkzone" -m finish -l "$start_lba" "$dev"

finished=$(report)
echo "$finished"
finished_line=$(printf '%s\n' "$finished" | zone_line)
[ -n "$finished_line" ] || die "could not parse finished zone line"

finished_wp=$(printf '%s\n' "$finished" | zone_field 5)
finished_cond=$(printf '%s\n' "$finished" | zone_field 7)

[ "$finished_cond" = "full" ] ||
	die "expected first zone condition full, got $finished_cond"
[ "$finished_wp" = "$finish_wp" ] ||
	die "expected first zone wp_lba $finish_wp after finish, got $finished_wp"

echo "== reset zone =="
"$dkzone" -m reset -l "$start_lba" "$dev"

reset=$(report)
echo "$reset"
reset_line=$(printf '%s\n' "$reset" | zone_line)
[ -n "$reset_line" ] || die "could not parse reset zone line"

reset_wp=$(printf '%s\n' "$reset" | zone_field 5)
reset_cond=$(printf '%s\n' "$reset" | zone_field 7)

[ "$reset_cond" = "empty" ] ||
	die "expected first zone condition empty, got $reset_cond"
[ "$reset_wp" = "$start_lba" ] ||
	die "expected first zone wp_lba $start_lba after reset, got $reset_wp"

echo "ok"
