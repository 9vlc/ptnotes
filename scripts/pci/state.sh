#!/bin/sh
# vim: noet:sw=2:ts=2:
set -eu

#
# safety switch.
# only set to 0 (in env) if you know how much you can damage your hardware
# by writing random stuff to vendor registers
#
strip_above_ff="${strip_above_ff:-1}"

#
# fork all the var loading processes to bg then wait
#
forked_load=1

action="${1:-}"
state_dir="${2:-}"
devices="${pptdevs:-}"

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

#
# width and devices
#
width=4
if _p "${3:-}" | grep -qE '^[1-9]$'; then
	width="$3"
	shift 1
fi

if [ "${3:-}" ]; then
	shift 2
	devices="$*"
fi

#
# help
#
if [ -z "$action" ] || [ -z "$state_dir" ] || [ -z "$devices" ]; then
	>&2 cat << eol
usage: state.sh save/load statedir [width] [dev 1] [dev 2] ...
save and load the config space of a list of pci devices.

note: this script can take a device list from a pptdevs env variable, but
      prioritizes arguments over it. if a width is not provided,
      4 bytes is used as a default value.

note: this script strips all variables above 0xff when loading by default
      to prevent unexpected device behavior and damage.
      set 'strip_above_ff=0' in environment to override this.

eol
	exit 1
fi

[ -d "$state_dir" ] || mkdir -p "$state_dir" 

#
# logic
#
case "$action" in
	s*)
		for dev in $devices; do
			var_file="$state_dir/$(_p "$dev" | sed 's/[^a-z0-9]/_/g')"
			vars="$(_d_save_conf "$dev" "$width")" || exit $?
			_P "$vars" > "$var_file"
		done
	;;
	l*)
		[ -d "$state_dir" ] || _l e "no such state directory: $state_dir"
		for dev in $devices; do
			var_file="$state_dir/$(_p "$dev" | sed 's/[^a-z0-9]/_/g')"
			#
			# assume everything that isn't 0 as yes for safety
			#
			if [ "$strip_above_ff" = 0 ]; then
				vars="$(cat "$var_file")"
			else
				vars="$(awk -F, '
					/^[^#]/{
						$0 = tolower($0)
						if ($1 ~ /^[0-9a-f].?$/) print $0
					}' "$var_file")"
			fi

			[ -f "$var_file" ] || _l w "missing device state file: $var_file"
			
			if [ "$forked_load" = 1 ]; then
				if _P "$vars" | _d_load_conf "$dev"; then
					_p "$dev "
				fi &
			else
				_l i "loading variables for $dev"
				_P "$vars" | _d_load_conf "$dev"
			fi
		done; wait
		>&2 echo
	;;
	*) echo "?" ; exit 1 ;;
esac
