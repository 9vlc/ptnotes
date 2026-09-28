/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Alexey Laurentsyeu <alex@paidbsd.org>
 */

/*
 * pcisave.c - Save and restore PCI config space states
 */

#include <sys/stat.h>

#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <crc32.h>
#include <pcisave.h>
#include <nyetpci.h>

/*
 * Structs
 */

struct prog_ctx {
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
 * If full = 1, print full usage instructions
 */
static void
usage(
	const int full
	)
{
	const char *name = getprogname();

	fprintf(stderr,
"usage: %s -h\n"
"       %s save -d device [-e] [-n off,..] [-y off,..] savefile\n"
"       %s load -d device [-e] [-v] [-n off,..] [-y off,..] savefile\n"
"",
		name, name, name);

	if (full) {
		fprintf(stderr,
"\n"
"Actions:\n"
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
 * Get, set and clear a bit at position i
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
		return (NULL);
	} else if (*cc == ',') {
		return (NULL);
	} else if (!isxdigit((unsigned char)*cc)) {
		return (NULL);
	}

	/* Walk first time to check the correctness */
	while ((cc = strchr(cc + 1, ',')) != NULL) {
		if (cc[1] == '\0') {
			return (NULL);
		} else if (!isxdigit((unsigned char)cc[1])) {
			return (NULL);
		}
		nums++;
	}

	uint16_t *xlist = calloc(nums, sizeof(uint16_t));
	if (xlist == NULL) {
		warn("calloc");
		return (NULL);
	}

	/* Walk second time to fill in the array */
	nums = 0;
	cc = list;
	while (1) {
		errno = 0;
		unsigned long val = strtoul(cc, &cc, 16);
		if (errno) {
			warn("strtoul");
			free(xlist);
			return (NULL);
		} else if (val > UINT16_MAX) {
			free(xlist);
			return (NULL);
		}
		xlist[nums++] = (uint16_t)val;

		if (*cc == '\0') {
			break;
		} else if (*cc != ',') {
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
 * Compute the CRC32 of a savefile state
 *
 * The checksum field must be zeroed by the caller
 */
static uint32_t
state_crc32(
	const struct pci_save *state
	)
{
	const uint8_t *p = (const uint8_t *)state;
	uint32_t crc = UINT32_MAX;

	for (size_t i = 0; i < sizeof(struct pci_save); i++) {
		crc = crc32_step(crc, p[i]);
	}

	return (crc ^ UINT32_MAX);
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
			warnx("%s: not a regular file", path);
			return (-1);
		}
	/* ENOENT is fine on save since fopen() creates a file */
	} else if (errno != ENOENT || prog->mode != MODE_SAVE) {
		warn("%s", path);
		return (-1);
	}

	prog->fp = fopen(path, prog->mode == MODE_SAVE ? "wb" : "rb");
	if (prog->fp == NULL) {
		warn("%s", path);
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

		warnx("could not read PCI IDs from device");
		return (-1);
	}

	if (prog->flag_validate) {
		warnx("-v ignored on save");
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
			warnx("could not read config space at 0x%zX", i);
			return (-1);
		}
		memcpy(state.config + i, &dw, sizeof(uint32_t));
	}

	if (prog->flag_ext) {
		for (size_t i = CFG_SZ; i < CFG_EXT_SZ; i += 4) {
			uint32_t dw;
			if (nyetpci_cfg_read(pci, i, 4, &dw) < 0) {
				warnx("could not read extended config space at "
					"0x%zX, only saving regular config "
					"space", i);
				goto noext;
			}
			memcpy(state.config + i, &dw, sizeof(uint32_t));
		}
	} else {
noext:
		memset(state.config_mask + CFG_MASK_SZ, 0,
			CFG_EXT_MASK_SZ - CFG_MASK_SZ);
	}

	state.checksum = 0;
	state.checksum = state_crc32(&state);

	if (fwrite(&state, sizeof(struct pci_save), 1, prog->fp) != 1) {
		warn("%s", prog->a_savefile);
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
		if (ferror(prog->fp)) {
			warn("%s", prog->a_savefile);
		} else {
			warnx("%s: truncated savefile", prog->a_savefile);
		}
		return (-1);
	}

	if (memcmp(state.magic, "\x09SAV", 4) != 0) {
		warnx("wrong savefile magic");
		return (-1);
	}

	if (state.version > HDR_VERSION) {
		warnx("savefile format too new");
		return (-1);
	} else if (state.version < HDR_VERSION) {
		warnx("savefile format too old");
		return (-1);
	}

	uint32_t state_crc = state.checksum;
	state.checksum = 0;

	if (state_crc32(&state) != state_crc) {
		warnx("invalid savefile checksum");
		return (-1);
	}

	uint16_t vendor, device, subvendor, subdevice;

	if (	(nyetpci_get_vendor(pci, &vendor) < 0) ||
		(nyetpci_get_device(pci, &device) < 0) ||
		(nyetpci_get_subvendor(pci, &subvendor) < 0) ||
		(nyetpci_get_subdevice(pci, &subdevice) < 0)) {

		warnx("could not read PCI IDs from device");
		return (-1);
	}

	if (state.vendor != vendor || state.device != device) {
		warnx("savefile PCI vendor/device do not match");
		return (-1);
	} else if (state.subvendor != subvendor ||
		state.subdevice != subdevice) {

		warnx("savefile PCI subvendor/subdevice do not match");
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
	size_t end = prog->flag_ext ? CFG_EXT_SZ : CFG_SZ;

	for (size_t i = 0; i < end; i += 4) {
		if (!mask_get(state.config_mask, i / 4)) {
			continue;
		}

		uint32_t dw;
		memcpy(&dw, state.config + i, sizeof(uint32_t));
		if (nyetpci_cfg_write(pci, i, 4, &dw) < 0) {
			warnx("could not write config space at 0x%zX", i);
		}
	}

	if (prog->flag_validate) {
		int perfect = 1;
		for (size_t i = 0; i < end; i += 4) {
			uint32_t dw_cfg,
				 dw_pci;

			if (!mask_get(state.config_mask, i / 4)) {
				continue;
			}

			if (nyetpci_cfg_read(pci, i, 4, &dw_pci) < 0) {
				warnx("could not read config space at 0x%zX",
					i);
				return (-1);
			}

			memcpy(&dw_cfg, state.config + i, sizeof(uint32_t));

			if (dw_cfg != dw_pci) {
				fprintf(stderr, "Dword at offset 0x%zX did not "
					"stick! (save:0x%08X pci:0x%08X)\n",
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
		usage(0);
	}

	if (strcmp(argv[1], "-h") == 0) {
		usage(1);
	}

	if (strcmp(argv[1], "save") == 0) {
		prog->mode = MODE_SAVE;
	} else if (strcmp(argv[1], "load") == 0) {
		prog->mode = MODE_LOAD;
	} else {
		warnx("invalid mode: %s", argv[1]);
		usage(0);
	}

	optind = 2;

	int ch;
	while ((ch = getopt(argc, argv, "hved:n:y:")) != -1) {
		switch (ch) {
		case 'h':
			usage(1);
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
			usage(0);
		}
	}

	if (optind >= argc) {
		usage(0);
	}

	prog->a_savefile = argv[optind++];

	if (prog->a_device == NULL) {
		warnx("device not specified");
		usage(0);
	}

	if (optind != argc) {
		warnx("trailing arguments after savefile");
		usage(0);
	}

	/*
	 * Validate arguments
	 */

	if (prog->a_yeslist != NULL && prog->a_nolist != NULL) {
		warnx("-y and -n are mutually exclusive");
		usage(0);
	}

	/* Parse PCI selector */
	if (nyetpci_parse_sel(prog->a_device, &pci->sel) < 0) {
		warnx("invalid device selector: %s", prog->a_device);
		return (-1);
	}

	/* Parse yeslist */
	if (prog->a_yeslist != NULL) {
		if ((prog->yeslist = parse_csx(prog->a_yeslist,
			&prog->yeslist_len)) == NULL) {

			warnx("invalid -y list: %s", prog->a_yeslist);
			return (-1);
		}
	}

	for (size_t i = 0; i < prog->yeslist_len; i++) {
		if (prog->yeslist[i] >= CFG_EXT_SZ) {
			warnx("-y offset 0x%X is outside extended config space",
				prog->yeslist[i]);
			return (-1);
		} else if (prog->yeslist[i] >= CFG_SZ && !prog->flag_ext) {
			warnx("-y offset 0x%X is in extended config space, "
				"but -e was not passed", prog->yeslist[i]);
			return (-1);
		}
	}

	/* Parse nolist */
	if (prog->a_nolist != NULL) {
		if ((prog->nolist = parse_csx(prog->a_nolist,
			&prog->nolist_len)) == NULL) {

			warnx("invalid -n list: %s", prog->a_nolist);
			return (-1);
		}
	}

	for (size_t i = 0; i < prog->nolist_len; i++) {
		if (prog->nolist[i] >= CFG_EXT_SZ) {
			warnx("-n offset 0x%X is outside extended config space",
				prog->nolist[i]);
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

	if (parse_prog_args(&prog, &pci, argc, argv) < 0) {
		goto exit;
	}

	if (nyetpci_init(&pci) < 0) {
		warnx("nyetpci_init failed");
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
		warnx("mode not set");
		goto exit;
	}

	ret = EXIT_SUCCESS;
exit:
	(void)nyetpci_free(&pci);
	prog_free(&prog);
	return (ret);
}
