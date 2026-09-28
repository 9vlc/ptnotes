/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Alexey Laurentsyeu <alex@paidbsd.org>
 *
 * This is a tool for dumping GPU's VBIOS for passthrough.
 * The produced images in a lot of the cases are stripped.
 * DO NOT USE THIS TOOL FOR ROM BACKUPS!
 */

#include <sys/stat.h>
#include <sys/mman.h>

#include <err.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

#include <mmio.h>
#include <oprom.h>
#include <nyetpci.h>

#define MAX_VBIOS_SIZE 1024 * 1024 * 10

/* Intel dGPU MMIO registers */
#define IOREG_INTEL_GET_REGION	0x102090 /* Get ROM regions */
#define IOREG_INTEL_SEL_REGION	0x102084 /* Select ROM region */
#define IOREG_INTEL_GET_ROM_OFF	0x1020C0 /* Get OpROM offset in region */
#define IOREG_INTEL_SEL_ADDR	0x102080 /* Offset to read */
#define IOREG_INTEL_READ_ADDR	0x102040 /* Returned read DWORD */

/* Intel masks */
#define MASK_INTEL_ROM_REGION	0xFF
#define MASK_INTEL_ROM_OFFSET	0x1F0000

/* AMD GPU MMIO registers for SOC15+ */
#define IOREG_AMD_ROM_INDEX	0x5A0A0 /* Select ROM index */
#define IOREG_AMD_ROM_DATA	0x5A0A4 /* Read ROM */

/* Nvidia GPU MMIO SPI offset; OpROM not always in the beginning */
#define IOREG_NVIDIA_SPI_OFF	0x300000
#define IOREG_NVIDIA_SPI_SIZE	0x100000

static int vflag = 0;

#define LOG(STR) do { \
	if (vflag) { \
		fprintf(stderr, STR"\n"); \
	} \
} while (0)

#define LOGV(FMT, ...) do { \
	if (vflag) { \
		fprintf(stderr, FMT"\n", __VA_ARGS__); \
	} \
} while (0)

/* Program context */
struct prog_ctx {
	/* GPU selector */
	const char *a_device;

	/* ROM to trim path */
	const char *a_trim_path;

