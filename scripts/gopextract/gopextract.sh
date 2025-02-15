#!/bin/sh
set -e

uefiextract="$PWD/UEFIExtract"
to_find="AmdGopDriver"
mode="by_name"

err() {
	printf "${0}: %s\n" "$@"
	if [ -e "$work_dir" ]; then
		rm -rf "$work_dir"
	fi
	exit 1
}

log() {
	printf "! %s\n" "$@" 1>&2
}

print_help() {
cat << EOL 1>&2
gopextract - extract a uefi image
=================================
Usage: $0 {-i file.dat} [-d /path/to/workdir] [-f WhatToFind] [-t filetype] [-c] [-u]

Switches:
  -i [path] input file
  -d [path] work directory path (default: random name in /tmp)
  -f [string] name to find and extract (default: AmdGopDriver)
  -t [string] search by file type / using egrep
  -u [path] specify path to UEFIExtract executable
  -c case insensitive search

Examples:
  default action / extract AmdGopRom.efi:
    $ $0 -i MSI_BIOS.rom -d output

  extract AmdGfxInitPei data:
    $ $0 -i MSI_BIOS.rom -f AmdGfxInitPei -d output

  extract images:
    $ $0 -i MSI_BIOS.rom -c -t '(bitmap|image)' -d output
EOL
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
	-i)
		shift 1
		input="$(eval echo \$$argnum)"
	;;
	-d)
		shift 1
		work_dir="$(eval echo \$$argnum)"
	;;
	-f)
		shift 1
		mode=by_name
		to_find="$(eval echo \$$argnum)"
	;;
	-t)
		shift 1
		mode=by_type
		to_find="$(eval echo \$$argnum)"
	;;
	-u)
		shift 1
		uefiextract="$(eval echo \$$argnum)"
	;;
	-c)
		case_insensitive=-i
	;;
esac; done

# the mess
if [ -z "$input" ]; then
	err "no input file specified"
elif [ -z "$work_dir" ]; then
	err "work directory not specified"
elif ! [ -r "$work_dir" ]; then
	if ! mkdir -p "$work_dir" 2>/dev/null; then
		err "work directory $work_dir not readable"
	fi
elif ! [ -r "$input" ]; then
	err "input file $input does not exist / is not readable"
elif ! [ -x "$uefiextract" ]; then
	err "$uefiextract does not exist or is not executable"
fi


# made so that you can get the default work_dir
# in scripts with workdir="$(gopextract.sh -i ...)"
log "working in:"
echo "$work_dir"

log "copying $input to $work_dir/input.dat"
cp "$input" "$work_dir/input.dat"
cd "$work_dir"

mkdir OUTPUT

log "extracting bios image"
"$uefiextract" input.dat all 1>&2

log "finding files"
case "$mode" in
	by_name)
		old_ifs=$IFS
		IFS=$'\n'
		guid="$(cat input.dat.guids.csv | grep $case_insensitive -E -- "$to_find" | awk -F, '{print $1}')"
		name="$(cat input.dat.guids.csv | grep $case_insensitive -E -- "$to_find" | awk -F, '{print $2}' | tr '\n' '_')"
		if [ -z "$name" ] || [ -z "$name" ]; then
			err "$to_find is not in guids.csv"
		fi
		for finfo in $(find input.dat.dump -type f -name 'info.txt'); do
			if grep -q "$guid" "$finfo"; then
				log "found $name"
				cp -a "$(dirname "$finfo")" "$(mktemp -d -t "$name" -p OUTPUT)"
			fi
		done
		IFS=$old_ifs
	;;
	by_type)
		old_ifs=$IFS
		IFS=$'\n'
			for file in $(find input.dat.dump -type f -name '*.bin'); do
				if file "$file" | sed 's/.*\.bin://g' | grep -q $case_insensitive -E "$to_find"; then
					fn="$(basename "$file" | tr ' ' '_')"
					log "found $fn"
					cp -a "$file" "$(mktemp -u -t $fn -p OUTPUT)".dat
				fi
			done
		IFS=$old_ifs
	;;
esac

log "ended work, output is in:"
log "$work_dir"/OUTPUT
