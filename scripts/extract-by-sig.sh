#!/bin/sh
set -e

nproc="$(nproc)"
uefiextract="$HOME/Desktop/util/UEFIExtract"
# format: hex data to find::output name::output extension
signatures="41 00 4D 00 44 00 20 00 47 00 4F 00 50 00 20 00 58 00 36 00 34::AmdGopDriver::efi"

# check for valid input
if [ -z "$1" ] || [ -z "$2" ]; then
	echo "usage: $0 [file.rom] [output-dir] {signatures.txt}" 1>&2
	exit 1
elif [ ! -r "$1" ] || [ -d "$1" ]; then
	echo "$0: cannot read rom from $1" 1>&2
	exit 1
elif ! mkdir "$2" 2>/dev/null; then
	echo "$0: cannot create output directory" 1>&2
	exit 1
fi

input="$PWD/$1"
work_dir="$PWD/$2"

cleanup()
{
	if [ -d "$work_dir" ]; then
		rm -rf "$work_dir"
	fi
	exit 1
}
trap cleanup INT TERM HUP

# check if we recieve a signature list
if [ "$3" ]; then
	if [ -f "$3" ] && [ -r "$3" ]; then
		signatures="$(cat "$3")"
	else
		echo "$0: could not read signatures from $3" 1>&2
		cleanup
	fi
fi


# the thing
sigcheck()
{
	# we need to be very careful here to not cause ANY errors.
	local file="$1"
	local out_dir="$2"
	local signatures="$3"
	local counter="$4"

	local sig_hex
	local sig_fn
	local sig_ext

	for signature in $signatures; do
		sig_hex="$(echo "$signature" | awk -F:: '{print$1}')"
		sig_fn="$(echo "$signature" | awk -F:: '{print$2}')"
		sig_ext="$(echo "$signature" | awk -F:: '{print$3}')"

		# do case sensitive because case insensitive grep is slow
		if hexdump -e '1/1 "%02x"' "$file" | grep -q "$sig_hex"; then
			echo "found $sig_hex at ...$(echo -n "$file"|tail -c30)" 1>&2
			cp "$file" "$out_dir"/"$sig_fn"_"$counter"."$sig_ext"
		fi
	done
}

extract()
{
	local input="$1"
	local work_dir="$2"
	local signatures="$3"
	local oldifs=$IFS

	local new_signatures
	local counter
	local thread_counter
	local signature
	local sig_hex
	local sig_fn
	local sig_ext

	IFS=$'\n'

	# check if the signature list is valid

	counter=1
	new_signatures=
	for signature in $signatures; do
		sig_hex="$(echo "$signature" | awk -F:: '{print$1}')"
		sig_fn="$(echo "$signature" | awk -F:: '{print$2}')"
		sig_ext="$(echo "$signature" | awk -F:: '{print$3}')"

		if [ -z "$sig_hex" ]; then
			echo "$0: hex signature missing at $counter" 1>&2
			return 1
		elif [ -z "$sig_fn" ]; then
			echo "$0: file name signature missing at $counter" 1>&2
			return 1
		elif [ -z "$sig_ext" ]; then
			echo "$0: file extension signature missing at $counter" 1>&2
		fi

		# we also build a new signature list here with a formatted hex sig list
		sig_hex="$(echo -n "$sig_hex"|sed 's/ //g'|dd conv=lcase 2>/dev/null)"
		sig_fn="$(echo -n "$sig_fn"|sed 's/_//g')"
		sig_ext="$(echo -n "$sig_ext"|sed 's/_//g')"
		new_signatures="$sig_hex::$sig_fn::$sig_ext
			$new_signatures"

		counter=$((counter+1))
	done

	cd "$work_dir"
	mkdir output
	cp "$input" b
	"$uefiextract" b 1>&2 || return 1

	# find and copy matching files to output directory
	counter=1
	thread_counter=0
	for file in $(find b.dump -type f -name body.bin); do
		if [ "$thread_counter" -ge "$nproc" ]; then
			wait
			thread_counter=0
		fi

		sigcheck "$file" output "$new_signatures" "$counter" &

		thread_counter=$((thread_counter+1))
		counter=$((counter+1))
	done
	wait

	IFS=$oldifs

	rm -rf b b.*
	if [ "$(echo output/*)" = 'output/*' ]; then
		echo "$0: nothing found" 1>&2
		cleanup
	fi
	mv output/* .
	rmdir output
}

if ! extract "$input" "$work_dir" "$signatures"; then
	cleanup
fi
