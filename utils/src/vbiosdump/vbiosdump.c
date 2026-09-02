/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Alexey Laurentsyeu <alex@paidbsd.org>
 */

#include <sys/stat.h>
#include <sys/mman.h>

#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

#include <debug.h>
#include <vbiosdump.h>
#include <nyetpci.h>

/*
 * Structs
 */

/* Program context */
struct prog_ctx {
	/* Executable name */
	char *name;

	/* GPU selector */
	char *a_device;

	/* VBIOS output file path */
	char *vbios_path;

	/* VBIOS output file pointer */
	FILE *vbios_fp;

	/* Are we outputing to stdout? */
	int flag_stdout;

	/* Operation BAR override */
	int opbar;

	/* VBIOS itself */
	uint8_t *vbios_data;
	size_t vbios_len;
};

/* Generic dumper context */
struct dctx_generic {
	/* BAR base */
	volatile void *base;
};

/*
 * Print usage
 *
 * Exits program on call
 */
static void
usage(
	const char *name,
	int full
	)
{
	fprintf(stderr, "usage: %s -d device [-b bar] [-u] vbios.rom\n", name);
	if (full) {
		fprintf(stderr,
"\n"
"Options:\n"
"  -d device  PCI selector (same form as pciconf(8), e.g. 'pci0:5:0:0')\n"
"  -b bar     Use the specified BAR for dumping instead of the default one\n"
"             If 0x30 is specified, use a (mostly) GPU-agnostic dump method\n"
"               through the expansion ROM BAR (requires /dev/mem access)\n"
"\n"
"This program automatically saves the VBIOS ROM of the selected GPU as a file\n"
"  ready to use for GPU passthrough.\n"
"There are some limitations like highly limited iGPU support, untested IFR\n"
"  parsing (Nvidia A100 datacenter accelerators w/o display engine) and\n"
"  reliance on /dev/mem for legacy AMD GPU ROM dumping support.\n"
"Currently, this tool is only available for FreeBSD.\n"
"\n"
"Examples:"
"  Dump the VBIOS of a GPU to stdout, driver name in selector\n"
"  $ %s -d vgapci0@pci0:1:0:0 -\n"
"\n"
"  Dump the VBIOS of a GPU using expansion ROM BAR\n"
"  $ %s -d pci0:1:0:0 -b 0x30 vbios.rom\n"
"\n"
"  Dump the VBIOS of a GPU using an MMIO BAR at 0x20, mmap through /dev/mem\n"
"  $ %s -d pci0:1:0:0 -b 0x20 -u vbios.rom\n"
"",
			name, name, name);
	}

	exit(EXIT_FAILURE);
}

static void
prog_free(
	struct prog_ctx *prog
	)
{
        if (prog->vbios_fp != NULL) {
		fclose(prog->vbios_fp);
		prog->vbios_fp = NULL;
        }

	if (prog->vbios_data) {
		free(prog->vbios_data);
		prog->vbios_data = NULL;
	}
}

/*
 * Validate and open a file for writing
 *
 * Returns:
 *   0 - Success
 *  -1 - Failure
 */
static int
open_file_write(
	struct prog_ctx *prog,
	char *path
	)
{
	struct stat s;

	if (stat(path, &s) == 0) {
		if (!S_ISREG(s.st_mode)) {
			fprintf(stderr, "%s: Output exists and is not a "
				"regular file\n", prog->name);
			return (-1);
		}
	} else if (errno == ENOENT) {
		errno = 0;
	} else {
		perror("stat()");
		return (-1);
	}

	LOGV("Opening '%s' for writing", path);
	prog->vbios_fp = fopen(path, "wb");

	if (prog->vbios_fp == NULL) {
		perror("fopen()");
		return (-1);
	}

	return (0);
}

/*
 * Mmap a physical address from /dev/mem
 *
 * Returns:
 *   Addr - Success
 *   NULL - Failure
 */
static void *
mmap_paddr(
	uint64_t paddr,
	uint64_t len,
	int rw
	)
{
	int memfd = open("/dev/mem", (rw ? O_RDWR : O_RDONLY));
	if (memfd < 0) {
		return (NULL);
	}

	void *base = mmap(NULL, len, (rw ? PROT_READ | PROT_WRITE : PROT_READ),
		MAP_SHARED | MAP_NOCORE, memfd, paddr);

	if (base == MAP_FAILED) {
		base = NULL;
	}

	close(memfd);
	return (base);
}

