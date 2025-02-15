#!/bin/sh
set -e

# extract-binwalk.sh
# By ninevlc https://github.com/9vlc
# BSD 3-Clause License

print_help() {
	cat << EOL
Useage: $ $0 {-i file.dat} [-m map.txt] [-e OFFSET]

map.txt is a text file that contains a list of embedded files in the raw data file (outputted by binwalk)
Example:
273283        0x42B83         JPEG image data, JFIF standard 1.01
274051        0x42E83         JPEG image data, JFIF standard 1.01
274346        0x42FAA         JPEG image data, JFIF standard 1.01
274642        0x430D2         JPEG image data, JFIF standard 1.01
(All we really care about here is the first column - decimal offset)
If not provided, the map will be taken from stdin
EOL
}

get_offset() {
	offset_midcheck="$(echo "$map" | awk "NR == $1 {print \$1}" | tr 'A-z!-)@-+*=' E)"
	if echo "$offset_midcheck" | grep -q E; then
		echo "INVALID"
		return 1
	fi
	echo "$offset_midcheck"
}

if [ -z "$*" ]; then
	print_help
	exit 1
fi

for argnum in $(seq $#); do case "$(eval echo \$$argnum)" in
	-h|--help)
		print_help
		exit
	;;
	-i|--in*)
		shift 1
		input="$(eval echo \$$argnum)"
	;;
	-m|--map*)
		shift 1
		mapfile="$(eval echo \$$argnum)"
	;;
	-e|--ext*)
		shift 1 
		extra_end="$(eval echo \$$argnum)"
	;;
esac; done

if [ ! -e "$input" ]; then
	echo "$0: $input: No such file."
	exit 1
fi

if [ "$mapfile" ]; then
	map="$(cat $mapfile)"
else
	map="$(cat)"
fi

if [ "$extra_end" ]; then
	extra_end_old="$extra_end"
	extra_end="$(echo "$extra_end" | tr 'A-z!-)@+*=' E)"
	if echo "$extra_end" | grep -q E; then
		echo "Invalid definition of end offset: $extra_end_old"
		exit 1
	fi
fi

map="$(echo "$map" | awk '1' RS='' OFS='\n')"
map_entries="$(($(echo "$map" | wc -l)))"
input_file_size="$(($(wc -c<"$input")))"

if [ 1 -gt "$map_entries" ]; then
	echo "Nothing to extract"
	exit 1
fi

echo "Extracting $map_entries entries..."

#mkdir -v output
for current_entry in $(seq $map_entries); do

	# a billion checks incoming!!!
	offset="$(get_offset $current_entry)"
	if [ "$offset" = INVALID ]; then
		echo "Invalid decimal offset in the map at position $current_entry"
		exit 1
	fi
	
	end_offset="$(get_offset $((current_entry + 1)))"
	if [ "$end_offset" = INVALID ]; then
		echo "Invalid decimal offset in the map at position $((current_entry+1))"
		exit 1
	fi
	
	if [ -z "$end_offset" ]; then
		end_offset="$input_file_size"
	fi

	if [ "$offset" -gt "$input_file_size" ]; then
		echo "Offset at position $current_entry is bigger than the input file"
		echo "Did you by any chance input the wrong file?"
		exit 1
	fi
	
	if [ "$offset" -gt "$end_offset" ]; then
		echo "WARNING WARNING WARNING"
		echo "Offset at position $current_entry larger than next offset"
		echo "Setting the end offset to the input file size"
		echo "PLEASE. FIX. THE. MAP."
		end_offset="$input_file_size"
	fi

	# finally
	echo "Extracting ${offset}-${end_offset}..."

	tail -c+"$((offset + 1))" "$input" | head -c"$((end_offset - offset + extra_end))" > extracted_${offset}.dat
	# replace previous line with this if your os doesn't support -c+N in tail
	# dd if="$1" of="extracted_${offset}.dat" skip="$((offset + 1))" bs=1 count=$((end_offset - offset + extra_end))
done
