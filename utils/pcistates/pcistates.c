/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Alexey Laurentsyeu <alex@paidbsd.org>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 * 	notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 * 	notice, this list of conditions and the following disclaimer in the
 * 	documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holder nor the names of its
 * 	contributors may be used to endorse or promote products derived from
 * 	this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
 * TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * pcistates.c - Save and restore PCI config space states
 */

#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/pciio.h>

#include <fcntl.h>
#include <errno.h>
#include <ctype.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

/*
 * Macros
 */

#if defined(DEBUG)
#  define LOG(STR) fprintf(stderr, "Log @ %s:%d:%s: "STR"\n", \
	__FILE__, __LINE__, __func__)
#  define LOGV(FMT, ...) fprintf(stderr, "Log @ %s:%d:%s: "FMT"\n", \
	__FILE__, __LINE__, __func__, __VA_ARGS__)
#else
#  define LOG(STR) (void)0
#  define LOGV(FMT, ...) (void)0
#endif

#define CFG_SZ		(256)
#define CFG_MASK_SZ	(CFG_SZ / 8 / 4)
#define CFG_EXT_SZ	(4096)
#define CFG_EXT_MASK_SZ	(CFG_EXT_SZ / 8 / 4)

/*
 * Structs
 */

/* Context */
struct pci_ctx {
	/* FD to /dev/pci */
	int fd;

	/* PCI device selector */
	struct pcisel sel;

	/* Generic IO */
	struct pci_conf_io pc;
};

struct prog_ctx {
	/* Executable name */
	char *name;

	/* State file pointer */
	FILE *state_fp;

	/* Operation mode (MODE_INVALID / MODE_STORE / MODE_LOAD) */
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

/* Savefile header */
#define HDR_VERSION 2
#pragma pack(push, 1)
struct pci_save {
	/* 0; 0x9 + "SAV" */
	char magic[4];

	/* 4; Header version */
	uint16_t version;

	/* 6; PCI vendor ID */
	uint16_t vendor;

	/* 8; PCI device ID */
	uint16_t device;

	/* 10; PCI subsystem vendor ID */
	uint16_t subvendor;

	/* 12; PCI subsystem device ID */
	uint16_t subdevice;

	/* 14; Extended config space mask */
	uint8_t config_mask[CFG_EXT_MASK_SZ];

	/* 142; Extended config space */
	uint8_t config[CFG_EXT_SZ];

	/* 4238 */
};
#pragma pack(pop)

enum { MODE_INVALID, MODE_STORE, MODE_LOAD };

/*
 * Variables
 */

static const uint16_t cfg_ro_dwords[] = {
	/* IDs */
	0x00, 0x08,

	/* CLS; LT; HT; BIST */
	0x0C,

	/* CardBus CIS pointer */
	0x28,

	/* Subsystem IDs */
	0x2C,

	/* Capabilities; Reserved */
	0x34, 0x38,

	/* IL; IP; Timer */
	0x3C
};

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
	int full
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
"  $ %s load -vd pci0:5:0:0 -n 10,14,18,1C,20,24 ./device.state\n"
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
	unsigned int i
	)
{
	return ((mask[i / 8] >> (i % 8)) & 1);
}

static inline void
mask_set(
	uint8_t *mask,
	unsigned int i
	)
{
	mask[i / 8] |= (uint8_t)(1u << (i % 8));
}

static inline void
mask_clr(
	uint8_t *mask,
	unsigned int i
	)
{
	mask[i / 8] &= (uint8_t)~(1u << (i % 8));
}

/*
 * Build the default config space bitmask
 */