/*
 * Walk the ROM and dump it to prog->vbios_data provided a read function
 *
 * Returns:
 *   0 - Success
 *  -1 - Error
 */
static int
vbiosdump_walk_rom(
	struct prog_ctx *prog,
	uint32_t off_initial,
	void *dctx,
	void (*h_read)
		(
		void *dctx,
		void *data,
		size_t len,
		uint32_t off
		)
	)
{
	size_t alloc_size = 0;
	uint32_t off = off_initial;

	/* Scan headers and calculate image size */
	while (1) {
		struct oprom_hdr_efi hdr;
		struct oprom_pcir pcir;
		size_t image_bytes;

		if (off > off_initial) {
			fprintf(stderr, "Image split at 0x%04X\n", off);
		}

		if (off > MAX_VBIOS_SIZE) {
			fprintf(stderr, "%s: Image chain longer than max "
				"VBIOS size", prog->name);
			return (-1);
		}

		/* Grab header, use the EFI struct because it doesn't matter */
		h_read(dctx, &hdr, sizeof(struct oprom_hdr_efi), off);

		/* Verify magic number */
		if (hdr.magic[0] != 0x55 || hdr.magic[1] != 0xAA) {
			fprintf(stderr, "%s: OpROM magic missmatch "
				"(expected 0x55AA, got 0x%02X%02X)\n",
				prog->name, hdr.magic[0], hdr.magic[1]);

			return (-1);
		}

		/* Grab PCIR */
		h_read(dctx, &pcir, sizeof(struct oprom_pcir),
			off + hdr.pcir_off);

		if (memcmp(pcir.magic, "PCIR", 4) != 0) {
			fprintf(stderr, "%s: PCIR magic missmatch "
				"(expected 'PCIR', got '%c%c%c%c')\n",
				prog->name, pcir.magic[0], pcir.magic[1],
				pcir.magic[2], pcir.magic[3]);
			return (-1);
		}

		/* Sanity check */
		if (pcir.image_len == 0) {
			fprintf(stderr, "%s: PCIR reports zero image "
				"length, corrupt ROM?\n", prog->name);
			return (-1);
		}

		image_bytes = pcir.image_len * 512;

		/* Check for overflow */
		if (alloc_size + image_bytes > MAX_VBIOS_SIZE) {
			fprintf(stderr, "%s: VBIOS size exceeds set limit "
				"(%ub)\n", prog->name, MAX_VBIOS_SIZE);
			return (-1);
		}

		alloc_size += image_bytes;
		off = off_initial + alloc_size;

		if (pcir.image_is_last & 0x80) {
			break;
		}
	}

	/* Allocate memory and dump VBIOS */
	prog->vbios_data = malloc(alloc_size);
	if (prog->vbios_data == NULL) {
		perror("malloc()");
		return (-1);
	}

	prog->vbios_len = alloc_size;
	h_read(dctx, prog->vbios_data, alloc_size, off_initial);

	return (0);
}

/*
 * Read helper - Read from raw mmapped memory
 */
static void
h_raw_rom_read(
	void *dctx,
	void *data,
	size_t len,
	uint32_t off
	)
{
	volatile void *base = ((struct dctx_generic *)dctx)->base;
	uint32_t i = 0;

	/* Read most as 32-bit */
	while (i + 4 <= len) {
		uint32_t chunk = MMIO_R32(base, off + i);
		memcpy((uint8_t *)data + i, &chunk, sizeof(uint32_t));
		i += sizeof(uint32_t);
	}

	/* Finish off tail as a single OOB-not access */
	if (i < len) {
		uint32_t chunk = MMIO_R32(base, off + i);
		memcpy((uint8_t *)data + i, &chunk, len - i);
	}
}

/*
 * Generic dumper through expansion ROM BAR
 *
 * Returns:
 *   0 - Success
 *  -1 - Failure
 */
