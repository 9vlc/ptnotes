#!/bin/sh
# set -x
set -e

nproc=$(nproc)
extract="$PWD/scripts/ext/extract-binwalk.sh"

if [ ! -d "$1" ]; then
	echo "input directory does not exist"
	exit 1
elif [ ! -x "$extract" ]; then
	echo "extract script missing / not executable"
	exit 1
fi


if [ -d out.ext ]; then
	rm -rf out.ext
	mkdir out.ext
else
	mkdir out.ext
fi

cd out.ext
oldifs=$IFS
IFS=$'\n'
counter=1
for filemap in $(find ../"$1" -type f); do
	if [ "$counter" -gt "$nproc" ]; then
		counter=1
		wait
	fi
	
	thing1="$(($(printf "$1"|wc -c)))"
	in_filename="../$(echo "$filemap" | sed 's/\^%/\//g;s/..\/out\/*.*.dir\///')"
	out_filename="$(echo "$filemap" | sed 's/\^%//g;s/..\/out\/*.*.dir\///' | tail -c23)"
	
	: "$filemap"
	: "$filename"
	: "$PWD"
	: NOexit
	
	mkdir E_"$out_filename"
	cd E_"$out_filename"
	$extract -i ../"$in_filename" -m ../"$filemap" -e 1000 &
	cd ..
done
wait
IFS=$oldifs
cd ..
