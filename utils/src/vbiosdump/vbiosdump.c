/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Alexey Laurentsyeu <alex@paidbsd.org>
 *
 * This is a tool for dumping GPU's VBIOS for passthrough.
 * The produced images in a lot of the cases are stripped.
 * DO NOT USE THIS TOOL FOR ROM BACKUPS!
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

	/* Are we doing a full chain dump ? */
	int flag_fullchain;

	/* Operation BAR override */
	int opbar;

	/* VBIOS itself */
	uint8_t *vbios_data;
	size_t vbios_len;
};

/* Generic dumper context */
struct dctx_generic {
	/* BAR base */
	void *base;

	/* BAR length */
	size_t len;
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
	fprintf(stderr, "usage: %s -d device [-b bar] [-a] vbios.rom\n", name);
	if (full) {
		fprintf(stderr,
"\n"
"Options:\n"
"  -a         Dump the full image chain including stubs\n"
"  -d device  PCI selector (same form as pciconf(8), e.g. 'pci0:5:0:0')\n"
"  -b bar     Use the specified BAR for dumping instead of the default one\n"
"             If 0x30 is specified, use a (mostly) GPU-agnostic dump method\n"
"               through the expansion ROM BAR\n"
"\n"
"This program automatically saves the VBIOS ROM of the selected GPU as a file\n"
"  ready to use for GPU passthrough.\n"
"There are some limitations like highly limited iGPU support.\n"
"\n"
"Examples:"
"  Dump the VBIOS of a GPU to stdout\n"
"  $ %s -d pci0:1:0:0 -\n"
"\n"
"  Dump the VBIOS of a GPU using expansion ROM BAR\n"
"  $ %s -d pci0:1:0:0 -b 0x30 vbios.rom\n"
"",
			name, name);
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
	size_t len,
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
 * Is [off, off + len) inside readable memory?
 */
static int
rom_in_range(
	uint64_t bound,
	uint64_t off,
	size_t len
	)
{
	if (bound == 0) {
		return (1);
	}

	return ((off <= bound) && (len <= (bound - off)));
}

/*
 * (Helper) Read from raw mmapped memory
 *
 * Reads cannot go over dctx.length
 * Cannot fully read a mapping that's not aligned to uint32_t
 */
static void
h_raw_rom_read(
	void *dctx,
	void *data,
	size_t data_len,
	uint32_t off
	)
{
	volatile void *base = ((struct dctx_generic *)dctx)->base;
	size_t len = ((struct dctx_generic *)dctx)->len;
	uint32_t i = 0;

	/* Read most in 32-bit chunks */
	while ((i + 4 <= data_len) && (off + i + 4 <= len)) {
		uint32_t chunk = MMIO_R32(base, off + i);
		memcpy((uint8_t *)data + i, &chunk, sizeof(uint32_t));
		i += sizeof(uint32_t);
	}

	/* Finish off the tail */
	if ((i < data_len) && (off + i + 4 <= len)) {
		uint32_t chunk = MMIO_R32(base, off + i);
		memcpy((uint8_t *)data + i, &chunk, data_len - i);
		i = (uint32_t)data_len;
	}

	/* Clean everything OOB */
	if (i < data_len) {
		LOGV("Got 0x%02zX OOB bytes", data_len - i);
		memset((uint8_t *)data + i, 0xFF, data_len - i);
	}
}

/*
 * (Helper) Read AMDGPU (SOC15+) ROM
 */
static void
h_amd_rom_read(
	void *dctx,
	void *data,
	size_t data_len,
	uint32_t off
	)
{
	volatile void *base = ((struct dctx_generic *)dctx)->base;
	uint32_t i = 0;

	/* "Hi, I would like to begin reading from here." */
	MMIO_W32(base, IOREG_AMD_ROM_INDEX, off);

	/* Read most in 32-bit chunks */
	while (i + 4 <= data_len) {
		uint32_t chunk = MMIO_R32(base, IOREG_AMD_ROM_DATA);
		memcpy((uint8_t *)data + i, &chunk, sizeof(uint32_t));
		i += sizeof(uint32_t);
	}

	/* Finish off the tail */
	if (i < data_len) {
		uint32_t chunk = MMIO_R32(base, IOREG_AMD_ROM_DATA);
		memcpy((uint8_t *)data + i, &chunk, data_len - i);
	}
}

/*
 * (Helper) Initialize Intel dGPU ROM and find the OpROM region
 */
static void
h_intel_rom_init(
	volatile void *base,
	uint32_t *off
	)
{
	LOG("Initializing Intel SPI");

	/* Find region with VBIOS */
	uint32_t region = MMIO_R32(base, IOREG_INTEL_GET_REGION) &
		MASK_INTEL_ROM_REGION;

	/* Select it */
	MMIO_W32(base, IOREG_INTEL_SEL_REGION, region);

	/* Offset to VBIOS start in that region */
	*off = MMIO_R32(base, IOREG_INTEL_GET_ROM_OFF) & MASK_INTEL_ROM_OFFSET;
}

/*
 * (Helper) Read Intel dGPU ROM
 */
static void
h_intel_rom_read(
	void *dctx,
	void *data,
	size_t data_len,
	uint32_t off
	)
{
	volatile void *base = ((struct dctx_generic *)dctx)->base;
	uint32_t i = 0;

	/* Read most in 32-bit chunks */
	while (i + 4 <= data_len) {
		/* Ask for an address */
		MMIO_W32(base, IOREG_INTEL_SEL_ADDR, off + i);

		/* Read the return */
		uint32_t chunk = MMIO_R32(base, IOREG_INTEL_READ_ADDR);

		memcpy((uint8_t *)data + i, &chunk, sizeof(uint32_t));
		i += sizeof(uint32_t);
	}

	/* Finish off the tail */
	if (i < data_len) {
		MMIO_W32(base, IOREG_INTEL_SEL_ADDR, off + i);
		uint32_t chunk = MMIO_R32(base, IOREG_INTEL_READ_ADDR);
		memcpy((uint8_t *)data + i, &chunk, data_len - i);
	}
}

/*
 * Validate one OpROM image at the offset and grab its PCIR
 *
 * Returns:
 *   0 - Success, PCIR filled in
 *  -1 - Error
 */
static int
vbiosdump_walk_image(
	void *dctx,
	void (*h_read) (
		void *dctx,
		void *data,
		size_t data_len,
		uint32_t off
		),
	uint32_t bound,
	uint32_t off,
	struct oprom_pcir *pcir,
	int fullchain
	)
{
	uint16_t pcir_off;
	uint32_t hdr_size;

	uint8_t peek[8]; /* magic + size + efi magic */

	if (!rom_in_range(bound, off, sizeof(peek))) {
		fprintf(stderr, "OpROM header: Outside mapped region\n");
		return (-1);
	}
	h_read(dctx, peek, sizeof(peek), off);

	if ((peek[0] != 0x55) || (peek[1] != 0xAA)) {
		fprintf(stderr, "OpROM header: Invalid magic\n");
		return (-1);
	}

	/*
	 * Dumb split since EFI size entry covers the first
	 *   legacy init vector byte
	 */
	if ((peek[4] == 0xF1) && (peek[5] == 0x0E) && (peek[6] == 0x00) &&
		(peek[7] == 0x00)) {

		struct oprom_hdr_efi hdr;

		if (!rom_in_range(bound, off, sizeof(hdr))) {
			fprintf(stderr, "OpROM header: Outside mapped "
				"region\n");
			return (-1);
		}
		h_read(dctx, &hdr, sizeof(hdr), off);

		if (hdr.pcir_off == 0) {
			fprintf(stderr, "OpROM header: Invalid PCIR offset\n");
			return (-1);
		}

		pcir_off = hdr.pcir_off;
		hdr_size = hdr.size;
	} else {
		struct oprom_hdr_legacy hdr;

		if (!rom_in_range(bound, off, sizeof(hdr))) {
			fprintf(stderr, "OpROM header: Outside mapped "
				"region\n");
			return (-1);
		}
		h_read(dctx, &hdr, sizeof(hdr), off);

		if (hdr.pcir_off == 0) {
			fprintf(stderr, "OpROM header: Invalid PCIR offset: "
				"0x%04X\n", hdr.pcir_off);
			return (-1);
		}

		if ((hdr.init_vector[0] == 0x00) &&
			(hdr.init_vector[1] == 0x00) &&
			(hdr.init_vector[2] == 0x00)) {

			LOG("OpROM header: Legacy image without init vector, "
				"stub?");

			if (!fullchain) {
				return (-1);
			}
		}

		pcir_off = hdr.pcir_off;
		hdr_size = hdr.size;
	}

	/*
	 * Technically a good idea, but Nvidia's newer images have OpROM's size
	 *   field zeroed out but still point to a PCIR, which contains a
	 *   proper size field.
	 */
	/*if ((size_t)pcir_off + sizeof(struct oprom_pcir) >
		(size_t)hdr_size * 512) {

		fprintf(stderr, "OpROM header: PCIR outside the image "
			"bounds (offset 0x%04X)\n", pcir_off);
		return (-1);
	}*/

	if (!rom_in_range(bound, (uint64_t)off + pcir_off,
		sizeof(struct oprom_pcir))) {

		fprintf(stderr, "OpROM header: PCIR outside mapped region\n");
		return (-1);
	}
	h_read(dctx, pcir, sizeof(*pcir), off + pcir_off);

	if (memcmp(pcir->magic, "PCIR", 4) != 0) {
		fprintf(stderr, "PCIR header: Invalid magic\n");
		return (-1);
	}

	if (pcir->image_len == 0) {
		fprintf(stderr, "PCIR header: Zero image length\n");
		return (-1);
	}

	if (hdr_size != pcir->image_len) {
		LOGV("Image size mismatch between header (%u) and PCIR (%u), "
			"using PCIR", hdr_size, pcir->image_len);
	}

	return (0);
}

/*
 * Walk the ROM and dump it to prog->vbios_data provided a read function
 *
 * bound is a maximum, pass 0 if the reader only uses fixed registers
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
	uint32_t bound,
	void (*h_read) (
		void *dctx,
		void *data,
		size_t data_len,
		uint32_t off
		)
	)
{
	size_t alloc_size = 0;
	uint32_t off = off_initial;

	/* Scan headers and calculate image size */
	while (1) {
		struct oprom_pcir pcir;
		size_t image_bytes;

		if (off > off_initial) {
			fprintf(stderr, "Image split at 0x%04X\n", off);
		}

		if (vbiosdump_walk_image(dctx, h_read, bound, off,
			&pcir, prog->flag_fullchain) < 0) {

			/* The first chain entry has to be valid */
			if (alloc_size == 0) {
				return (-1);
			}

			/* Failed later - padding? */
			uint32_t scan;
			int found = 0;

			for (scan = off + 0x100;
				scan < off_initial + MAX_VBIOS_SIZE;
				scan += 0x100) {

				uint8_t m[2];
				if (!rom_in_range(bound, scan,
					sizeof(m))) {
					break;
				}

				h_read(dctx, m, sizeof(m), scan);
				if ((m[0] == 0x55) && (m[1] == 0xAA)) {
					off = scan;
					found = 1;
					break;
				}
			}

			if (!found) {
				fprintf(stderr, "OpROM chain broke at 0x%04X "
					"without last image indicator\n", off);
				break;
			}
			alloc_size = (size_t)(off - off_initial);

			continue;
		}

		image_bytes = (size_t)pcir.image_len * 512;

		/* Check for overflow */
		if (alloc_size + image_bytes > MAX_VBIOS_SIZE) {
			fprintf(stderr, "VBIOS size exceeds set limit "
				"(%ub)\n", MAX_VBIOS_SIZE);
			return (-1);
		}

		alloc_size += image_bytes;
		off = off_initial + (uint32_t)alloc_size;

		if (pcir.indicator & 0x80) {
			break;
		}
	}

	/* Allocate memory and dump VBIOS */
	prog->vbios_data = malloc(alloc_size);
	if (prog->vbios_data == NULL) {
		perror("malloc()");
		return (-1);
	}

	if (!rom_in_range(bound, off_initial, alloc_size)) {
		fprintf(stderr, "VBIOS goes outside the allowed region\n");
		free(prog->vbios_data);
		prog->vbios_data = NULL;
		return (-1);
	}

	prog->vbios_len = alloc_size;
	h_read(dctx, prog->vbios_data, alloc_size, off_initial);

	return (0);
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
	struct dctx_generic dctx = {0};
	int ret = -1;

	uint64_t bar_paddr = nyetpci_bar_get_paddr(pci, 0x30, &dctx.len);
	if (bar_paddr == 0) {
		fprintf(stderr, "No BAR\n");
		goto exit;
	}

	dctx.base = mmap_paddr(bar_paddr, dctx.len, 1);
	if (dctx.base == NULL) {
		perror("mmap_paddr()");
		fprintf(stderr, "%s: Could not mmap BAR\n", prog->name);
		return (-1);
	}

	uint32_t bound = (dctx.len > UINT32_MAX ?
		UINT32_MAX : (uint32_t)dctx.len);

	LOG("Walking ROM");
	if (vbiosdump_walk_rom(prog, 0, &dctx, bound, h_raw_rom_read) < 0) {
		goto exit;
	}

	ret = 0;
exit:
	if ((dctx.base != NULL) && (munmap(dctx.base, dctx.len) < 0)) {
		fprintf(stderr, "%s: Failure munmapping BAR\n", prog->name);
		return (-1);
	}

	return (ret);
}

/*
 * Dumper for Nvidia
 *
 * Returns:
 *   0 - Success
 *  -1 - Failure
 */
static int
vbiosdump_nvidia_main(
	struct prog_ctx *prog,
	struct nyetpci_ctx *pci
	)
{
	LOG("Dumping for NVIDIA");
	struct dctx_generic dctx = {0};

	uint64_t bar_len;
	uint64_t bar_paddr = nyetpci_bar_get_paddr(pci, prog->opbar, &bar_len);
	if (bar_paddr == 0) {
		fprintf(stderr, "%s: No BAR\n", prog->name);
		goto ebar;
	} else if (bar_len > UINT32_MAX) {
		fprintf(stderr, "Warning: BAR larger than 4 GiB\n");
		bar_len = UINT32_MAX;
	}

	dctx.len = bar_len;

	if (dctx.len - 4 < IOREG_NVIDIA_SPI_OFF) {
		fprintf(stderr, "BAR too small\n");
		goto ebar;
	}

	dctx.base = mmap_paddr(bar_paddr, bar_len, 1);
	if (dctx.base == NULL) {
		perror("mmap_paddr()");
		fprintf(stderr, "%s: Could not mmap BAR\n", prog->name);
		return (-1);
	}

	fprintf(stderr, "Scanning ROM...\n");
	int success = 0;

	for (uint32_t o = IOREG_NVIDIA_SPI_OFF;
		o < IOREG_NVIDIA_SPI_OFF + IOREG_NVIDIA_SPI_SIZE;
		o += 0x100) {

		if (MMIO_R8(dctx.base, o + 0) == 0x55 &&
			MMIO_R8(dctx.base, o + 1) == 0xAA) {

			fprintf(stderr, "Trying 0x%X\n", o);
			if (vbiosdump_walk_rom(prog, o, &dctx,
				IOREG_NVIDIA_SPI_OFF + IOREG_NVIDIA_SPI_SIZE,
				h_raw_rom_read) < 0) {

				continue;
			}

			success = 1;
			break;
		}
	}

	if (munmap(dctx.base, dctx.len) < 0) {
		fprintf(stderr, "%s: Failure munmapping BAR\n", prog->name);
		return (-1);
	}

	if (success) {
		return (0);
	}

ebar:
	fprintf(stderr, "Trying expansion ROM BAR as fallback\n");
	return (vbiosdump_ebar(prog, pci));
}

/*
 * Dumper for AMD (SOC15+)
 *
 * Returns:
 *   0 - Success
 *  -1 - Failure
 */
static int
vbiosdump_amd_main(
	struct prog_ctx *prog,
	struct nyetpci_ctx *pci
	)
{
	LOG("Dumping for AMD");
	struct dctx_generic dctx = {0};

	uint64_t bar_len;
	uint64_t bar_paddr = nyetpci_bar_get_paddr(pci, prog->opbar, &bar_len);
	if (bar_paddr == 0) {
		fprintf(stderr, "%s: No BAR\n", prog->name);
		goto ebar;
	} else if (bar_len > UINT32_MAX) {
		fprintf(stderr, "Warning: BAR larger than 4 GiB\n");
		bar_len = UINT32_MAX;
	}

	/* Check if the BAR is too small for the access regsistrers */
	if (bar_len - 4 < IOREG_AMD_ROM_DATA) {
		fprintf(stderr, "BAR too small\n");
		goto ebar;
	}

	dctx.len = bar_len;
	dctx.base = mmap_paddr(bar_paddr, bar_len, 1);
	if (dctx.base == NULL) {
		perror("mmap_paddr()");
		fprintf(stderr, "%s: Could not mmap BAR\n", prog->name);
		return (-1);
	}

	fprintf(stderr, "Scanning ROM...\n");
	int success = 0;

	/* Limit to 512 KiB */
	for (uint32_t o = 0; o < 0x80000; o += 0x100) {
		uint8_t sig[2];
		h_amd_rom_read(&dctx, sig, 2, o);

		if (sig[0] == 0x55 && sig[1] == 0xAA) {
			fprintf(stderr, "Trying 0x%X\n", o);
			if (vbiosdump_walk_rom(prog, o, &dctx, 0,
				h_amd_rom_read) < 0) {

				continue;
			}

			success = 1;
			break;
		}
	}

	if (munmap(dctx.base, dctx.len) < 0) {
		fprintf(stderr, "%s: Failure munmapping BAR\n", prog->name);
		return (-1);
	}

	if (success) {
		return (0);
	}

ebar:
	fprintf(stderr, "Trying expansion ROM BAR as fallback\n");
	return (vbiosdump_ebar(prog, pci));
}

/*
 * Dumper for Intel (ARC) dGPUs
 *
 * Returns:
 *   0 - Success
 *  -1 - Failure
 */
static int
vbiosdump_intel_main(
	struct prog_ctx *prog,
	struct nyetpci_ctx *pci
	)
{
	LOG("Dumping for Intel");
	struct dctx_generic dctx = {0};

	uint64_t bar_len;
	uint64_t bar_paddr = nyetpci_bar_get_paddr(pci, prog->opbar, &bar_len);
	if (bar_paddr == 0) {
		fprintf(stderr, "%s: No BAR\n", prog->name);
		goto ebar;
	} else if (bar_len > UINT32_MAX) {
		fprintf(stderr, "Warning: BAR larger than 4 GiB\n");
		bar_len = UINT32_MAX;
	}

	/* Check if the BAR is too small for the access regsistrers */
	if (bar_len - 4 < IOREG_INTEL_GET_ROM_OFF) {
		fprintf(stderr, "BAR too small\n");
		goto ebar;
	}

	dctx.len = bar_len;
	dctx.base = mmap_paddr(bar_paddr, bar_len, 1);
	if (dctx.base == NULL) {
		perror("mmap_paddr()");
		fprintf(stderr, "%s: Could not mmap BAR\n", prog->name);
		return (-1);
	}

	uint32_t off;
	h_intel_rom_init(dctx.base, &off);
	LOGV("Offset to VBIOS: 0x%X", off);

	fprintf(stderr, "Scanning ROM...\n");
	int success = 0;

	/* Limit to 512 KiB */
	for (uint32_t o = off; o < off + 0x80000; o += 0x100) {
		uint8_t sig[2];
		h_intel_rom_read(&dctx, sig, 2, o);

		if (sig[0] == 0x55 && sig[1] == 0xAA) {
			fprintf(stderr, "Trying 0x%X\n", o);
			if (vbiosdump_walk_rom(prog, o, &dctx, 0,
				h_intel_rom_read) < 0) {

				continue;
			}

			success = 1;
			break;
		}
	}

	if (munmap(dctx.base, dctx.len) < 0) {
		fprintf(stderr, "%s: Failure munmapping BAR\n", prog->name);
		return (-1);
	}

	if (success) {
		return (0);
	}

ebar:
	fprintf(stderr, "Trying expansion ROM BAR as fallback\n");
	return (vbiosdump_ebar(prog, pci));
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
			fprintf(stderr, "BAR 0x%02X not fitting, trying "
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
	while ((ch = getopt(argc, argv, "hab:d:")) != -1) {
		switch (ch) {
		case 'h':
			usage(prog->name, 1);
			break;

		case 'a':
			prog->flag_fullchain = 1;
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
		fprintf(stderr, "%s: VBIOS never got fetched?\n",
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
	if (ret == EXIT_FAILURE) {
		fprintf(stderr, "%s: Failed to dump VBIOS\n", prog.name);
	}

	(void)nyetpci_free(&pci);
	prog_free(&prog);
	return (ret);
}