	/* VBIOS output file path */
	const char *vbios_path;

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

/* For extracting VBIOS from a dirty ROM file */
struct dctx_file {
	FILE *fp;
	size_t len;
};

/*
 * Print usage
 *
 * Exits program on call
 */
static void
usage(
	int full
	)
{
	const char *name = getprogname();

	fprintf(stderr, "usage: %s -d device [-v] [-b bar] [-a] vbios.rom\n"
			"       %s -r dirty.rom [-v] [-a] vbios.rom\n",
		name, name);
	if (full) {
		fprintf(stderr,
"\n"
"Options:\n"
"  -a          Dump the full image chain including stubs\n"
"  -d device   PCI selector (same form as pciconf(8), e.g. 'pci0:5:0:0')\n"
"  -r bad.rom  Read from a dirty ROM file and extract VBIOS out of it\n"
"  -b bar      Use the specified BAR for dumping instead of the default one\n"
"              If 0x30 is specified, use a (mostly) GPU-agnostic dump method\n"
"                through the expansion ROM BAR\n"
"\n"
"This program automatically saves the VBIOS ROM of the selected GPU as a file\n"
"  ready to use for GPU passthrough.\n"
"There are some limitations like highly limited iGPU support.\n"
"\n"
"Examples:\n"
"  Dump the VBIOS of a GPU to stdout\n"
"  $ %s -d pci0:1:0:0 -\n"
"\n"
"  Dump the VBIOS of a GPU using expansion ROM BAR\n"
"  $ %s -d pci0:1:0:0 -b 0x30 vbios.rom\n"
"\n"
"  Carve out the first found VBIOS from a dirty ROM dump file\n"
"  $ %s -r dirty.rom clean.rom\n"
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
	const char *path
	)
{
	struct stat s;

	if (stat(path, &s) == 0) {
		if (!S_ISREG(s.st_mode)) {
			warnx("%s: not a regular file", path);
			return (-1);
		}
	} else if (errno != ENOENT) {
		warn("%s", path);
		return (-1);
	}

	prog->vbios_fp = fopen(path, "wb");

	if (prog->vbios_fp == NULL) {
		warn("fopen");
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
		MAP_SHARED, memfd, paddr);

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
 * (Helper) Read from an open file descriptor
 */
static void
h_file_rom_read(
	void *dctx,
	void *data,
	size_t data_len,
	uint32_t off
	)
{
	FILE *fp = ((struct dctx_file *)dctx)->fp;
	size_t len = ((struct dctx_file *)dctx)->len;

	/* No way of reporting error back to walker, discard it */

	if (fseek(fp, off, SEEK_SET) < 0) {
		return;
	} else if (data_len > len) {
		data_len = len;
	}
	(void)fread(data, data_len, 1, fp);
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
		LOG("OPROM header: outside mapped region");
		return (-1);
	}
	h_read(dctx, peek, sizeof(peek), off);

	if ((peek[0] != 0x55) || (peek[1] != 0xAA)) {
		LOG("OPROM header: invalid magic");
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
			LOG("OPROM header: outside mapped region");
			return (-1);
		}
		h_read(dctx, &hdr, sizeof(hdr), off);

		if (hdr.pcir_off == 0) {
			LOG("OPROM header: invalid PCIR offset");
			return (-1);
		}

		pcir_off = hdr.pcir_off;
		hdr_size = hdr.size;
	} else {
		struct oprom_hdr_legacy hdr;

		if (!rom_in_range(bound, off, sizeof(hdr))) {
			LOG("OPROM header: outside mapped region");
			return (-1);
		}
		h_read(dctx, &hdr, sizeof(hdr), off);

		if (hdr.pcir_off == 0) {
			LOGV("OPROM header: invalid PCIR offset: 0x%04X",
				hdr.pcir_off);
			return (-1);
		}

		if ((hdr.init_vector[0] == 0x00) &&
			(hdr.init_vector[1] == 0x00) &&
			(hdr.init_vector[2] == 0x00)) {

			LOG("OPROM header: legacy image without init vector, "
				"stub?");

			if (!fullchain) {
				return (-1);
			}
		}

		pcir_off = hdr.pcir_off;
		hdr_size = hdr.size;
	}

	if (!rom_in_range(bound, (uint64_t)off + pcir_off,
		sizeof(struct oprom_pcir))) {

		LOG("OPROM header: PCIR outside mapped region");
		return (-1);
	}
	h_read(dctx, pcir, sizeof(*pcir), off + pcir_off);

	if (memcmp(pcir->magic, "PCIR", 4) != 0) {
		LOG("PCIR header: invalid magic");
		return (-1);
	}

	if (pcir->image_len == 0) {
		LOG("PCIR header: zero image length");
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
			LOGV("Image split at 0x%04X", off);
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
				warnx("OPROM chain broke at 0x%04X without "
					"last image indicator", off);
				break;
			}
			alloc_size = (size_t)(off - off_initial);

			continue;
		}

		image_bytes = (size_t)pcir.image_len * 512;

		/* Check for overflow */
		if (alloc_size + image_bytes > MAX_VBIOS_SIZE) {
			warnx("VBIOS size exceeds set limit (%ub)",
				MAX_VBIOS_SIZE);
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
		warnx("VBIOS not in range");
		free(prog->vbios_data);
		prog->vbios_data = NULL;
		return (-1);
	}

	prog->vbios_len = alloc_size;
	h_read(dctx, prog->vbios_data, alloc_size, off_initial);

	return (0);
}

/*
 * Generic dumper through a file @ a_trim_path
 *
 * Returns:
 *   0 - Success
 *  -1 - Failure
 */
static int
vbiosdump_file(
	struct prog_ctx *prog
	)
{
	LOG("Dumping from file");
	struct dctx_file dctx = {0};
	int ret = -1;

	struct stat s;

	if (stat(prog->a_trim_path, &s) == 0) {
		if (!S_ISREG(s.st_mode)) {
			warnx("%s: not a regular file", prog->a_trim_path);
			goto exit;
		}
	} else {
		warn("%s", prog->a_trim_path);
		goto exit;
	}

	if ((dctx.fp = fopen(prog->a_trim_path, "rb")) == NULL) {
		warn("fopen");
		goto exit;
	}

	if (fseek(dctx.fp, 0, SEEK_END) < 0) {
		warn("fseek");
		goto exit;
	}
	dctx.len = ftell(dctx.fp);
	rewind(dctx.fp);

	if (dctx.len > UINT32_MAX) {
		warnx("%s: file larger than 4 GiB", prog->a_trim_path);
		goto exit;
	}

	/* Walk the entire file byte by byte, there might not be alignment */
	for (uint32_t o = 0; o < dctx.len; o ++) {
		uint8_t sig[2];
		h_file_rom_read(&dctx, sig, 2, o);

		if (sig[0] == 0x55 && sig[1] == 0xAA) {
			LOGV("Trying 0x%X", o);
			if (vbiosdump_walk_rom(prog, o, &dctx, dctx.len,
				h_file_rom_read) < 0) {

				continue;
			}

			ret = 0;
			break;
		}
	}

exit:
	if (dctx.fp != NULL) {
		if (fclose(dctx.fp) < 0) {
			warn("fclose");
			return (-1);
		}
	}
	return (ret);
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
		warnx("BAR 0x30 is invalid");
		return (-1);
	} else if (dctx.len > UINT32_MAX) {
		warnx("BAR 0x30 cannot be larger than 4 GiB");
		return (-1);
	}

