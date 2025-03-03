#!/bin/sh
set -e

nproc=5
uefiextract="$PWD/UEFIExtract"

if [ -z "$1" ] || [ -z "$2" ] || [ -z "$3" ]; then
	echo "usage: $0 [file.rom] [output dir] {expression}"
	exit 1
elif [ ! -d "$2" ]; then
	mkdir "$2"
fi

bios="$PWD/$1"
outdir="$2"
exprthing="$3"

extract_thread()
{
	local bios="$1"
	local entry="$2"
	local counter="$3"

	guid=$(echo $entry|awk -F, {print\$1})
	name=$(echo $entry|awk -F, {print\$2})
	counter2=1

	mkdir ext_$counter
	cp "$bios" ext_$counter/r
	cd ext_$counter

	"$uefiextract" r $guid -o d

	for image in $(find d -type d|grep -i 'PE32 image section'); do
		cp "$image"/body.bin ../"$name"_$counter2.efi
		counter2=$((counter2+1))
	done
	
	for image in $(find d -type d|grep -i 'TE image section'); do
		cp "$image"/body.bin ../"$name"_$counter2.te
		counter2=$((counter2+1))
	done

	cd ..
	rm -rf ext_$counter

}

IFS=$'\n'

test_thread()
{
	echo test $1 $2
	sleep 1
}

"$uefiextract" "$bios" guids
found="$(grep -E "$exprthing" "$(basename "$bios")".guids.csv)"

cd "$outdir"

counter1=1
tcounter=0
for entry in $found; do
	if [ $tcounter -ge $nproc ]; then
		wait
		tcounter=0
	fi

	extract_thread "$bios" "$entry" "$counter1" &

	tcounter=$((tcounter+1))
	counter1=$((counter1+1))
done
wait
rm -f "$1".guids.csv