static void
mask_set_defaults(
	/* 128 byte array (4096 / 8 / 4) */
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
 * Free PCI context members
 */
static void
free_pci_ctx(
	struct pci_ctx *pci
	)
{
	LOG("Cleaning context");

	if (pci->fd > -1) {
		close(pci->fd);
		pci->fd = -1;
	}

	if (pci->pc.patterns != NULL) {
		free(pci->pc.patterns);
		pci->pc.patterns = NULL;
	}

	if (pci->pc.matches != NULL) {
		free(pci->pc.matches);
		pci->pc.matches = NULL;
	}
}

/*
 * Free program context members
 */
static void
free_prog_ctx(
	struct prog_ctx *prog
	)
{
        if (prog->state_fp != NULL) {
		fclose(prog->state_fp);
		prog->state_fp = NULL;
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
 * Parse a PCI selector
 * Adapted from /usr/src/usr.sbin/pciconf/pciconf.c
 *
 * Returns:
 *   0 - Success
 *  -1 - Failure
 */
static int
pci_parse_sel(
	const char *str,
	struct pcisel *sel
	)
{
	const char *ep;
	char *eppos;
	unsigned long selarr[4];
	int i;

	ep = strchr(str, '@');
	if (ep != NULL) {
		ep++;
	} else {
		ep = str;
	}

	if (strncmp(ep, "pci", 3) == 0) {
		ep += 3;
		i = 0;
		while (isdigit((unsigned char)*ep) && i < 4) {
			selarr[i++] = strtoul(ep, &eppos, 10);
			ep = eppos;
			if (*ep == ':') {
				ep++;
			}
		}
		if (i > 0 && *ep == '\0') {
			sel->pc_func = (i > 2) ? selarr[--i] : 0;
			sel->pc_dev = (i > 0) ? selarr[--i] : 0;
			sel->pc_bus = (i > 0) ? selarr[--i] : 0;
			sel->pc_domain = (i > 0) ? selarr[--i] : 0;
			return (0);
		}
	}

	return (-1);
}

/*
 * Read a dword from config space
 *
 * Returns:
 *   0 - Success
 *  -1 - Failure
 */
static int
pci_cfg_read_dword(
	struct pci_ctx *pci,
	uint32_t *dw,
	size_t dwi
	)
{
	struct pci_io io = {0};

	io.pi_sel = pci->sel;
	io.pi_reg = dwi * 4;
	io.pi_width = 4;

	if (ioctl(pci->fd, PCIOCREAD, &io) < 0) {
		perror("ioctl(PCIOCREAD)");
		return (-1);
	}

	*dw = io.pi_data;

	return (0);
}

/*
 * Write a dword to config space
 *
 * Returns:
 *   0 - Success
 *  -1 - Failure
 */
static int
pci_cfg_write_dword(
	struct pci_ctx *pci,
	uint32_t dw,
	size_t dwi
	)
{
	struct pci_io io = {0};

	io.pi_sel = pci->sel;
	io.pi_reg = dwi * 4;
	io.pi_width = 4;
	io.pi_data = dw;

	if (ioctl(pci->fd, PCIOCWRITE, &io) < 0) {
		perror("ioctl(PCIOCWRITE)");
		return (-1);
	}

	return (0);
}

/*
 * Open a PCI device from a selector and fill in pci_ctx
 *
 * Returns:
 *   0 - Success
 *  -1 - Failure
 */
static int
pci_open(
	struct prog_ctx *prog,
	struct pci_ctx *pci
	)
{
	LOGV("Opening PCI device pci%u:%u:%u:%u", pci->sel.pc_domain,
		pci->sel.pc_bus, pci->sel.pc_dev, pci->sel.pc_func);
	struct pci_conf_io *pc = &pci->pc;

	pc->num_patterns = 1;
	pc->pat_buf_len = pc->num_patterns * sizeof(struct pci_match_conf);
	pc->patterns = calloc(pc->num_patterns, sizeof(struct pci_match_conf));
	if (pc->patterns == NULL) {
		perror("calloc()");
		return (-1);
	}
	pc->patterns->pc_sel = pci->sel;
	pc->patterns->flags = PCI_GETCONF_MATCH_DOMAIN | PCI_GETCONF_MATCH_BUS
		| PCI_GETCONF_MATCH_DEV | PCI_GETCONF_MATCH_FUNC;

	pc->match_buf_len = 1 * sizeof(struct pci_conf);
	pc->matches = calloc(1, sizeof(struct pci_conf));
	if (pc->matches == NULL) {
		perror("calloc()");
		return (-1);
	}

	pci->fd = open("/dev/pci", O_RDWR);
	if (pci->fd == -1) {
		perror("open(/dev/pci)");
		return (-1);
	}

do_ioctl:
	if (ioctl(pci->fd, PCIOCGETCONF, pc) < 0) {
		perror("ioctl(PCIOCGETCONF)");
		return (-1);
	}

	if (pc->status == PCI_GETCONF_ERROR) {
		LOGV("PCIOCGETCONF status = %d", pc->status);
		fprintf(stderr, "%s: PCI_GETCONF_ERROR\n", prog->name);
		return (-1);
	} else if (pc->status == PCI_GETCONF_LIST_CHANGED) {
		/* loop back until the status clears */
		goto do_ioctl;
	}

	if (pc->num_matches == 0) {
		errno = 0;
		return (-1);
	}

	return (0);
}

/*
 * Parse program arguments to fill in prog_ctx and pci_ctx.sel
 *
 * Returns:
 *   0 - Success
 *  -1 - Failure
 *
 * Calls usage() on failure before any lists were allocated, after
 *   any malloc() calls returns -1 for main to correctly clean up
 */
static int
parse_prog_args(
	struct prog_ctx *prog,
	struct pci_ctx *pci,
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
		prog->mode = MODE_STORE;
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

	if (optind != argc) {
		fprintf(stderr, "%s: Trailing arguments after savefile\n",
			prog->name);
		usage(prog->name, 0);
	}

	/*
	 * Validate arguments
	 */

	if (prog->a_savefile == NULL || prog->a_device == NULL) {
		fprintf(stderr, "%s: Savefile or device not specified\n",
			prog->name);
		usage(prog->name, 0);
	} else if (prog->a_yeslist != NULL && prog->a_nolist != NULL) {
		fprintf(stderr, "%s: Cannot use both yeslist and nolist\n",
			prog->name);
		usage(prog->name, 0);
	}

	/* Parse PCI selector */
	if (pci_parse_sel(prog->a_device, &pci->sel) < 0) {
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
 * Open the state file
 *
 * Returns:
 *   0 - Success
 *  -1 - Failure
 *
 * If mode is STORE, create a file if it doesn't exist
 */
static int
pci_state_open(
	struct prog_ctx *prog,
	char *path,
	int mode
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
		if (mode == MODE_STORE) {
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

	if (mode == MODE_STORE) {
		LOGV("Opening '%s' for writing", path);
		prog->state_fp = fopen(path, "wb");
	} else {
		LOGV("Opening '%s' for reading", path);
		prog->state_fp = fopen(path, "rb");
	}

	if (prog->state_fp == NULL) {
		perror("fopen()");
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
pci_state_load(
	struct pci_ctx *pci,
	struct prog_ctx *prog
	)
{
	if (pci_state_open(prog, prog->a_savefile, prog->mode) < 0) {
		return (-1);
	}

	struct pci_save state;
	struct pci_conf *match = &pci->pc.matches[0];

	if (fread(&state, sizeof(struct pci_save), 1, prog->state_fp) != 1) {
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

	if (state.vendor != match->pc_vendor ||
		state.device != match->pc_device) {

		fprintf(stderr, "%s: Savefile PCI vendor/device do not match\n",
			prog->name);
		return (-1);
	} else if (state.subvendor != match->pc_subvendor ||
		state.subdevice != match->pc_subdevice) {

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
		end = CFG_EXT_SZ / 4;
		LOG("Writing extended config space");
	} else {
		end = CFG_SZ / 4;
		LOG("Writing regular config space");
	}

	for (size_t i = 0; i < end; i++) {
		if (!mask_get(state.config_mask, i)) {
			continue;
		}

		uint32_t dw;
		memcpy(&dw, state.config + i * 4, sizeof(uint32_t));
		if (pci_cfg_write_dword(pci, dw, i) < 0) {
			return (-1);
		}
	}

	if (prog->flag_validate) {
		int perfect = 1;
		LOG("Validating the loaded config space");
		for (size_t i = 0; i < end; i++) {
			uint32_t dw_cfg,
				 dw_pci;

			if (!mask_get(state.config_mask, i)) {
				continue;
			}

			if (pci_cfg_read_dword(pci, &dw_pci, i) < 0) {
				return (-1);
			}

			memcpy(&dw_cfg, state.config + i * 4, sizeof(uint32_t));

			if (dw_cfg != dw_pci) {
				fprintf(stderr, "%s: dword at offset 0x%zX did "
					"not stick! (save:0x%08X pci:0x%08X)\n",
					prog->name, i * 4, dw_cfg, dw_pci);
				perfect = 0;
			}
		}

		if (perfect) {
			fprintf(stderr,
				"%s: Every dword loaded successfully!\n",
				prog->name);
		}
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
pci_state_save(
	struct pci_ctx *pci,
	struct prog_ctx *prog
	)
{
	if (pci_state_open(prog, prog->a_savefile, prog->mode) < 0) {
		return (-1);
	}

	struct pci_save state = {0};
	struct pci_conf *match = &pci->pc.matches[0];

	memcpy(state.magic, "\x09SAV", 4);
	state.version = HDR_VERSION;
	state.vendor = match->pc_vendor;
	state.device = match->pc_device;
	state.subvendor = match->pc_subvendor;
	state.subdevice = match->pc_subdevice;

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
	for (size_t i = 0; i < CFG_SZ / 4; i++) {
		uint32_t dw;
		if (pci_cfg_read_dword(pci, &dw, i) < 0) {
			LOG("Error reading config space");
			return (-1);
		}
		memcpy(state.config + i * 4, &dw, sizeof(uint32_t));
	}

	if (prog->flag_ext) {
		for (size_t i = CFG_SZ / 4; i < CFG_EXT_SZ / 4; i++) {
			uint32_t dw;
			if (pci_cfg_read_dword(pci, &dw, i) < 0) {
				fprintf(stderr, "%s: Could not grab extended "
					"config, only saving regular "
					"config space\n",
					prog->name);
				errno = 0;
				goto noext;
			}
			memcpy(state.config + i * 4, &dw, sizeof(uint32_t));
		}
	} else {
noext:
		memset(state.config_mask + CFG_MASK_SZ, 0,
			CFG_EXT_MASK_SZ - CFG_MASK_SZ);
	}

	if (fwrite(&state, sizeof(struct pci_save), 1, prog->state_fp) != 1) {
		fprintf(stderr, "%s: Could not write header to savefile\n",
			prog->name);
		return (-1);
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
	struct prog_ctx prog = {0};
	prog.name = argv[0];
	prog.mode = MODE_INVALID;
	struct pci_ctx pci = {0};
	pci.fd = -1;

	if (parse_prog_args(&prog, &pci, argc, argv) < 0) {
		goto error;
	}

	errno = 0;
	if (pci_open(&prog, &pci) < 0) {
		if (!errno) {
			fprintf(stderr, "%s: No matching PCI device\n",
				prog.name);
		}
		goto error;
	}

	if (prog.mode == MODE_LOAD) {
		if (pci_state_load(&pci, &prog) < 0) {
			goto error;
		}
	} else {
		if (pci_state_save(&pci, &prog) < 0) {
			goto error;
		}
	}

	free_prog_ctx(&prog);
	free_pci_ctx(&pci);
	return (EXIT_SUCCESS);
error:
	LOGV("Hello, error handler! errno = %d", errno);
	free_prog_ctx(&prog);
	free_pci_ctx(&pci);
	return (EXIT_FAILURE);
}
