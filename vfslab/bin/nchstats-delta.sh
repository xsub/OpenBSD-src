#!/bin/sh
#
# Report namecache activity over the lifetime of a command.
#
# kern.nchstats is maintained by the stock kernel and exported by
# sysctl(2); no kernel change is needed to read it.  The counters are
# system wide, so run this on an otherwise idle machine.
#
# usage: nchstats-delta.sh command [argument ...]

set -e

fields="good_hits negative_hits bad_hits false_hits misses long_names"

snapshot()
{
	for f in $fields; do
		sysctl -n "kern.nchstats.$f"
	done
}

if [ $# -lt 1 ]; then
	echo "usage: ${0##*/} command [argument ...]" >&2
	exit 1
fi

before=$(snapshot)
rc=0
"$@" || rc=$?
after=$(snapshot)

echo
echo "namecache delta over: $* (exit $rc)"

i=1
for f in $fields; do
	b=$(echo "$before" | sed -n "${i}p")
	a=$(echo "$after" | sed -n "${i}p")
	d=$((a - b))
	eval "d_$f=\$d"
	printf '%-16s %12d\n' "$f" "$d"
	i=$((i + 1))
done

lookups=$((d_good_hits + d_negative_hits + d_bad_hits))
lookups=$((lookups + d_false_hits + d_misses))
if [ "$lookups" -gt 0 ]; then
	hits=$((d_good_hits + d_negative_hits))
	printf '%-16s %11d%%\n' "hit rate" $((hits * 100 / lookups))
fi

exit "$rc"
