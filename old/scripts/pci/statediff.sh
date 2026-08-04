#!/bin/sh
# vim: noet:sw=2:ts=2:
set -eu

state1_dir="${1:-}"
state2_dir="${2:-}"

if [ -z "$state1_dir" ] || [ -z "$state2_dir" ]; then
	>&2 cat << eol
usage: statediff.sh state1 state2
provide two state directories to see if any config space variables have changed.
eol
	exit 1
fi

#
# the usual
#
if script_dir="$(command -v "$0")"; then
	script_dir="$(dirname "$(realpath "$script_dir")")"
else
	script_dir="$(dirname "$(realpath "$0")")"
fi
. "$script_dir"/helpers.in

[ -d "$state1_dir" ] || _l e "invalid state directory: $state1_dir"
[ -d "$state2_dir" ] || _l e "invalid state directory: $state2_dir"

#
# find common devices between the two statedirs
#
states=""
for state in $(ls "$state1_dir" | sort -V); do
	if [ -f "$state1_dir/$state" ] && [ -f "$state2_dir/$state" ]; then
		states="$states $state"
	else
		_l w "$state is not a device in state directory 2, skipping"
	fi
done

for state in $states; do
	if ! diff_output="$(diff "$state1_dir/$state" "$state2_dir/$state")"; then
		_l i "states $state differ"
		_P "$diff_output"
		>&2 echo
	fi
done