static int
vbiosdump_ebar(
	struct prog_ctx *prog,
	struct nyetpci_ctx *pci
	)
{
	LOG("Dumping through expansion ROM BAR");

	uint64_t bar_len;
	uint64_t bar_paddr = nyetpci_bar_get_paddr(pci, 0x30, &bar_len);
	if (bar_paddr == 0) {
		fprintf(stderr, "%s: No expansion ROM BAR\n", prog->name);
		return (-1);
	}

	uint8_t *bar_base = mmap_paddr(bar_paddr, bar_len, 1);
	if (bar_base == NULL) {
		perror("mmap_paddr()");
		fprintf(stderr, "%s: Could not mmap BAR\n", prog->name);
		return (-1);
	}

	struct dctx_generic dctx;
	dctx.base = bar_base;

	LOG("Walking ROM");
	if (vbiosdump_walk_rom(prog, 0, &dctx, h_raw_rom_read) < 0) {
		return (-1);
	}

	return (0);
}

/*
 * Stubs
 */

static int
vbiosdump_nvidia_main(
	struct prog_ctx *prog,
	struct nyetpci_ctx *pci
	)
{
	(void)prog;
	(void)pci;
	fprintf(stderr, "nvidia stub\n");

	return (-1);
}

static int
vbiosdump_amd_main(
	struct prog_ctx *prog,
	struct nyetpci_ctx *pci
	)
{
	(void)prog;
	(void)pci;
	fprintf(stderr, "amd stub\n");

	return (-1);
}

static int
vbiosdump_intel_main(
	struct prog_ctx *prog,
	struct nyetpci_ctx *pci
	)
{
	(void)prog;
	(void)pci;
	fprintf(stderr, "intel stub\n");

	return (-1);
}

/*
 * Hub for all vbiosdump functions
 *
 * Returns:
 *   0 - Success
 *  -1 - Failure
 */