	dctx.base = mmap_paddr(bar_paddr, dctx.len, 1);
	if (dctx.base == NULL) {
		warn("mmap BAR 0x30");
		return (-1);
	}

	if (vbiosdump_walk_rom(prog, 0, &dctx, dctx.len, h_raw_rom_read) < 0) {
		goto exit;
	}

	ret = 0;
exit:
	if (dctx.base != NULL && munmap(dctx.base, dctx.len) < 0) {
		warn("munmap BAR 0x%02X", prog->opbar);
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
		warnx("BAR 0x%02X is invalid", prog->opbar);
		goto ebar;
	} else if (bar_len > UINT32_MAX) {
		warnx("BAR 0x%02X larger than 4 GiB", prog->opbar);
		bar_len = UINT32_MAX;
	}

	dctx.len = bar_len;

	if (dctx.len < IOREG_NVIDIA_SPI_OFF + IOREG_NVIDIA_SPI_SIZE / 4) {
		warnx("BAR 0x%02X too small for ROM", prog->opbar);
		goto ebar;
	}

	dctx.base = mmap_paddr(bar_paddr, bar_len, 1);
	if (dctx.base == NULL) {
		warn("mmap BAR 0x%02X", prog->opbar);
		return (-1);
	}

	LOG("Scanning ROM");
	int success = 0;

	for (uint32_t o = IOREG_NVIDIA_SPI_OFF;
		o < IOREG_NVIDIA_SPI_OFF + IOREG_NVIDIA_SPI_SIZE;
		o += 0x100) {

		if (MMIO_R8(dctx.base, o + 0) == 0x55 &&
			MMIO_R8(dctx.base, o + 1) == 0xAA) {

			LOGV("Trying 0x%X", o);
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
		warn("munmap BAR 0x%02X", prog->opbar);
		return (-1);
	}

	if (success) {
		return (0);
	}

ebar:
	LOG("Trying expansion ROM BAR as fallback");
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
		warnx("BAR 0x%02X is invalid", prog->opbar);
		goto ebar;
	} else if (bar_len > UINT32_MAX) {
		warnx("BAR 0x%02X larger than 4 GiB", prog->opbar);
		bar_len = UINT32_MAX;
	}

	/* Check if the BAR is too small for the access regsistrers */
	if (bar_len - 4 < IOREG_AMD_ROM_DATA) {
		warnx("BAR 0x%02X too small for registers", prog->opbar);
		goto ebar;
	}

	dctx.len = bar_len;
	dctx.base = mmap_paddr(bar_paddr, bar_len, 1);
	if (dctx.base == NULL) {
		warn("mmap BAR 0x%02X", prog->opbar);
		return (-1);
	}

	LOG("Scanning ROM");
	int success = 0;

	/* Limit to 512 KiB */
	for (uint32_t o = 0; o < 0x80000; o += 0x100) {
		uint8_t sig[2];
		h_amd_rom_read(&dctx, sig, 2, o);

		if (sig[0] == 0x55 && sig[1] == 0xAA) {
			LOGV("Trying 0x%X", o);
			if (vbiosdump_walk_rom(prog, o, &dctx, 0,
				h_amd_rom_read) < 0) {

				continue;
			}

			success = 1;
			break;
		}
	}

	if (munmap(dctx.base, dctx.len) < 0) {
		warn("munmap BAR 0x%02X", prog->opbar);
		return (-1);
	}

	if (success) {
		return (0);
	}

ebar:
	LOG("Trying expansion ROM BAR as fallback");
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
		warnx("BAR 0x%02X is invalid", prog->opbar);
		goto ebar;
	} else if (bar_len > UINT32_MAX) {
		warnx("BAR 0x%02X larger than 4 GiB", prog->opbar);
		bar_len = UINT32_MAX;
	}

	/* Check if the BAR is too small for the access regsistrers */
	if (bar_len - 4 < IOREG_INTEL_GET_ROM_OFF) {
		warnx("BAR 0x%02X too small for registers", prog->opbar);
		goto ebar;
	}

	dctx.len = bar_len;
	dctx.base = mmap_paddr(bar_paddr, bar_len, 1);
	if (dctx.base == NULL) {
		warn("mmap BAR 0x%02X", prog->opbar);
		return (-1);
	}

	uint32_t off;
	h_intel_rom_init(dctx.base, &off);
	LOGV("Offset to VBIOS: 0x%X", off);

	LOG("Scanning rom...");
	int success = 0;

	/* Limit to 512 KiB */
	for (uint32_t o = off; o < off + 0x80000; o += 0x100) {
		uint8_t sig[2];
		h_intel_rom_read(&dctx, sig, 2, o);

		if (sig[0] == 0x55 && sig[1] == 0xAA) {
			LOGV("Trying 0x%X", o);
			if (vbiosdump_walk_rom(prog, o, &dctx, 0,
				h_intel_rom_read) < 0) {

				continue;
			}

			success = 1;
			break;
		}
	}

	if (munmap(dctx.base, dctx.len) < 0) {
		warn("munmap BAR 0x%02X", prog->opbar);
		return (-1);
	}

	if (success) {
		return (0);
	}

