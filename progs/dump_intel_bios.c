#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <ctype.h>
#include <assert.h>

/* 
 * default: 33554432 (32m)
 * FULL VALUE ALLOCATED AT RUNTIME.
 */
#define MAX_ROM_LENGTH 33554432

unsigned char buffer[MAX_ROM_LENGTH];

static void
print_usage(const char *progname)
{
	(void)fprintf(stderr, "usage: %s [length] [offset] [output.bin]\n", progname);
	exit(1);
}

unsigned long
errchk_strtol(const char *input, const char *progname)
{
	char *endptr;
	long long output;
	int errno = 0;

	output = strtoll(input, &endptr, 0);
	
	if (output < 0)
	{
		(void)fprintf(stderr, "%s: invalid input: %s\n", progname, input);
		exit(1);
	}

	assert(errno == 0);
	
	/*
	 * we assume the requested value is in bytes and return it if
	 * there's no suffix.
	 */
	if (*endptr == '\0')
		return output;

	switch (tolower((unsigned char)*endptr))
	{
		case 'b':
			break;
		case 'k':
			output *= 1024;
			break;
		case 'm':
			output *= 1024 * 1024;
			break;
		default:
			(void)fprintf(stderr, "%s: unknown suffix: %s\n", progname, endptr);
			exit(1);
	}

	return (unsigned long long)output;
}

int
main(const int argc, const char *argv[])
{
	const char *progname = argv[0];
	int errno = 0;

	FILE *dev_mem, *rom_output;
	size_t read_bytes, wrote_bytes;

	if (argc <= 3)
		print_usage(progname);

	/*
	 * const my beloved
	 */
	const unsigned long long rom_length = errchk_strtol(argv[1], progname);
	const unsigned long long rom_offset = errchk_strtol(argv[2], progname);
	const char *rom_output_str = argv[3];

	if (rom_length > MAX_ROM_LENGTH)
	{
		(void)fprintf(stderr, "%s: rom length too large: %llub (max: %ub)\n", progname, rom_length, MAX_ROM_LENGTH);
		exit(1);
	}

	/*
	 * open /dev/mem and read data here
	 */
	memset(buffer, 0, MAX_ROM_LENGTH); /* initialize the buffer with zeroes */
	dev_mem = fopen("/dev/mem", "rb");
	if (!dev_mem)
	{
		(void)fprintf(stderr, "%s: failed to open /dev/mem\n", progname);
		exit(1);
	}

	if (fseek(dev_mem, rom_offset, SEEK_SET))
	{
		(void)fprintf(stderr, "%s: could not traverse to offset %llu in /dev/mem\n", progname, rom_offset);
		exit(1);
	}

	assert(errno == 0);

	/*
	 * read inconsistencies are expected here since
	 * some parts of the bios may be locked down
	 */
	read_bytes = fread(buffer, 1, rom_length, dev_mem);
	if (read_bytes != rom_length)
	{
		(void)fprintf(stderr, "%s: !! inconsistencies reading rom: expected %llu bytes, got %zu bytes !!\n", progname, rom_length, read_bytes);
		(void)fprintf(stderr, "%s: !! your chipset may be blocking reads to some memory addresses !!\n", progname);
	}

	/*
	 * don't care if fclose returns EOF
	 */
	(void)fclose(dev_mem);
	
	/*
	 * we write the output here
	 */
	rom_output = fopen(rom_output_str, "wb");
	if (!rom_output)
	{
		(void)fprintf(stderr, "%s: failed to write to %s\n", progname, rom_output_str);
		exit(1);
	}

	wrote_bytes = fwrite(buffer, 1, rom_length, rom_output);

	(void)fclose(rom_output);

	return 0;
}
