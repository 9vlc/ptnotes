#!/bin/sh
set -e

out='out'

if [ ! -d "$1" ]; then
	echo "input is not a directory"
	exit 1
elif [ ! -d "$out" ]; then
	mkdir -p "$out"
fi

oldifs=$IFS
IFS=$'\n'
for file in $(find "$1" -type f); do
	ftype="$(file "$file"|sed "s/.*$(basename "$1")*.*: //"|awk '{print$1$2$3}'|tr \  _)"
	echo "$file" >> "$out/$ftype"
	echo "$(basename "$file") => $ftype"
done
IFS=$oldifs
