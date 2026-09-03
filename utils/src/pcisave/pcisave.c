/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Alexey Laurentsyeu <alex@paidbsd.org>
 */

/*
 * pcisave.c - Save and restore PCI config space states
 */

#include <sys/stat.h>

#include <ctype.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

#include <debug.h>
#include <crc32.h>
#include <pcisave.h>
#include <nyetpci.h>

/*
 * Structs
 */

struct prog_ctx {
	/* Executable name */
	char *name;

	/* State file pointer */
	FILE *fp;

	/* Operation mode (MODE_INVALID / MODE_SAVE / MODE_LOAD) */
	int mode;

	/* Also write extended config space */
	int flag_ext;

	/* Validate did registers stick after load */
	int flag_validate;

	/* Arguments */
	char *a_device;
	char *a_nolist;
	char *a_yeslist;
	char *a_savefile;

	/* Config space allowlist (OVERRIDE) */
	size_t yeslist_len;
	uint16_t *yeslist;

	/* Config space disallowlist (OVERLAY) */
	size_t nolist_len;
	uint16_t *nolist;
};

enum { MODE_INVALID, MODE_SAVE, MODE_LOAD };

/*
 * Functions
 */

/*
 * Print program usage and exit
 *
 * If arg2 = 1, print full usage instructions
 */
static void
usage(
	const char *name,
	const int full
	)
{
	fprintf(stderr,
"usage: %s -h\n"
"       %s save -d device [-e] [-n off,..] [-y off,..] savefile\n"
"       %s load -d device [-e] [-v] [-n off,..] [-y off,..] savefile\n"
"",
		name, name, name);

	if (full) {
		fprintf(stderr,
"\n"
"Actions:\n"/* Savefile header */
"  save  Read config space from a device and save it to a file\n"
"  load  Read a savefile and write a config space to a device\n"
"\n"
"Options:\n"
"  -d device  PCI selector (same form as pciconf(8), e.g. 'pci0:5:0:0')\n"
"  -e         Save the full 4 KiB extended config space\n"
"  -v         After load, report which dwords did not stick\n"
"  -n off,..  Exclude config space at byte offsets (rounded down to dwords),\n"
"               applied on top of existing blocklist\n"
"  -y off,..  Only apply config space byte offsets from this list,\n"
"               cannot be combined with -n\n"
"\n"
"This program allows you to save and load snapshots of the PCI config space\n"
"By default, this program saves almost all config space addresses, excluding\n"
"  ones that are known to be read-only like IDs and capability pointers.\n"
"The byte offsets in -n and -y are rounded down to dwords.\n"
"The list parsing always assumes hex values and works without the 0x prefix.\n"
"This program restores BAR definitions (0x10-0x28) by default.\n"
"\n"
"Examples:\n"
"  Manage states of a device with stable extended config space behavior\n"
"  $ %s save -ed pci0:40:0:0 /tmp/gpu.state\n"
"  $ %s load -evd pci0:40:0:0 /tmp/gpu.state\n"
"\n"
"  Skip restoring BARs for a device where it causes issues\n"
"  $ %s load -vd pci0:5:0:0 -n 10,14,18,1C,20,24,30 ./device.state\n"
"\n"
"  Only restore one specific extended config space address\n"
"  $ %s load -evd pci0:31:0:0 -y 216 ./device.state\n"
"",
			name, name, name, name);
	}

	exit(EXIT_FAILURE);
}


/*
 * Bitmask access functions
 *
 * Get, Set and Clear a bit at position i
 */

static inline int
mask_get(
	uint8_t *mask,
	const unsigned int i
	)
{
	return ((mask[i / 8] >> (i % 8)) & 1);
}

static inline void
mask_set(
	uint8_t *mask,
	const unsigned int i
	)
{
	mask[i / 8] |= (uint8_t)(1u << (i % 8));
}

static inline void
mask_clr(
	uint8_t *mask,
	const unsigned int i
	)
{
	mask[i / 8] &= (uint8_t)~(1u << (i % 8));
}

/*
 * Build the default config space bitmask
 * Input is a 128 byte array
 */
