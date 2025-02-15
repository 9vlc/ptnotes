#!/bin/sh

gopdump="$PWD/gopdump.sh"
msi_bios="$PWD/E7C56AMS.AI0"

tc_1_desc=\
"
default case
a bare input and workdir
"
tc_1() {
	# default extract
	sh "$gopdump" -i "$msi_bios" -d "$1"
}

tc_2_desc=\
"
extracting all images and bitmaps from a bios
output should consist of mostly (>80%) images
"
tc_2() {
	sh "$gopdump" -i "$msi_bios" -t "(image|bitmap)" -d "$1"
}

tc_3_desc=\
"
extracting all images and bitmaps from a bios
but case insensitive (-c to script)
searching 'image' as 'iMaGE' and
'bitmap' as 'BiTmAp'
output should consist of mostly (>80%) images
"
tc_3() {
	sh "$gopdump" -i "$msi_bios" -c -t "(iMaGE|BiTmAp)" -d "$1"
}

tc_4_desc=\
"
extracting AmdGfxInit, a non standard module
output should consist of a bin dependency file,
a TE image and a preload PE image.
"
tc_4() {
	sh "$gopdump" -i "$msi_bios" -f "AmdGfxInit" -d "$1"
}

tc_5_desc=\
"
extracting AmdGfxInit, a non standard module,
but case insensitive (-c to script)
searching 'AmdGfxInit' as 'aMdGFXiniT'
output should consist of a bin dependency file,
a TE image and a preload PE image.
"
tc_5() {
	sh "$gopdump" -i "$msi_bios" -c -f "aMdGFXiniT" -d "$1"
}


rm -rf test
mkdir test
for test in $(seq 5); do
	mkdir test/$test
	echo "/// TEST $test ///"
	eval echo "\"\$tc_${test}_desc\""
	eval echo "\"\$tc_${test}_desc\"" > test/$test/DESC
	tc_$test test/$test >test/$test/LOG 2>test/$test/LOG
done
