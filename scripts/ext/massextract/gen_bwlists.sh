#!/bin/sh
set -e

nproc=$(nproc)

if [ ! -r "$1" ]; then
	echo "input file not readable"
	exit 1
fi

process_entry() {
	lname="$(echo "$2" | sed 's/\//\^%/g')"
	out="$(binwalk "$2" | tail +4 | uniq)"
	if [ "$out" ]; then
		echo "$lname"
		echo "$out" >> "$1.bwlists/$lname"
	fi
}

entries="$(cat "$1")"

if [ -d "$1.dir" ]; then
	rm -rf "$1.bwlists"
	mkdir "$1.bwlists"
else
	mkdir "$1.bwlists"
fi

oldifs=$IFS
IFS=$'\n'
counter=1
for entry in $entries; do
	if [ "$counter" -gt "$nproc" ]; then
		counter=1
		wait
	fi
	process_entry "$1" "$entry" &
	counter="$((counter+1))"
done
wait
IFS=$oldifs
