#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include "trim.h"

static void
usage(const char *progname)
{
	fprintf(stderr, "usage: %s [input.rom] [output.rom]\n", progname);
	exit(1);
}

ssize_t
find_header(uint8_t *buffer, size_t buffer_size)
{
	int match, i, j;

	if (buffer_size < HEADER_LENGTH)
		return -1;

	for (i = 0; i <= buffer_size - HEADER_LENGTH; i++)
	{
		match = 1;
		for (j = 0; j < HEADER_LENGTH; j++)
		{
			if (vbios_mask[j] && (buffer[i + j] != vbios_header[j]))
			{
				match = 0;
				break;
			}
		}
		if (match)
			return i;
	}
	return -1;
}

int
main(int argc, char *argv[])
{
	char *progname;
	FILE *input_file, *output_file;
	long input_size;

	uint8_t *buffer;
	size_t input_file_bytes, new_vbios_bytes, written_vbios_bytes;
	ssize_t vbios_offset;

	progname = argv[0];

	if (argc <= 2)
		usage(progname);

	input_file = fopen(argv[1], "rb");
	if (input_file == NULL)
	{
		fprintf(stderr, "%s: input file could not be read\n", progname);
		exit(1);
	}

	fseek(input_file, 0, SEEK_END);
	input_size = ftell(input_file);
	if (input_size <= INPUT_SIZE_THRESHOLD)
	{
		fprintf(stderr, "%s: input file too small (%ldb, min threshold %ib)\n", progname, input_size, INPUT_SIZE_THRESHOLD);
		fclose(input_file);
		exit(1);
	}
	rewind(input_file);

	buffer = malloc(input_size);
	if (!buffer)
	{
		fprintf(stderr, "%s: could not allocate memory for input\n", progname);
		fclose(input_file);
		exit(1);
	}

	input_file_bytes = fread(buffer, 1, input_size, input_file);
	if (input_file_bytes != input_size)
	{
		fprintf(stderr, "%s: issue reading input: expected %ld bytes, got %zu bytes\n", progname, input_size, input_file_bytes);	fclose(input_file);
		free(buffer);
		exit(1);
	}

	fclose(input_file);

	vbios_offset = find_header(buffer, input_size);
	if (vbios_offset < 0)
	{
		fprintf(stderr, "%s: could not find vbios header inside input\n", progname);
		free(buffer);
		exit(1);
	}
	fprintf(stderr, "%s: found vbios header at %zd\n", progname, vbios_offset);

	output_file = fopen(argv[2], "wb");
	if (output_file == NULL)
	{
		fprintf(stderr, "%s: could not write to output file", progname);
		free(buffer);
		exit(1);
	}

	new_vbios_bytes = input_size - vbios_offset;
	written_vbios_bytes = fwrite(buffer + vbios_offset, 1, new_vbios_bytes, output_file);
	if (written_vbios_bytes != new_vbios_bytes)
		fprintf(stderr, "%s: !!! issue writing output: expected %zu bytes, wrote %zu bytes !!!\n", progname, new_vbios_bytes, written_vbios_bytes);

	free(buffer);
	fclose(output_file);

	return 0;
}