static void
mask_set_defaults(
	uint8_t *mask
	)
{
	memset(mask, 0xFF, CFG_EXT_MASK_SZ);

	for (size_t i = 0; i < sizeof(cfg_ro_dwords) /
		sizeof(cfg_ro_dwords[0]); i++) {

		mask_clr(mask, cfg_ro_dwords[i] / 4);
	}
}

/*
 * Free program context members
 */
static void
prog_free(
	struct prog_ctx *prog
	)
{
	LOG("Freeing program context");

        if (prog->fp != NULL) {
		fclose(prog->fp);
		prog->fp = NULL;
        }

	if (prog->nolist != NULL) {
		free(prog->nolist);
		prog->nolist = NULL;
	}

	if (prog->yeslist != NULL) {
		free(prog->yeslist);
		prog->yeslist = NULL;
	}
}

/*
 * Parse a comma separated hex value list
 *
 * Returns:
 *   Pointer to list - Success
 *   NULL - Failure
 *
 * Argument outlen is an address to a variable that will be set to
 *   parsed list length.
 */
static uint16_t *
parse_csx(
	char *list,
	size_t *outlen
	)
{
	char *cc = list;
	int nums = 1;

	/* Validate beginning */
	if (*cc == '\0') {
		LOG("Empty list");
		return (NULL);
	} else if (*cc == ',') {
		LOG("Early comma");
		return (NULL);
	} else if (!isxdigit((unsigned char)*cc)) {
		LOGV("Not a hex digit: '%c'", *cc);
		return (NULL);
	}

	/* Walk first time to check the correctness */
	while ((cc = strchr(cc + 1, ',')) != NULL) {
		if (cc[1] == '\0') {
			LOG("Trailing comma");
			return (NULL);
		} else if (!isxdigit((unsigned char)cc[1])) {
			LOGV("Not a hex digit: '%c'", cc[1]);
			return (NULL);
		}
		nums++;
	}

	uint16_t *xlist = calloc(nums, sizeof(uint16_t));
	if (xlist == NULL) {
		LOG("calloc() failed");
		return (NULL);
	}

	/* Walk second time to fill in the array */
	nums = 0;
	cc = list;
	while (1) {
		errno = 0;
		unsigned long val = strtoul(cc, &cc, 16);
		if (errno) {
			LOG("strtoul() failed");
			free(xlist);
			return (NULL);
		} else if (val > UINT16_MAX) {
			LOGV("Value out of range: %lu", val);
			free(xlist);
			return (NULL);
		}
		xlist[nums++] = (uint16_t)val;

		if (*cc == '\0') {
			break;
		} else if (*cc != ',') {
			LOGV("char != ',': '%c'", *cc);
			free(xlist);
			return (NULL);
		}

		/* Skip the comma */
		cc++;
	}

	*outlen = (size_t)nums;
	return (xlist);
}

/*
 * Open the state file
 *
 * Returns:
 *   0 - Success
 *  -1 - Failure
 *
 * If mode is SAVE, create a file if it doesn't exist
 */
static int
state_open(
	struct prog_ctx *prog,
	char *path
	)
{
	struct stat s;

	if (stat(path, &s) == 0) {
		if (!S_ISREG(s.st_mode)) {
			fprintf(stderr, "%s: Savestate is not a regular file\n",
				prog->name);
			return (-1);
		}
	} else if (errno == ENOENT) {
		if (prog->mode == MODE_SAVE) {
			/* fopen() creates a new file */
			errno = 0;
		} else {
			fprintf(stderr, "%s: Savestate does not exist\n",
				prog->name);
			return (-1);
		}
	} else {
		perror("stat()");
		return (-1);
	}

	if (prog->mode == MODE_SAVE) {
		LOGV("Opening '%s' for writing", path);
		prog->fp = fopen(path, "wb");
	} else {
		LOGV("Opening '%s' for reading", path);
		prog->fp = fopen(path, "rb");
	}

	if (prog->fp == NULL) {
		perror("fopen()");
		return (-1);
	}

	return (0);
}

