#!/bin/sh
# vim: noet:sw=2:ts=2:
set -eu

#
# the amazing ppt script
# prerequisites: the generic bhyve pci passthrough ones, aka:
#   iommu w/ virtualization enabled in uefi
#   hw.vmm.amdvi.enable=1 set in loader settings (if on an amd machine)
#   cpu that supports all virtualization stuff
#

pptdevs="${pptdevs:-}"
[ "${1:-}" ] && pptdevs="$*"

err=0

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
# did the script even get devices provided? 
#
if [ -z "$pptdevs" ]; then
	>&2 cat << eol
usage: bind.sh [dev 1] [dev 2] ...
bind a list of pci devices to pptdevs without rebooting

this script takes a device list from a pptdevs env variable or passed arguments
note: argument devices are prioritized over environment
eol
	exit 1
fi

#
# check if we have bhyve vms running
#
if [ -d /dev/vmm ]; then
	_l e "cannot run while bhyve VMs are active" \
	"please stop the following VMs:" \
	"    $(ls /dev/vmm|tr '\n' ' ')"
fi

#
# check if the devices are valid and exist
#
devices=""
for device_ppt in $pptdevs; do
	if ! _d_is_ppt_syntax "$device_ppt"; then
		_l x "error: malformed ppt device: $device_ppt"
	else
		device_pci="$(_d_conv_syntax_pci "$device_ppt")"
		if ! driver="$(_d_get_driver "$device_pci")"; then
			_l x "error: device does not exist: $device_pci"
		fi
	fi
	if [ "$driver" = ppt ]; then
		_l i "device $device_pci already bound to ppt, skipping"
		continue
	fi
	devices="$device_pci $devices"
done

[ "$err" -ne 0 ] && exit "$err"

#
# and then we detach devices from their driver
#
for device in $devices; do
	_d_detach "$device"
done

#
# is vmm unloaded? if so, load it!
#
vmm_prev_loaded=1
if ! kldstat -qn vmm; then
	_l i "loading vmm.ko"
	vmm_prev_loaded=0
	kenv hw.vmm.amdvi.enable=1 2>/dev/null
	kenv pptdevs="$pptdevs" 2>/dev/null
	kldload vmm
fi

#
# manually attach ppt to whatever devices vmm ignored
#
for device in $devices; do
	driver="$(_d_get_driver "$device")"
	if [ "$driver" != ppt ]; then
		if [ "$vmm_prev_loaded" -eq 0 ]; then
			_l w "manually attaching ppt for device $device"
			_l w "if bhyve ppt does not work, add the device to loader's pptdevs!"
		fi
		if ! devctl set driver -f "$device" ppt; then
			_l x "attaching ppt for device $device failed"
		fi
	fi
done

[ "$err" -ne 0 ] && exit "$err"
