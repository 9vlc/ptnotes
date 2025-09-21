#!/bin/sh
# vim: noet:sw=2:ts=2:
set -eu

action="${1:-}"
state_dir="${2:-}"
devices="${pptdevs:-}"
[ "${3:-}" ] && shift 2 && devices="$*"

#
# get the script directory
#
if script_dir="$(command -v "$0")"; then
	script_dir="$(dirname "$(realpath "$script_dir")")"
else
	script_dir="$(dirname "$(realpath "$0")")"
fi

#
# load in a bunch of functions
#
. "$script_dir"/helpers.in

#
# are we root?
#
[ "$USER" = root ] || _l e "must be root to run this"

if [ -z "$action" ] || [ -z "$state_dir" ] || [ -z "$devices" ]; then
	>&2 echo "usage: state.sh save/load statedir [dev 1] [dev 2] ..."
	>&2 echo "save and load the config space of a list of pci devices"
	>&2 echo "note: variables above 0xff shall not be written to unless you know what you're doing"
	>&2 echo
	>&2 echo "this script takes a device list from a pptdevs env variable or passed arguments"
	>&2 echo "note: argument devices are prioritized over environment"
	exit 1
fi

[ -d "$state_dir" ] || mkdir -p "$state_dir" 

case "$action" in
	s*)
		for dev in $devices; do
			var_file="$state_dir/$(_p "$dev" | sed 's/[^a-z0-9]/_/g')"
			vars="$(_d_save_conf "$dev")" || exit $?
			_P "$vars" > "$var_file"
		done
	;;
	l*)
		[ -d "$state_dir" ] || _l e "no such state directory: $state_dir"
		for dev in $devices; do
			var_file="$state_dir/$(_p "$dev" | sed 's/[^a-z0-9]/_/g')"
			[ -f "$var_file" ] || _l w "missing device state file: $var_file"
			
			_l i "loading variables for $dev"
			_d_load_conf "$dev" < "$var_file"
		done
	;;
	*) echo "?" ; exit 1 ;;
esac