static int
vbiosdump_main(
	struct prog_ctx *prog,
	struct nyetpci_ctx *pci
	)
{
	int r;

	/* Probe for an MMIO BAR */
	if (prog->opbar == 0) {
		int skip_next = 0;

		for (int bar = 0x10; bar <= 0x24; bar += 4) {
			uint32_t b;

			/* Skip for upper part of 64-bit BARs */
			if (skip_next) {
				skip_next = 0;
				continue;
			}

			if (nyetpci_cfg_read(pci, bar, 4, &b) < 0) {
				LOG("Could not read BAR");
				return (-1);
			}

			if (b == 0 || b == UINT32_MAX) {
				goto forend;
				LOG("BAR is null");
			}

			/* Must not be an IO port BAR */
			if (nyetpci_bar_is_ioport(b)) {
				goto forend;
				LOG("BAR is in IO port space");
			}

			/* Skip upper part of 64-bit BARs */
			if (nyetpci_bar_is_64bit(b)) {
				skip_next = 1;
			}

			/* Must not be a prefetchable BAR */
			if (!nyetpci_bar_is_prefetchable(b)) {
				prog->opbar = bar;
				break;
			}
			LOG("BAR is Prefetchable");
forend:
			fprintf(stderr, "BAR 0x%02X not MMIO, trying "
				"next one\n", bar);
		}
	}

	/* If none was chosen */
	if (prog->opbar == 0) {
		fprintf(stderr, "Trying BAR 0x30 as last resort\n");
		prog->opbar = 0x30;
	}

	/* Don't check vendor for 0x30 */
	if (prog->opbar == 0x30) {
		return (vbiosdump_ebar(prog, pci));
	}

	uint16_t vendor;
	if (nyetpci_get_vendor(pci, &vendor) < 0) {
		return (-1);
	}

	switch (vendor) {
	case 0x10DE:
		/* NVIDIA*/
		r = vbiosdump_nvidia_main(prog, pci);
		break;

	case 0x1002:
		/* AMD */
		r = vbiosdump_amd_main(prog, pci);
		break;

	case 0x8086:
		/* Intel */
		r = vbiosdump_intel_main(prog, pci);
		break;

	default:
		fprintf(stderr, "%s: Unknown GPU vendor: 0x%04X\n",
			prog->name, vendor);
		return (-1);
	}

	return (r);

#undef DEFAULT_OPBAR
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

	int ch;
	while ((ch = getopt(argc, argv, "hb:d:")) != -1) {
		switch (ch) {
		case 'h':
			usage(prog->name, 1);
			break;

		case 'b':
			errno = 0;
			char *ep;
			prog->opbar = strtoul(optarg, &ep, 16);
			if (errno) {
				fprintf(stderr, "%s: Invalid BAR: '%s'\n",
					prog->name, optarg);
				return (-1);
			} else if (prog->opbar < 0x10 ||
				(prog->opbar > 0x24 && prog->opbar != 0x30)) {

				fprintf(stderr, "%s: Provided BAR is "
					"out of range, must be 0x10-0x24 or "
					"0x30\n", prog->name);
				return (-1);
			} else if (prog->opbar % 4 != 0) {
				fprintf(stderr, "%s: Provided BAR is not "
					"aligned, try 0x%02X\n", prog->name,
					prog->opbar - prog->opbar % 4);
				return (-1);
			}

			break;

		case 'd':
			prog->a_device = optarg;
			break;

		default:
			usage(prog->name, 0);
		}
	}

	/* Fallback to stdout */
	if (optind >= argc) {
		fprintf(stderr, "%s: No output specified\n", prog->name);
		return (-1);
	} else {
		prog->vbios_path = argv[optind++];
	}

	/* Check if we're outputing to stdout */
	if (strcmp(prog->vbios_path, "-") == 0) {
		LOG("Using stdout for VBIOS output");
		prog->flag_stdout = 1;
	}

	if (optind != argc) {
		fprintf(stderr, "%s: Trailing arguments after VBIOS\n",
			prog->name);
		usage(prog->name, 0);
	} else if (prog->a_device == NULL) {
		fprintf(stderr, "%s: No device specified\n", prog->name);
		usage(prog->name, 0);
	}

	if (nyetpci_parse_sel(prog->a_device, &pci->sel) < 0) {
		fprintf(stderr, "%s: Invalid device selector\n", prog->name);
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
	struct nyetpci_ctx pci = {0};
	struct prog_ctx prog = {0};
	int ret = EXIT_FAILURE;

	prog.name = argv[0];

	if (parse_prog_args(&prog, &pci, argc, argv) < 0) {
		goto exit;
	}

	if (nyetpci_init(&pci) < 0) {
		perror("nyetpci_init()");
		goto exit;
	}

	if (!nyetpci_device_exists(&pci)) {
		fprintf(stderr, "%s: Device does not exist\n", prog.name);
		goto exit;
	}

	int r;
	if ((r = nyetpci_is_gpu(&pci)) < 1) {
		if (r == 0) {
			fprintf(stderr, "%s: Not a GPU\n", prog.name);
		} else {
			perror("nyetpci_is_gpu()");
		}
		goto exit;
	}

	if (prog.opbar == 0x30) {
		LOG("Using expansion ROM BAR");
		if (vbiosdump_ebar(&prog, &pci) < 0) {
			goto exit;
		}
	} else {
		LOG("Probing GPU");
		if (vbiosdump_main(&prog, &pci) < 0) {
			goto exit;
		}
	}

	if (prog.vbios_data == NULL) {
		fprintf(stderr, "%s: VBIOS never got fetched\n",
			prog.name);
		goto exit;
	}

	if (prog.flag_stdout) {
		prog.vbios_fp = stdout;
	} else if (open_file_write(&prog, prog.vbios_path) < 0) {
		goto exit;
	}

	LOG("Writing");
	if (fwrite(prog.vbios_data, prog.vbios_len, 1, prog.vbios_fp) != 1) {
		fprintf(stderr, "%s: Error writing output\n", prog.name);
		goto exit;
	}

	/* Flush and check for error again */
	LOG("Flushing output");
	fflush(prog.vbios_fp);
	if (ferror(prog.vbios_fp)) {
		fprintf(stderr, "%s: Error writing output\n", prog.name);
		goto exit;
	}

	fprintf(stderr, "VBIOS dumped successfully!\n");

	ret = EXIT_SUCCESS;
exit:
	(void)nyetpci_free(&pci);
	prog_free(&prog);
	return (ret);
}
