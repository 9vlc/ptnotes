# dumping AmdGopDriver

1. building UEFIExtract
- aquire the source code of UEFITool from https://github.com/LongSoft/UEFITool
- install `meson`, `ninja` and QT6 development libraries
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

2. getting your bios
- find your motherboard model
- search for a bios download on the manufacturer's website
- download it

make sure it's a raw file sized at a round number like 8, 16 or 32 megabytes

there may be a windows installer and something like EZFLASH installer, chose the EZFLASH one.

4. extracting AmdGopDriver.efi
- get `gopextract.sh` from this repo
- create a directory and put `gopextract.sh`, `UEFIExtract` and your BIOS inside
- chmod +x `gopextract.sh` and `UEFIExtract`
- `./gopextract.sh -i MyBios.bin -d ext -c -f AmdGopDriver`
- `cp "$(find ext/OUTPUT -name '*image section*')"/body.bin ./AmdGopDriver`
- `file AmdGopDriver`
- now, if the output of `file` is "TE image" or "data", rename AmdGopDriver to AmdGopDriver.te. if it is PE32, rename it to AmdGopDriver.efi
- if you got a TE image, go to 4.1. if you got pe32, skip to 5.

4.1. converting TE to EFI
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

5. converting the efi to a pci option rom
- install `gcc`, `gmake`, `python3` and `nasm` *(python3 and nasm might not be needed)*
- download edk2 sources with git: `git clone --recurse-submodules --depth=1 https://github.com/tianocore/edk2`
- build the BaseTools:
```sh
cd edk2/BaseTools
gmake -j$(nproc)
```
- copy over `edk2/BaseTools/Sources/C/bin/EfiRom` to the directory with AmdGopDriver.efi
- go to that directory
- `./EfiRom -f 0x1002 -i 0xffff -e AmdGopDriver.efi`

congratulations, you got yourself an AmdGopDriver.rom
