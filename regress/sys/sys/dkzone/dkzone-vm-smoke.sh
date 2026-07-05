#!/bin/sh
set -eu

usage()
{
	echo "usage: $0 raw-device [start-lba]" >&2
	echo "example: $0 /dev/rsd1c 0" >&2
	echo "warning: this runs mutating ZNS smoke tests" >&2
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

tmp=${TMPDIR:-/tmp}/dkzone-vm-smoke.$$
cleanup()
{
	rm -f "$tmp"
}
trap cleanup 0 1 2 3 15

section "build dkzone"
./dkzone-build.sh

dkzone=./obj/dkzone
[ -x "$dkzone" ] || die "missing built dkzone helper"

section "single-page report"
"$dkzone" -n 4 -s "$start_lba" "$dev"

section "header-only report"
"$dkzone" -n 0 -s "$start_lba" "$dev" >"$tmp"
cat "$tmp"
filled=$(awk '
	/^report=/ {
		for (i = 1; i <= NF; i++) {
			if ($i ~ /^entries_filled=/) {
				sub(/^entries_filled=/, "", $i)
				print $i
				exit
			}
		}
	}
' "$tmp")
[ "$filled" = "0" ] || die "header-only report filled $filled entries"

section "paginated report"
"$dkzone" -p -n 4 -s "$start_lba" "$dev" >"$tmp"

pages=$(awk '/^report=/ { n++ } END { print n + 0 }' "$tmp")
zones=$(awk '$1 ~ /^[0-9]+$/ { n++ } END { print n + 0 }' "$tmp")
last_start=$(awk '$1 ~ /^[0-9]+$/ { start = $2 } END { print start }' "$tmp")

[ "$pages" -gt 1 ] || die "paginated report returned only $pages page"
[ "$zones" -gt 4 ] || die "paginated report returned only $zones zones"
[ -n "$last_start" ] || die "could not parse last reported zone"

echo "pagination pages=$pages zones=$zones last_start_lba=$last_start ok"

section "protocol-dependent report filter"
if "$dkzone" -r reset -n 0 -s "$start_lba" "$dev" >"$tmp" 2>&1; then
	cat "$tmp"
	echo "reset report filter supported ok"
else
	if grep -Eq "Operation not supported|Invalid argument" "$tmp"; then
		cat "$tmp"
		echo "reset report filter unsupported ok"
	else
		cat "$tmp"
		die "unexpected reset report filter failure"
	fi
fi

section "report filters"
./dkzone-report-filter.sh "$dev" "$start_lba"

section "zone management"
./dkzone-zns-smoke.sh "$dev" "$start_lba"

section "write policy"
./dkzone-write-policy.sh "$dev" "$start_lba"

echo
echo "ok"
