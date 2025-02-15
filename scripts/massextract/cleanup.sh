#!/bin/sh
set -e

blacklist='(^ data$|TIFF image)'
icase=1

oldifs=$IFS
IFS=$'\n'
olddir="$PWD"

if [ "$icase" = 1 ]; then
	icase_def=-i
fi

cd "$1"
for file in $(find . -type f); do
	if file "$file" | awk -F: '{print $2}' | grep $icase_def -E "$blacklist"; then
		rm -v "$file"
	fi
done

cd "$olddir"
IFS=$oldifs