ebar:
	LOG("Trying expansion ROM BAR as fallback");
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
			LOG("BAR is prefetchable");
forend:
			LOGV("BAR 0x%02X not fitting, trying next one", bar);
		}
	}

	/* If none was chosen */
	if (prog->opbar == 0) {
		LOG("Trying BAR 0x30 as last resort");
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
		warnx("unknown GPU vendor: 0x%04X", vendor);
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
		usage(0);
	}

	int ch;
	while ((ch = getopt(argc, argv, "vhar:b:d:")) != -1) {
		switch (ch) {
		case 'v':
			vflag = 1;
			break;

		case 'h':
			usage(1);
			break;

		case 'a':
			prog->flag_fullchain = 1;
			break;

		case 'r':
			prog->a_trim_path = optarg;
			break;

		case 'b':
			errno = 0;
			char *ep;
			prog->opbar = strtoul(optarg, &ep, 16);
			if (errno) {
				warn("strtoul");
				return (-1);
			} else if (prog->opbar < 0x10 ||
				(prog->opbar > 0x24 && prog->opbar != 0x30)) {

				warnx("provided BAR is out of range, must be "
					"0x10-0x24 or 0x30");
				return (-1);
			} else if (prog->opbar % 4 != 0) {
				warnx("provided BAR is not aligned, try 0x%02X",
					prog->opbar - prog->opbar % 4);
				return (-1);
			}

			break;

		case 'd':
			prog->a_device = optarg;
			break;

		default:
			usage(0);
		}
	}

	/* Fallback to stdout */
	if (optind >= argc) {
		warnx("no output specified");
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
		warnx("trailing arguments after VBIOS");
		usage(0);
	}

	if (prog->a_trim_path == NULL) {
		if (prog->a_device == NULL) {
			warnx("no device specified");
			usage(0);
		}

		if (nyetpci_parse_sel(prog->a_device, &pci->sel) < 0) {
			warnx("invalid device selector format, try "
				"pci0:AA:BB:CC");
			usage(0);
		}
	} else {
		if (prog->a_device != NULL) {
			warnx("must specify either -d or -r, but not both");
			usage(0);
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
	const char *on;
	int ret = EXIT_FAILURE;
	int r;

	if (parse_prog_args(&prog, &pci, argc, argv) < 0) {
		return (EXIT_FAILURE);
	}

	if (prog.a_trim_path != NULL) {
		r = vbiosdump_file(&prog);
	} else {
		if (nyetpci_init(&pci) < 0) {
			warnx("nyetpci_init failed");
			goto exit;
		}

		if (!nyetpci_device_exists(&pci)) {
			warnx("device '%s' does not exist", prog.a_device);
			goto exit;
		}

		if ((r = nyetpci_is_gpu(&pci)) < 0) {
			warn("nyetpci_is_gpu");
			goto exit;
		} else if (r == 0) {
			warnx("device '%s' is not a GPU", prog.a_device);
			goto exit;
		}

		if (prog.opbar == 0x30) {
			r = vbiosdump_ebar(&prog, &pci);
		} else {
			r = vbiosdump_main(&prog, &pci);
		}
	}

	if (r < 0) {
		goto exit;
	}

	if (prog.vbios_data == NULL) {
		warnx("internal error: dump without data");
		goto exit;
	}

	if (prog.flag_stdout) {
		prog.vbios_fp = stdout;
		on = "stdout";
	} else {
		if (open_file_write(&prog, prog.vbios_path) < 0) {
			goto exit;
		}
		on = prog.vbios_path;
	}

	if (fwrite(prog.vbios_data, prog.vbios_len, 1, prog.vbios_fp) != 1 ||
		fflush(prog.vbios_fp) != 0) {

		warn("%s", on);
		goto exit;
	}

	if (!prog.flag_stdout) {
		r = fclose(prog.vbios_fp);
		prog.vbios_fp = NULL;
		if (r != 0) {
			warn("%s", on);
			goto exit;
		}
	}

	LOG("VBIOS dumped successfully");
	ret = EXIT_SUCCESS;
exit:
	(void)nyetpci_free(&pci);
	prog_free(&prog);
	return (ret);
}
