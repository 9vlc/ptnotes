# The BHYVE GPU passthrough compatibility list

# Intel:
## Intel iGPU

- Tested on Thinkpad X250 i3 & T580 i7

ppt binding: works

Early driver loading:
- bhyve crash (`Unhandled inb 0x0402 at 0xbe791370`)

Late driver loading:
- Windows: doesn't work
- Linux: works
- FreeBSD: works

## Intel ARC dGPU
Not tested.

# AMD
## AMD iGPU

ppt binding: works

Early driver loading:
- EDK2: works
- Windows: works
- Linux: works
- FreeBSD: not tested (most likely works)

* Note: Needs vbios extracted from ACPI tables & AmdGopDriver.efi converted to a PCI OpRom with EDK2 EfiRom
* Note: Suffers from reset bug, improper VM shutdown breaks passthrough / locks up host machine

Late driver loading:
- Windows: doesn't work
- Linux: doesn't work
- FreeBSD: doesn't work

## AMD dGPU
### RX 4XX - 6XXX

ppt binding: works

Early driver loading:
- EDK2: works
- Windows: works
- Linux: works
- FreeBSD: work (only RX 580 tested)

Late driver loading (blank/garbage vbios):
- Windows: RX 580 doesn't work, later series not tested
- Linux: doesn't work
- FreeBSD: not tested

* Note: Suffers from reset bug, improper VM shutdown breaks passthrough / locks up host machine

### RX 7XXX

ppt binding: works

Early driver loading:
- needs further testing. just passing vbios doesn't work.

Late driver loading (blank/garbage vbios):
- Windows: works (pulls from acpi)
- Linux: works (??????)
- FreeBSD: sometimes, on a good day

* Note: HEAVILY suffers from reset bug, ANY VM shutdown requires to hard reboot the host machine (passthrough breaks, locks up on attempted shutdown)
* Note: https://github.com/inga-lovinde/RadeonResetBugFix help a bit with host shutdown on Windows guests, still reboot the guest on every vm shutdown
* Note: RadeonResetBugFix causes VM to bluescreen / bhyve to crash on install, works on next boot
