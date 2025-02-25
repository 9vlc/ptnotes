#!/bin/sh
# this is a example bhyve vm with amd igpu passthrough

# exit if not being ran by root
if [ "$(whoami)" != root ]; then
	echo "must be root to run this"
	exit 1
fi

# load nmdm module for serial
if ! kldstat | grep -q nmdm.ko; then
	kldload nmdm
fi

bhyve \
	-D -P -S -H -A -w \
	-c sockets=1,cores=4 \
	-m 8G \
	`: ` \
	-s 0,amd_hostbridge \
	-s 31,lpc \
	`: lpc stuff` \
	-o pci.0.31.0.pcireg.vendor=host \
	-o pci.0.31.0.pcireg.device=host \
	-o pci.0.31.0.pcireg.subvendor=host \
	-o pci.0.31.0.pcireg.subdevice=host \
	-l com1,/dev/nmdm0A \
	-l bootrom,/usr/local/share/edk2-bhyve/BHYVE_UEFI.fd \
	`: devices` \
	-s 1,nvme,/vms/windows.raw \
	-s 5,e1000,tap0 \
	`: passthru devices` \
	-s 4:0,passthru,4/0/0,/root/vms/vbios_vega.rom \
	-s 4:1,passthru,4/0/1,/root/vms/AmdGopDriver.rom \
	-s 4:2,passthru,4/0/2 \
	-s 4:3,passthru,4/0/3 \
	-s 4:4,passthru,4/0/4 \
	-s 4:5,passthru,4/0/5 \
	-s 4:6,passthru,4/0/6 \
	windows