/*
 * Save state from device to file
 *
 * Returns:
 *   0 - Success
 *  -1 - Failure
 */
static int
state_save(
	struct prog_ctx *prog,
	struct nyetpci_ctx *pci
	)
{
	if (state_open(prog, prog->a_savefile) < 0) {
		return (-1);
	}

	struct pci_save state = {0};

	memcpy(state.magic, "\x09SAV", 4);
	state.version = HDR_VERSION;

	if (	(nyetpci_get_vendor(pci, &state.vendor) < 0) ||
		(nyetpci_get_device(pci, &state.device) < 0) ||
		(nyetpci_get_subvendor(pci, &state.subvendor) < 0) ||
		(nyetpci_get_subdevice(pci, &state.subdevice) < 0)) {

		return (-1);
	}

	if (prog->flag_validate) {
		fprintf(stderr, "%s: Warning: validate flag ignored on save\n",
			prog->name);
	}

	if (prog->yeslist) {
		for (size_t i = 0; i < prog->yeslist_len; i++) {
			mask_set(state.config_mask, prog->yeslist[i] / 4);
		}
	} else {
		mask_set_defaults(state.config_mask);
		if (prog->nolist) {
			for (size_t i = 0; i < prog->nolist_len; i++) {
				mask_clr(state.config_mask, prog->nolist[i] / 4);
			}
		}
	}

	/* Read regular config space */
	for (size_t i = 0; i < CFG_SZ; i += 4) {
		uint32_t dw;
		if (nyetpci_cfg_read(pci, i, 4, &dw) < 0) {
			fprintf(stderr, "%s: Could not read config space at "
				"0x%zX\n", prog->name, i);
			return (-1);
		}
		memcpy(state.config + i, &dw, sizeof(uint32_t));
	}

	if (prog->flag_ext) {
		for (size_t i = CFG_SZ; i < CFG_EXT_SZ; i += 4) {
			uint32_t dw;
			if (nyetpci_cfg_read(pci, i, 4, &dw) < 0) {
				fprintf(stderr, "Only saving regular"
					"config space\n");
				errno = 0;
				goto noext;
			}
			memcpy(state.config + i, &dw, sizeof(uint32_t));
		}
	} else {
noext:
		memset(state.config_mask + CFG_MASK_SZ, 0,
			CFG_EXT_MASK_SZ - CFG_MASK_SZ);
	}

	/* Checksum */
	uint32_t crc = UINT32_MAX;
	for (uint32_t i = 0; i < sizeof(struct pci_save); i++) {
		crc = crc32_step(crc, ((uint8_t *)&state)[i]);
	}
	crc ^= UINT32_MAX;
	state.checksum = crc;

	if (fwrite(&state, sizeof(struct pci_save), 1, prog->fp) != 1) {
		fprintf(stderr, "%s: Could not write header to savefile\n",
			prog->name);
		return (-1);
	}

	return (0);
}

/*
 * Load state from file to device
 *
 * Returns:
 *   0 - Success
 *  -1 - Failure
 */
