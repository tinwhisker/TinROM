#!/bin/sh
# tinrom - every host-side check, one after another.
#
#     sh scripts/test-all.sh
#
# scripts/syntax-check.sh, then every scripts/test-*.sh, each in its own
# shell so one failing does not stop the rest. Prints each one's last line
# as it goes - its verdict - and at the end the ones that failed, with the
# command to run one again on its own for the whole of its output. Exits
# non-zero if any did.
#
# Needs what the individual scripts need: a C compiler, Python 3, and zlib
# for the ones that write PNGs.
root=$(cd "$(dirname "$0")/.." && pwd)
log=$(mktemp); trap 'rm -f "$log"' EXIT
failed=""
count=0

for script in "$root/scripts/syntax-check.sh" "$root"/scripts/test-*.sh; do
	name=$(basename "$script")

	[ "$name" = "test-all.sh" ] && continue
	count=$((count + 1))

	if sh "$script" > "$log" 2>&1; then
		printf '  ok    %-20s %s\n' "$name" "$(tail -n 1 "$log")"
	else
		printf '  FAIL  %-20s %s\n' "$name" "$(tail -n 1 "$log")"
		failed="$failed $name"
	fi
done

echo
if [ -n "$failed" ]; then
	set -- $failed
	echo "Failed:$failed"
	echo "Run one again on its own for its full output, e.g.  sh scripts/$1"
	exit 1
fi

echo "All $count passed."
