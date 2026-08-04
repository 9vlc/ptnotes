# The BHYVE GPU passthrough compatibility list

# Intel

## Intel iGPU
- Tested on Thinkpad X250 i3 & T580 i7

#### Intel iGPU Early driver loading
Requirements:
- extract motherboard's BIOS with https://github.com/9vlc/ptnotes/blob/main/progs/dump_intel_bios.c
```sh
# running as root account
# 4080m may not be the correct offset on older motherboards
cc -o biosdump dump_intel_bios.c
./biosdump 16m 4080m bios.rom
```

- Extract IntelGopDriver.efi, IgdAssignmentDxe.efi and PlatformGOPPolicy.efi from the BIOS image (Idg and GOPPolicy files may be downloaded from [here](https://github.com/cmd2001/build-edk2-gvtd/releases/tag/v0.1.0))
- Use EfiRom from EDK2's source tree to combine the EFI files together
```sh
./EfiRom -f 0x8086 -i 0xffff -e IntelGopDriver.efi IgdAssignmentDxe.efi PlatformGOPPolicy.efi -o OpRom.rom
```

- Pass the resulted option rom with `,rom=/path/to/OpRom.rom` in bhyve arguments of the iGPU device

bhyve experience:
- bhyve crash (`Unhandled inb 0x0402 at 0xbe791370`)

-----------------

#### Intel iGPU late driver loading

Requirements:
- Pass through the iGPU alone without any ROMs

bhyve experience:
- Windows: doesn't work
- Linux: works
- FreeBSD: works

-----------------

## Intel ARC dGPU
Not tested

-----------------

# Nvidia
- Tested on T400 and RTX 4090

Notes:
- Use https://github.com/9vlc/ptnotes/tree/main/progs/trim_nvidia_vbios to trim the vbios for it to load properly

-----------------

#### Nvidia early driver loading
Requirements:
- [Download](https://www.techpowerup.com/vgabios/) or extract the GPU's vBIOS with [GPU-z](https://www.techpowerup.com/gpuz/) or [nvflash](https://www.techpowerup.com/download/nvidia-nvflash/)
- Trim the vBIOS with a hex editor (remove everything before `U....IVIDEO`) or with [my tool](https://github.com/9vlc/ptnotes/tree/main/progs/trim_nvidia_vbios)

bhyve experience:
- Windows: works
- Linux: doesn't work (not very tested)
- FreeBSD: not tested

-----------------

#### Nvidia late driver loading

Don't.

-----------------

# AMD
## AMD iGPU

Notes:
- Needs [vBIOS extracted from ACPI tables](https://github.com/9vlc/ptnotes/blob/main/notes/dumping_igpu_vbios_freebsd.md) and [AmdGopDriver.efi converted to a PCI OpRom with EDK2 EfiRom](https://github.com/9vlc/ptnotes/blob/main/notes/dumping_amdgopdriver.md)
- Suffers from reset bug, improper VM shutdown breaks passthrough / locks up host machine

-----------------

#### AMD iGPU early driver loading
Requirements:
- Pass through the GPU VGA and GPU audio device into the VM.
  The audio device is on the same PCI option as the VGA device.
  Example: pci0:4:0:0 and pci0:4:0:1
- Pass vBIOS as a PCI OpROM to the VGA device and AmdGopDriver.rom as the OpRom. Example:
```
...
  -s 7:0,passthru,4/0/0,rom=/vms/vbios.rom \
  -s 7:1,passthru,4/0/1,rom=/vms/AmdGopDriver.rom \
...
```

bhyve experience:
- Windows: works
- Linux: works
- FreeBSD: not tested (most likely works)

-----------------

#### AMD iGPU late driver loading
Don't.

-----------------

## AMD dGPU
### RX 4XX - 6XXX (<=RX 6XXX)
Tested: strange RX 4XX(?), Sapphire RX 580 Pulse, RX 6700 XT 

Notes:
- Suffers from reset bug, improper VM shutdown breaks passthrough / locks up host machine

-----------------

#### AMD dGPU (<=RX 6XXX) early driver loading
Requirements:
- Pass the vBIOS as a PCI OpROM to the GPU VGA device in bhyve. Example:
```
...
  -s 7:0,passthru,25/0/0,rom=/vms/radeon-vbios.rom \
  -s 7:1,passthru,25/0/1 \
... 
```

bhyve experience:
- Windows: works
- Linux: works
- FreeBSD: work (only RX 580 tested)

-----------------

#### AMD dGPU (<=RX 6XXX) late driver loading (blank/garbage vbios)
Requirements:
- Pass an empty / garbage 1-2 MB file as the vBIOS for the GPU VGA device.

bhyve experience:
- Windows: needs further testing
- Linux: needs further testing
- FreeBSD: needs further testing

-----------------

### RX 7XXX (>=RX 7XXX)
- Sapphire RX 7600 currently being tested

Notes:
- HEAVILY suffers from reset bug, ANY VM shutdown requires to hard reboot the host machine (passthrough breaks, host locks up on attempted shutdown)
- https://github.com/inga-lovinde/RadeonResetBugFix help a bit with host shutdown on Windows guests, still reboot the guest on every vm shutdown
- RadeonResetBugFix causes VM to bluescreen / bhyve to crash on install, works on next boot

#### AMD dGPU (>=RX 7XXX) early driver loading
needs further testing. just passing vBIOS doesn't work.
passing AmdGopDriver doesn't work too, we probably need to compile custom EDK2 with modules from regular motherboards patched-in.

-----------------

#### AMD dGPU (>=RX 7XXX) late driver loading
Requirements:
- Either [extract the vBIOS the same way as AMD iGPU](https://github.com/9vlc/ptnotes/blob/main/notes/dumping_igpu_vbios_freebsd.md), either [download one](https://www.techpowerup.com/vgabios/), either use an empty / garbage 1-2 MB file as the vBIOS for the GPU VGA device OpRom.

bhyve experience:
- Windows: works
- Linux: sort of works (memory clock speeds stuck at 25 MHz)
- FreeBSD: sometimes, on a good day, other times kernel panics (needs further testing)


