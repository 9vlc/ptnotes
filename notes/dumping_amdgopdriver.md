# dumping AmdGopDriver

## building UEFIExtract

- aquire the source code of UEFITool from https://github.com/LongSoft/UEFITool
- install `meson` and `ninja`
- `meson setup build && cd build && ninja` in the UEFITool directory

as shell:
```sh
git clone --depth=1 https://github.com/LongSoft/UEFITool
cd UEFITool
meson setup build
cd build
ninja
```

this will build UEFIExtract, UEFIFind and UEFITool.
copy the built UEFIExtract executable to something like your home directory

## getting your bios

either download it from your motherboard manufacturers website either use dump your bios using one of these four methods:
1. compile `dump-bios.c` from this repo's old progs directory, run it with the following args: `./dump 32m 4080m bios.bin`
2. `camdd -m 32m -i file=/dev/mem,offset=4080m -o file=bios.bin`
3. `dd if=/dev/mem of=bios.bin bs=1M count=32 skip=4080`
4. install flashrom and do `flashrom -p internal -r bios.bin`

notes:
- you might need to replace 32(m) with 8 or 16 depending on your actual bios size.
- if downloading: make sure it's a raw file sized at a round number like 8, 16 or 32 megabytes. there may be a windows installer and something like EZFLASH installer, chose the flash one as it's the raw file.

## extracting AmdGopDriver.efi

- get `extract-by-sig.sh` from this repo's scripts directory
- create a directory and put `extract-by-sig.sh`, `UEFIExtract` and your BIOS inside
- chmod +x `extract-by-sig.sh` and `UEFIExtract`
- `./extract-by-sig.sh bios.bin output`
- `mv output/AmdGop* ./AmdGopDriver`
- `file AmdGopDriver`
- now, if the output of `file` is "TE image" or "data", rename AmdGopDriver to AmdGopDriver.te. if it is PE32, rename it to AmdGopDriver.efi
- if you got a TE image, go to `converting TE to EFI`, if you got PE32, skip to `converting the efi to a pci option rom`.

### converting TE to EFI

we are going to do this right in the directory with AmdGopDriver.te
- compile TE2PE:
```sh
git clone https://github.com/LongSoft/TE2PE
cc TE2PE/TE2PE.c -o TE2PE.elf
```
- convert AmdGopDriver.te to PE32:
```
./TE2PE.elf AmdGopDriver.te AmdGopDriver.efi
```

## converting the efi to a pci option rom

- install `gcc`, `gmake` and `python`
- download edk2 sources with git: `git clone --recurse-submodules --depth=1 https://github.com/tianocore/edk2`
- build the BaseTools:
```sh
cd edk2/BaseTools
gmake -j$(nproc)
```
- copy over `edk2/BaseTools/Sources/C/bin/EfiRom` to the directory with AmdGopDriver.efi
- go to that directory
- `./EfiRom -f 0x1002 -i 0xffff -e AmdGopDriver.efi -o AmdGopDriver-OpRom.rom`

congratulations, you got yourself a PCI option ROM of AmdGopDriver