static int
state_load(
	struct prog_ctx *prog,
	struct nyetpci_ctx *pci
	)
{
	if (state_open(prog, prog->a_savefile) < 0) {
		return (-1);
	}

	struct pci_save state;

	if (fread(&state, sizeof(struct pci_save), 1, prog->fp) != 1) {
		fprintf(stderr, "%s: Could not read savefile\n", prog->name);
		return (-1);
	}

	if (memcmp(state.magic, "\x09SAV", 4) != 0) {
		fprintf(stderr, "%s: Wrong savefile magic\n", prog->name);
		return (-1);
	}

	if (state.version > HDR_VERSION) {
		fprintf(stderr, "%s: Savefile format too new\n", prog->name);
		return (-1);
	} else if (state.version < HDR_VERSION) {
		fprintf(stderr, "%s: Outdated savefile format\n", prog->name);
		return (-1);
	}

	uint32_t crc = UINT32_MAX;
	for (uint32_t i = 0; i < sizeof(struct pci_save); i++) {
		crc = crc32_step(crc, ((uint8_t *)&state)[i]);
	}
	crc ^= UINT32_MAX;

	if (state.checksum != crc) {
		fprintf(stderr, "%s: Invalid savefile checksum\n", prog->name);
		return (-1);
	}

	uint16_t vendor, device, subvendor, subdevice;

	if (	(nyetpci_get_vendor(pci, &vendor) < 0) ||
		(nyetpci_get_device(pci, &device) < 0) ||
		(nyetpci_get_subvendor(pci, &subvendor) < 0) ||
		(nyetpci_get_subdevice(pci, &subdevice) < 0)) {

		return (-1);
	}

	if (state.vendor != vendor || state.device != device) {
		fprintf(stderr, "%s: Savefile PCI vendor/device do not match\n",
			prog->name);
		return (-1);
	} else if (state.subvendor != subvendor ||
		state.subdevice != subdevice) {

		fprintf(stderr, "%s: Warning: Savefile PCI subvendor/subdevice "
			"do not match\n", prog->name);
	}

	/* Parse the lists */
	if (prog->yeslist) {
		memset(state.config_mask, 0, CFG_EXT_MASK_SZ);
		for (size_t i = 0; i < prog->yeslist_len; i++) {
			mask_set(state.config_mask, prog->yeslist[i] / 4);
		}
	} else if (prog->nolist) {
		for (size_t i = 0; i < prog->nolist_len; i++) {
			mask_clr(state.config_mask, prog->nolist[i] / 4);
		}
	}

	/* dwords */
	size_t end = 0;
	if (prog->flag_ext) {
		end = CFG_EXT_SZ;
		LOG("Writing extended config space");
	} else {
		end = CFG_SZ;
		LOG("Writing regular config space");
	}

	for (size_t i = 0; i < end; i += 4) {
		if (!mask_get(state.config_mask, i / 4)) {
			continue;
		}

		uint32_t dw;
		memcpy(&dw, state.config + i, sizeof(uint32_t));
		if (nyetpci_cfg_write(pci, i, 4, &dw) < 0) {
			fprintf(stderr, "%s: Could not write config space at "
				"0x%zX\n", prog->name, i);
		}
	}

	if (prog->flag_validate) {
		int perfect = 1;
		LOG("Validating the loaded config space");
		for (size_t i = 0; i < end; i += 4) {
			uint32_t dw_cfg,
				 dw_pci;

			if (!mask_get(state.config_mask, i)) {
				continue;
			}

			if (nyetpci_cfg_read(pci, i, 4, &dw_pci) < 0) {
				fprintf(stderr, "%s: Could not read config "
					"space at 0x%zX\n", prog->name, i);
				return (-1);
			}

			memcpy(&dw_cfg, state.config + i, sizeof(uint32_t));

			if (dw_cfg != dw_pci) {
				fprintf(stderr, "Dword at offset 0x%zX did "
					"not stick! (save:0x%08X pci:0x%08X)\n",
					i, dw_cfg, dw_pci);
				perfect = 0;
			}
		}

		if (perfect) {
			fprintf(stderr, "Every dword loaded successfully!\n");
		}
	}

	return (0);
}

/*
 * Parse program arguments to fill in prog_ctx and pci_ctx.sel
 *
 * Returns:
 *   0 - Success
 *  -1 - Failure
 */
