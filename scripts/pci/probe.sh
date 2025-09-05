#!/bin/sh
# by 9vlc
set -euo pipefail

devices="6/0/0 6/0/1"

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

################

#
# are we root?
#
[ "$USER" = root ] || _l e "must be root to run this"

if [ -z "${1:-}" ] || [ -z "${2:-}" ]; then
  1>&2 echo "usage: probe.sh save/load dir"
  exit 1
fi

state_dir="${2:-}"
[ -d "$state_dir" ] || mkdir -p "$state_dir" 

case "${1:-}" in
  s*)
    for dev in $devices; do
      var_file="$state_dir/$(printf '%s' "$dev" | tr / _)"

      _d_save_conf "$(_d_conv_syntax_pci "$dev")" > "$var_file" &
    done ; wait
  ;;
  l*)
    [ -d "$state_dir" ] || _l e "no such state directory: $state_dir"
    for dev in $devices; do
      var_file="$state_dir/$(printf '%s' "$dev" | tr / _)"
      [ -f "$var_file" ] || _l w "missing pci var file: $var_file"

      _l i "loading variables for $dev"
      _d_load_conf "$(_d_conv_syntax_pci "$dev")" < "$var_file" &
    done ; wait
  ;;
  *) echo "?" ; exit 1 ;;
esac