static int
parse_prog_args(
	struct prog_ctx *prog,
	struct nyetpci_ctx *pci,
	int argc,
	char *argv[]
	)
{
	if (argc < 2) {
		usage(prog->name, 0);
	}

	if (strcmp(argv[1], "-h") == 0) {
		usage(prog->name, 1);
	}

	if (strcmp(argv[1], "save") == 0) {
		prog->mode = MODE_SAVE;
	} else if (strcmp(argv[1], "load") == 0) {
		prog->mode = MODE_LOAD;
	} else {
		fprintf(stderr, "%s: Invalid mode\n", prog->name);
		usage(prog->name, 0);
	}

	optind = 2;

	int ch;
	while ((ch = getopt(argc, argv, "hved:n:y:")) != -1) {
		switch (ch) {
		case 'h':
			usage(prog->name, 1);
			break;

		case 'v':
			prog->flag_validate = 1;
			break;

		case 'e':
			prog->flag_ext = 1;
			break;

		case 'd':
			prog->a_device = optarg;
			break;

		case 'y':
			prog->a_yeslist = optarg;
			break;

		case 'n':
			prog->a_nolist = optarg;
			break;

		default:
			usage(prog->name, 0);
		}
	}

	if (optind >= argc) {
		usage(prog->name, 0);
	}

	prog->a_savefile = argv[optind++];

	if (prog->a_device == NULL) {
		fprintf(stderr, "%s: Device not specified\n", prog->name);
		usage(prog->name, 0);
	}

	if (optind != argc) {
		fprintf(stderr, "%s: Trailing arguments after savefile\n",
			prog->name);
		usage(prog->name, 0);
	}

	/*
	 * Validate arguments
	 */

	if (prog->a_savefile == NULL) {
		fprintf(stderr, "%s: Savefile not specified\n",
			prog->name);
		usage(prog->name, 0);
	} else if (prog->a_yeslist != NULL && prog->a_nolist != NULL) {
		fprintf(stderr, "%s: Cannot use both yeslist and nolist\n",
			prog->name);
		usage(prog->name, 0);
	}

	/* Parse PCI selector */
	if (nyetpci_parse_sel(prog->a_device, &pci->sel) < 0) {
		fprintf(stderr, "%s: Invalid device selector\n", prog->name);
		return (-1);
	}

	/* Parse yeslist */
	if (prog->a_yeslist != NULL) {
		if ((prog->yeslist = parse_csx(prog->a_yeslist,
			&prog->yeslist_len)) == NULL) {

			fprintf(stderr, "%s: Invalid yeslist string\n",
				prog->name);
			return (-1);
		}
	}

	for (size_t i = 0; i < prog->yeslist_len; i++) {
		if (prog->yeslist[i] >= CFG_EXT_SZ) {
			fprintf(stderr, "%s: Yeslist entry %zu overflows "
					"extended config space\n",
					prog->name, i);
			return (-1);
		} else if (prog->yeslist[i] >= CFG_SZ && !prog->flag_ext) {
			fprintf(stderr, "%s Yeslist entry %zu is in extended "
					"config space, yet -e was not passed\n",
					prog->name, i);
			return (-1);
		}
	}

	/* Parse nolist */
	if (prog->a_nolist != NULL) {
		if ((prog->nolist = parse_csx(prog->a_nolist,
			&prog->nolist_len)) == NULL) {

			fprintf(stderr, "%s: Invalid nolist string\n",
				prog->name);
			return (-1);
		}
	}

	for (size_t i = 0; i < prog->nolist_len; i++) {
		if (prog->nolist[i] >= CFG_EXT_SZ) {
			fprintf(stderr, "%s: Nolist entry %zu overflows cfg\n",
				prog->name, i);
			return (-1);
		}
	}

	return (0);
}

/*
 * Main
 */
int
main(
	int argc,
	char *argv[]
	)
{
	struct nyetpci_ctx pci = {0};
	struct prog_ctx prog = {0};
	int ret = EXIT_FAILURE;

	prog.name = argv[0];
	prog.mode = MODE_INVALID;

	if (parse_prog_args(&prog, &pci, argc, argv) < 0) {
		goto exit;
	}

	if (nyetpci_init(&pci) < 0) {
		perror("nyetpci_init()");
		goto exit;
	}

	if (prog.mode == MODE_LOAD) {
		if (state_load(&prog, &pci) < 0) {
			goto exit;
		}
	} else if (prog.mode == MODE_SAVE) {
		if (state_save(&prog, &pci) < 0) {
			goto exit;
		}
	} else {
		fprintf(stderr, "%s: Mode not set (This should not happen!)\n",
			prog.name);
		goto exit;
	}

	ret = EXIT_SUCCESS;
exit:
	(void)nyetpci_free(&pci);
	prog_free(&prog);
	return (ret);
}
