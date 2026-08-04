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
 * vbiosdump.c - Cross-GPU tool for dumping firmware
 *
 * Currently, only available for FreeBSD
 */

#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/pciio.h>

#include <vm/vm.h>

#include <assert.h>
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

/* Debug logging */

#if defined(DEBUG)
#  define LOG(STR) fprintf(stderr, "Log @ %s:%d:%s: "STR"\n", \
	__FILE__, __LINE__, __func__)
#  define LOGV(FMT, ...) fprintf(stderr, "Log @ %s:%d:%s: "FMT"\n", \
	__FILE__, __LINE__, __func__, __VA_ARGS__)
#else
#  define LOG(STR) (void)0
#  define LOGV(FMT, ...) (void)0
#endif

/* Plenty of space */
#define MAX_VBIOS_SIZE 1024 * 1024 * 10

/* Volatile MMIO crap */
#define MMIO_R8(B, O)     (*(volatile uint8_t *)((uint8_t *)(B) + (O)))
#define MMIO_R16(B, O)    (*(volatile uint16_t *)((uint8_t *)(B) + (O)))
#define MMIO_R32(B, O)    (*(volatile uint32_t *)((uint8_t *)(B) + (O)))
#define MMIO_R64(B, O)    (*(volatile uint64_t *)((uint8_t *)(B) + (O)))

#define MMIO_W8(B, O, V)  (*(volatile uint8_t *)((uint8_t *)(B) + (O))) = (V)
#define MMIO_W16(B, O, V) (*(volatile uint16_t *)((uint8_t *)(B) + (O))) = (V)
#define MMIO_W32(B, O, V) (*(volatile uint32_t *)((uint8_t *)(B) + (O))) = (V)
#define MMIO_W64(B, O, V) (*(volatile uint64_t *)((uint8_t *)(B) + (O))) = (V)

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

/* Nvidia GPU MMIO ROM offset */
#define IOREG_NVIDIA_ROM_OFFSET	0x300000 /* ROM located directly over here */

/*
 * Structs
 */

/* PCI context */
struct ctx_pci {
	/* FD to /dev/pci */
	int fd;

	/* PCI device selector */
	struct pcisel sel;

	/* Generic IO */
	struct pci_conf_io pc;

	/* BAR MMAP */
	struct pci_bar_mmap pbm;
};

/* Program context */
struct ctx_prog {
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

	/* Use internal BAR mmap function */
	int flag_unrestrict;

	/* VBIOS itself */
	uint8_t *vbios_data;
	size_t vbios_len;
};

/* Dumper contexts */

struct dctx_generic {
	/* BAR base */
	volatile void *base;
};

/*
 * Very much unnecessary data type implementation,
 *   probably going to repurpose this somewhere
 */

#pragma pack(push, 1)
/* Nvidia IFR */
struct ifr_hdr {
	/* 0x00; "NVGI" / 0x4947564E */
	uint32_t magic;

	/*
	 * 0x04; Bitfield
	 * 31    - reserved
	 * 30:16 - fixed_data_size
	 * 15:8  - version
	 * 7:0   - reserved
	 */
	uint32_t fixed1;

	/*
	 * 0x08; Bitfield
	 * 31    - reserved
	 * 30:20 - reserved, zero
	 * 19:0  - total_data_size
	 */
	uint32_t fixed2;
};
#define IFR_FIXED_DATA_SIZE(F1)	(((F1) >> 16) & 0x7FFF)
#define IFR_VERSION(F1)		(((F1) >> 8) & 0xFF)
#define IFR_TOTAL_DATA_SIZE(F2)	((F2) & 0xFFFFF)

/* Legacy BIOS OpROM */
struct oprom_hdr_legacy {
	/* 0x00; 55 AA */
	uint8_t magic[2];

	/* 0x02; Image runtime size in 512-byte chunks */
	uint8_t init_size;

	/* 0x03; X86 Init vector */
	uint8_t init_vector[4];

	/* 0x07; Reserved */
	uint8_t reserved1[17];

	/* 0x18; Offset to PCIR structure */
	uint16_t pcir_off;

	/* 0x1A */
};

/* UEFI OpROM */
struct oprom_hdr_efi {
	/* 0x00; 55 AA */
	uint8_t magic[2];

	/* 0x02; Legacy image runtime size in 512-byte chunks */
	uint16_t init_size;

	/* 0x04; 0x0EF1 */
	uint32_t efi_magic;

	/* 0x08; EFI subsystem type */
	uint16_t efi_subsystem_type;

	/* 0x0A; EFI machine type */
	uint16_t efi_machine_type;

	/* 0x0C; EFI compression type */
	uint16_t efi_compression_type;

	/* 0x0E; Reserved */
	uint8_t reserved1[8];

	/* 0x16; Offset to EFI image */
	uint16_t efi_off;

	/* 0x18; Offset to PCIR structure */
	uint16_t pcir_off;

	/* 0x1A */
};

/* PCIR */
struct oprom_pcir {
	/* 0x00; "PCIR" */
	uint8_t magic[4];

	/* 0x04; PCI vendor ID */
	uint16_t pci_vendor;

	/* 0x06; PCI device ID */
	uint16_t pci_device;

	/* 0x08; PCIR 3.0; pointer to device IDs or VPD */
	uint16_t pci_device_list_ptr;

	/* 0x0A; Length of this struct in bytes */
	uint16_t pcir_len;

	/* 0x0C; Version of PCIR struct */
	uint8_t pcir_ver;

	/* 0x0D; PCI class code */
	uint8_t pci_class[3];

	/* 0x10; Length of this OpROM image in 512-byte chunks */
	uint16_t image_len;

	/* 0x12; ROM version */
	uint16_t rom_ver;

	/* 0x14; Image type */
	uint8_t image_type;

	/* 0x15; Bit 7; (this & 0x80) == Last image in the ROM */
	uint8_t image_is_last;

	/* 0x16; Amount of runtime (after init) memory this image needs */
	uint16_t max_runtime_size;

	/* 0x18; Reserved (?) */
	uint16_t reserved1;

	/* 0x1A; Offset to MCTP initialization entries (?) */
	uint16_t mctp_off;

	/* 0x1C */
};
#pragma pack(pop)

enum oprom_efi_subsystem_type {
	EFI_SUBSYSTEM_BOOT_DRIVER = 0x0B,
	EFI_SUBSYSTEM_RUNTIME_DRIVER = 0x0C,
	EFI_SUBSYSTEM_ROM_IMAGE = 0x0D
};

enum oprom_efi_machine_type {
	EFI_MACHINE_IA32 = 0x014C,
	EFI_MACHINE_X64 = 0x8664,
	EFI_MACHINE_AARCH64 = 0xAA64
};

enum oprom_efi_compression_type {
	EFI_COMPRESSION_NO = 0x0000,
	EFI_COMPRESSION_UEFI = 0x0001
};

enum pcir_image_type {
	PCIR_IMAGE_LEGACY = 0x00,
	PCIR_IMAGE_OPENFW = 0x01,
	PCIR_IMAGE_HP = 0x02,
	PCIR_IMAGE_EFI = 0x03
};

/*
 * Functions
 */

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
"               through the expansion ROM BAR\n"
"  -u         Use internal BAR mmap function instead of an ioctl\n"
"             Requires /dev/mem access, implied when specifying -b 0x30\n"
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

/*
 * Free ctx members
 */
static void
free_ctx_pci(
	struct ctx_pci *pci
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

	if (pci->pbm.pbm_map_base) {
		if (munmap(pci->pbm.pbm_map_base,
			pci->pbm.pbm_map_length) < 0) {

			LOG("Failure munmapping BAR!");
		}
		memset(&pci->pbm, 0, sizeof(struct pci_bar_mmap));
	}
}

static void
free_ctx_prog(
	struct ctx_prog *prog
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
 * Open a PCI device by selector
 *
 * Returns:
 *   0 - Success
 *  -1 - Failure
 */
static int
pci_open(
	struct ctx_prog *prog,
	struct ctx_pci *pci
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
 * Read/write from/to PCI config space
 *
 * Returns:
 *   0 - Success
 *  -1 - Failure
 *
 * Width must be 1, 2 or 4
 * Offset is in bytes and must be a multiple of the width
 */

static int
pci_cfg_read(
	struct ctx_pci *pci,
	uint32_t *value,
	size_t offset,
	int width
	)
{
	if ((width != 1 && width != 2 && width != 4) || (offset % width) != 0) {
		return (-1);
	}

	struct pci_io io = {0};

	io.pi_sel = pci->sel;
	io.pi_reg = offset;
	io.pi_width = width;

	if (ioctl(pci->fd, PCIOCREAD, &io) < 0) {
		perror("ioctl(PCIOCREAD)");
		return (-1);
	}

	*value = io.pi_data;

	return (0);
}

static int
pci_cfg_write(
	struct ctx_pci *pci,
	uint32_t *value,
	size_t offset,
	int width
	)
{
	if ((width != 1 && width != 2 && width != 4) || (offset % width) != 0) {
		return (-1);
	}

	struct pci_io io = {0};

	io.pi_sel = pci->sel;
	io.pi_reg = offset;
	io.pi_width = width;
	io.pi_data = *value;

	if (ioctl(pci->fd, PCIOCWRITE, &io) < 0) {
		perror("ioctl(PCIOCWRITE)");
		return (-1);
	}

	return (0);
}

/*
 * Memory map a BAR
 *
 * Returns:
 *   0 - Success
 *  -1 - Failure
 */
static int
pci_bar_mmap(
	struct ctx_pci *pci,
	int reg,
	int flags,
	int memattr
	)
{
	LOGV("Mmapping BAR 0x%02X", reg);

	pci->pbm.pbm_sel = pci->sel;
	pci->pbm.pbm_reg = reg;
	pci->pbm.pbm_flags = flags;
	pci->pbm.pbm_memattr = memattr;

	if (ioctl(pci->fd, PCIOCBARMMAP, &pci->pbm) < 0) {
		perror("ioctl(PCIOCBARMMAP)");
		return (-1);
	}

	return (0);
}

/*
 * Memory unmap a BAR
 *
 * Returns:
 *   0 - Success
 *  -1 - Failure
 */
static int
pci_bar_munmap(
	struct pci_bar_mmap *pbm
	)
{
	LOGV("Munmapping BAR 0x%02X", pbm->pbm_reg);
	if (pbm->pbm_map_base != NULL) {

		if (munmap(pbm->pbm_map_base, pbm->pbm_map_length) < 0) {
			perror("munmap()");
			return (-1);
		}
		memset(pbm, 0, sizeof(struct pci_bar_mmap));
	} else {
		LOG("This is a NULL pointer, not a memory map!");
	}

	return (0);
}

/*
 * Memory map a BAR (the hard way)
 *
 * Required to mmap BAR 0x30 since ioctl(PCIOCBARMMAP) rejects it
 *
 * Returns:
 *   0 - Success
 *  -1 - Failure
 *
 * Return with -1 and errno = 0 - BAR does not exist
 */
static int
pci_bar_mmap_unrestricted(
	struct ctx_pci *pci,
	int reg,
	int allow_writes
	)
{
	/* Mimick the behavior of pci_bar_mmap */
	pci->pbm.pbm_sel = pci->sel;
	pci->pbm.pbm_reg = reg;

	LOGV("Mmapping BAR 0x%02X (unrestricted)", reg);

/* Convenience macros */
#define READ(TO, REG, WIDTH) \
	do { if (pci_cfg_read(pci, (TO), (REG), (WIDTH)) < 0) { \
		return (-1); \
	} } while (0)

#define WRITE(FROM, REG, WIDTH) \
	do { if (pci_cfg_write(pci, (FROM), (REG), (WIDTH)) < 0) { \
		return (-1); \
	} } while (0)

	/* Save the register value */
	uint32_t bar_save;
	READ(&bar_save, reg, 4);

	/* Get the base address from it */
	uint32_t bar_base = bar_save & ~0x7FF;
	pci->pbm.pbm_bar_off = 0; /* Hmmm */

	/* Write all ones, PCI returns BAR length */
	uint32_t tmp = 0xFFFFFFFF;
	WRITE(&tmp, reg, 4);
	READ(&tmp, reg, 4);
	tmp &= ~0x7FF;

	if (tmp == 0) {
		errno = 0;
		return (-1);
	}

	pci->pbm.pbm_bar_length = (uint32_t)(-(int32_t)tmp);
	pci->pbm.pbm_map_length = pci->pbm.pbm_bar_length;

	/* Restore original register value + enable the BAR */
	bar_save |= 0x01;
	WRITE(&bar_save, reg, 4);

	/* Enable PCI_CMD_MEM_SPACE just in case */
	READ(&tmp, 0x04, 2);
	tmp |= 0x02;
	WRITE(&tmp, 0x04, 2);

	/* Open /dev/mem */
	int flags = O_RDONLY;
	if (allow_writes) {
		flags = O_RDWR;
	}

	int memfd = open("/dev/mem", flags);
	if (memfd < 0) {
		perror("open(/dev/mem)");
		return (-1);
	}

	/* Mmap the BAR */
	flags = PROT_READ;
	if (allow_writes) {
		flags |= PROT_WRITE;
	}

	pci->pbm.pbm_map_base = mmap(NULL, pci->pbm.pbm_bar_length,
		flags, MAP_SHARED | MAP_NOCORE,
		memfd, bar_base);

	close(memfd);

	if (pci->pbm.pbm_map_base == MAP_FAILED) {
		perror("mmap(/dev/mem)");
		return (-1);
	}

	return (0);
}

#undef READ
#undef WRITE

/*
 * Check if a device in PCI context is a graphics adapter
 *
 * Returns:
 *   1 - Yes
 *   0 - No
 *  -1 - Error
 */
static int
pci_is_gpu(
	struct ctx_pci *pci
	)
{
	uint32_t r2;
	if (pci_cfg_read(pci, &r2, 8, 4) < 0) {
		return (-1);
	}

	LOGV("r2:0x%08X", r2);

	return ((r2 >> 24) == 0x03);
}

/*
 * Check if a BAR is prefetchable
 *
 * Returns:
 *  >0 - Yes
 *   0 - No
 *  -1 - Error
 */
static int
bar_is_prefetchable(
	struct ctx_pci *pci,
	int reg
	)
{
	uint32_t b;
	if (pci_cfg_read(pci, &b, reg, 4) < 0) {
		return (-1);
	}

	return ((b & 0x4) != 0);
}

/*
 * Check if a BAR is an I/O port
 *
 * Returns:
 *  >0 - Yes
 *   0 - No
 *  -1 - Error
 */
static int
bar_is_ioport(
	struct ctx_pci *pci,
	int reg
	)
{
	uint32_t b;
	if (pci_cfg_read(pci, &b, reg, 4) < 0) {
		return (-1);
	}

	return ((b & 0x1) != 0);
}

/*
 * Parse the Nvidia IFR header
 *
 * Return offset to VBIOS, 0 on error
 *
 * TODO: Find a GPU that actually exposes the IFR to 0x300000
 *       The A100 should do it? https://lwn.net/Articles/1068168/
 */
static uint32_t
h_nvidia_parse_ifr(
	volatile void *base
	)
{
	struct ifr_hdr ifr;
	volatile uint8_t *b = (volatile uint8_t *)base;

	ifr.magic = MMIO_R32(b, 0);
	ifr.fixed1 = MMIO_R32(b, 4);
	ifr.fixed2 = MMIO_R32(b, 8);

	if (memcmp(&ifr.magic, "NVGI", 4) != 0) {
		return (0);
	}

	uint32_t version = IFR_VERSION(ifr.fixed1);

	if (version < 3) {
		uint32_t fixed_data_size = IFR_FIXED_DATA_SIZE(ifr.fixed1);
		return (MMIO_R32(b, fixed_data_size + 4));
	} else if (version == 3) {
		uint32_t total_data_size = IFR_TOTAL_DATA_SIZE(ifr.fixed2);
		uint32_t rom_directory_offset = MMIO_R32(b,
			total_data_size) + 4096;
		uint32_t rd_magic = MMIO_R32(b, rom_directory_offset);

		if (memcmp(&rd_magic, "RFRD", 4) != 0) {
			return (0);
		} else {
			return (MMIO_R32(b, rom_directory_offset + 8));
		}
	}

	/* Eh. */
	return (0);
}

/*
 * Read from raw memory
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
 * Read AMD GPU ROM
 */
static void
h_amd_rom_read(
	void *dctx,
	void *data,
	size_t len,
	uint32_t off
	)
{
	volatile void *base = ((struct dctx_generic *)dctx)->base;
	uint32_t i = 0;

	/* Set GPU's internal ROM pointer */
	MMIO_W32(base, IOREG_AMD_ROM_INDEX, off);

	/* Read most as 32-bit */
	while (i + 4 <= len) {
		uint32_t chunk = MMIO_R32(base, IOREG_AMD_ROM_DATA);
		memcpy((uint8_t *)data + i, &chunk, sizeof(uint32_t));
		i += sizeof(uint32_t);
	}

	/* Finish off tail as a single OOB-not access */
	if (i < len) {
		uint32_t chunk = MMIO_R32(base, IOREG_AMD_ROM_DATA);
		memcpy((uint8_t *)data + i, &chunk, len - i);
	}
}

/*
 * Initialize Intel dGPU for ROM reading and go to VBIOS region
 *
 * Returns:
 *   0 - Success
 *  -1 - Failure
 *
 * BAR must be mmapped
 */
static void
h_intel_rom_init(
	volatile void *base,
	uint32_t *off
	)
{
	LOG("Initializing ROM");

	/* Find region with VBIOS */
	uint32_t region = MMIO_R32(base, IOREG_INTEL_GET_REGION) &
		MASK_INTEL_ROM_REGION;

	LOGV("region:0x%08X", region);

	/* Select it */
	MMIO_W32(base, IOREG_INTEL_SEL_REGION, region);

	/* Offset to VBIOS start */
	*off = MMIO_R32(base, IOREG_INTEL_GET_ROM_OFF) & MASK_INTEL_ROM_OFFSET;
}

/*
 * Read Intel dGPU ROM
 *
 * BAR must be mmapped
 */
static void
h_intel_rom_read(
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
		/* Ask for an address */
		MMIO_W32(base, IOREG_INTEL_SEL_ADDR, off + i);

		/* Read the return */
		uint32_t chunk = MMIO_R32(base, IOREG_INTEL_READ_ADDR);

		memcpy((uint8_t *)data + i, &chunk, sizeof(uint32_t));
		i += sizeof(uint32_t);
	}

	/* Finish off tail as a single OOB-not access */
	if (i < len) {
		MMIO_W32(base, IOREG_INTEL_SEL_ADDR, off + i);
		uint32_t chunk = MMIO_R32(base, IOREG_INTEL_READ_ADDR);
		memcpy((uint8_t *)data + i, &chunk, len - i);
	}
}

/*
 * Walk the ROM and dump it to prog->vbios_data provided a read function
 */
static int
vbiosdump_walk_rom(
	struct ctx_prog *prog,
	struct ctx_pci *pci,
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

			/* Additionally print a message for Intel */
			if (h_read == h_intel_rom_read) {
				fprintf(stderr, "%s: This is likely a mobile "
					"GPU\n", prog->name);
			}
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

		/*
		 * Intel GPUs seem to have the IDs in PCIR set to all zeroes
		 * Only do a debug log to not scare people
		 */
		if (pcir.pci_device != pci->pc.matches[0].pc_device) {
			LOGV("PCIR and real device IDs do not match "
				"(0x%04X and 0x%04X) (this is normal)",
				pcir.pci_device, pci->pc.matches[0].pc_device);
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
 * Generic dumper through expansion ROM BAR
 *
 * Returns:
 *   0 - Success
 *  -1 - Failure
 */
static int
vbiosdump_ebar(
	struct ctx_prog *prog,
	struct ctx_pci *pci
	)
{
	LOG("Dumping through expansion ROM BAR");

	if (pci_bar_mmap_unrestricted(pci, 0x30, 0) < 0) {
		if (errno == 0) {
			fprintf(stderr, "%s: No expansion ROM BAR\n", prog->name);
		}
		return (-1);
	}

	struct dctx_generic dctx;
	dctx.base = pci->pbm.pbm_map_base;

	LOG("Walking ROM");
	if (vbiosdump_walk_rom(prog, pci, 0, &dctx, h_raw_rom_read) < 0) {
		return (-1);
	}

	return (0);
}

/*
 * Dump NVIDIA VBIOS to prog->vbios_data; prog->vbios_len
 *
 * Returns:
 *   0 - Success
 *  -1 - Failure
 */
static int
vbiosdump_nvidia_main(
	struct ctx_prog *prog,
	struct ctx_pci *pci
	)
{
	LOG("Dumping for NVIDIA");

	if (prog->flag_unrestrict) {
		if (pci_bar_mmap_unrestricted(pci, prog->opbar, 0) < 0) {
			goto error;
		}
	} else {
		if (pci_bar_mmap(pci, prog->opbar, 0,
			VM_MEMATTR_UNCACHEABLE) < 0) {

			goto error;
		}
	}

	volatile uint8_t *base = (volatile uint8_t *)pci->pbm.pbm_map_base +
		pci->pbm.pbm_bar_off + IOREG_NVIDIA_ROM_OFFSET;

	/* Do a little header probe */
	uint32_t off = 0;
	/* OpROM */
	if (MMIO_R8(base, 0) != 0x55) {
		/* IFR */
		if (MMIO_R8(base, 0) != 0x4E) {
			fprintf(stderr, "%s: No OpROM or IFR header, "
				"this is likely a mobile GPU\n", prog->name);
			goto error;
		} else {
			LOG("Need to strip IFR");
			if ((off = h_nvidia_parse_ifr(base)) == 0) {
				fprintf(stderr, "%s: Corrupt IFR header\n",
					prog->name);
				goto error;
			}
		}
	}

	struct dctx_generic dctx;
	dctx.base = base + off;

	LOG("Walking ROM");
	if (vbiosdump_walk_rom(prog, pci, 0, &dctx, h_raw_rom_read) < 0) {
		goto error;
	}

	return (0);
error:
	return (-1);
}

/*
 * Dump AMD VBIOS to prog->vbios_data; prog->vbios_len
 *
 * Returns:
 *   0 - Success
 *  -1 - Failure
 */
static int
vbiosdump_amd_main(
	struct ctx_prog *prog,
	struct ctx_pci *pci
	)
{
	LOG("Dumping for AMD");

	if (prog->flag_unrestrict) {
		if (pci_bar_mmap_unrestricted(pci, prog->opbar, 1) < 0) {
			goto error;
		}
	} else {
		if (pci_bar_mmap(pci, prog->opbar, PCIIO_BAR_MMAP_RW,
			VM_MEMATTR_UNCACHEABLE) < 0) {

			goto error;
		}
	}

	struct dctx_generic dctx;

	dctx.base = (volatile uint8_t *)pci->pbm.pbm_map_base +
		pci->pbm.pbm_bar_off;

	LOG("Walking ROM");
	if (vbiosdump_walk_rom(prog, pci, 0, &dctx, h_amd_rom_read) < 0) {
		fprintf(stderr, "Trying expansion ROM BAR as fallback\n");

		if (pci_bar_munmap(&pci->pbm) < 0) {
			fprintf(stderr, "%s: Failure munmapping BAR\n",
				prog->name);
			return (-1);
		}

		return (vbiosdump_ebar(prog, pci));
	}

	return (0);
error:
	return (-1);
}

/*
 * Dump Intel VBIOS to prog->vbios_data; prog->vbios_len
 *
 * Returns:
 *   0 - Success
 *  -1 - Failure
 */
static int
vbiosdump_intel_main(
	struct ctx_prog *prog,
	struct ctx_pci *pci
	)
{
	LOG("Dumping for Intel");

	if (prog->flag_unrestrict) {
		if (pci_bar_mmap_unrestricted(pci, prog->opbar, 1) < 0) {
			goto error;
		}
	} else {
		if (pci_bar_mmap(pci, prog->opbar, PCIIO_BAR_MMAP_RW,
			VM_MEMATTR_UNCACHEABLE) < 0) {

			goto error;
		}
	}

	struct dctx_generic dctx;

	dctx.base = (volatile uint8_t *)pci->pbm.pbm_map_base +
		pci->pbm.pbm_bar_off;

	uint32_t off;
	h_intel_rom_init(dctx.base, &off);
	LOGV("Offset to VBIOS: %u", off);

	LOG("Walking ROM");
	if (vbiosdump_walk_rom(prog, pci, off, &dctx, h_intel_rom_read) < 0) {
		goto error;
	}

	return (0);
error:
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
	struct ctx_prog *prog,
	struct ctx_pci *pci
	)
{
	LOGV("vendor:0x%04X", pci->pc.matches[0].pc_vendor);
	LOGV("device:0x%04X", pci->pc.matches[0].pc_device);

	int r;
	uint32_t b;

	/* Select an MMIO BAR */
	if (prog->opbar == 0) {
		for (int bar = 0x10; bar <= 0x24; bar += 4) {
			if (pci_cfg_read(pci, &b, bar, 4) < 0) {
				return (-1);
			}

			if (!b) {
				goto forend;
			}

			r = bar_is_ioport(pci, bar);
			if (r < 0) {
				return (-1);
			} else if (r) {
				goto forend;
			}

			r = bar_is_prefetchable(pci, bar);
			if (r < 0) {
				return (-1);
			} else if (!r) {
				prog->opbar = bar;
				break;
			}
forend:
			fprintf(stderr, "BAR 0x%02X not MMIO, trying "
				"next one\n", bar);
		}
	}

	/* If none was chosen */
	if (prog->opbar == 0) {
		fprintf(stderr, "Trying BAR 0x30 as last resort\n");
		prog->opbar = 0x30;
		prog->flag_unrestrict = 1;
		return (vbiosdump_ebar(prog, pci));
	}

	switch (pci->pc.matches[0].pc_vendor) {
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
			prog->name, pci->pc.matches[0].pc_vendor);
		return (-1);
	}

	return (r);

#undef DEFAULT_OPBAR
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
	struct ctx_prog *prog,
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
 * Parse program arguments to fill in ctx_prog and ctx_pci.sel
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
	struct ctx_prog *prog,
	struct ctx_pci *pci,
	int argc,
	char *argv[]
	)
{
	if (argc < 2) {
		usage(prog->name, 0);
	}

	int ch;
	while ((ch = getopt(argc, argv, "hb:ud:")) != -1) {
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

			/* Use unrestrict mmap for 0x30 */
			if (prog->opbar == 0x30) {
				prog->flag_unrestrict = 1;
			}

			break;

		case 'u':
			prog->flag_unrestrict = 1;
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

	if (pci_parse_sel(prog->a_device, &pci->sel) < 0) {
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
	struct ctx_prog prog = {0};
	prog.name = argv[0];
	struct ctx_pci pci = {0};
	pci.fd = -1;

	if (parse_prog_args(&prog, &pci, argc, argv) < 0) {
		goto error;
	}

	if (pci_open(&prog, &pci) < 0) {
		/* Failure but no errno set */
		if (errno == 0) {
			fprintf(stderr, "%s: No matching PCI device\n",
				prog.name);
		}
		goto error;
	}

	int r;
	if ((r = pci_is_gpu(&pci)) < 1) {
		if (r == 0) {
			fprintf(stderr, "%s: Not a GPU\n", prog.name);
		}
		goto error;
	}

	if (prog.opbar == 0x30) {
		LOG("Using expansion ROM BAR");
		if (vbiosdump_ebar(&prog, &pci) < 0) {
			goto error;
		}
	} else {
		LOG("Probing GPU");
		if (vbiosdump_main(&prog, &pci) < 0) {
			goto error;
		}
	}

	if (prog.vbios_data == NULL) {
		fprintf(stderr, "%s: VBIOS never got fetched\n",
			prog.name);
		goto error;
	}

	if (prog.flag_stdout) {
		prog.vbios_fp = stdout;
	} else if (open_file_write(&prog, prog.vbios_path) < 0) {
		goto error;
	}

	LOG("Writing");
	if (fwrite(prog.vbios_data, prog.vbios_len, 1, prog.vbios_fp) != 1) {
		fprintf(stderr, "%s: Short write\n", prog.name);
		goto error;
	}

	/* Flush and check for error again */
	LOG("Flushing output");
	fflush(prog.vbios_fp);
	if (ferror(prog.vbios_fp)) {
		fprintf(stderr, "%s: Error writing output\n", prog.name);
		goto error;
	}

	fprintf(stderr, "VBIOS dumped successfully!\n");

	free_ctx_prog(&prog);
	free_ctx_pci(&pci);
	return (EXIT_SUCCESS);
error:
	LOGV("Hello, error handler! errno = %d", errno);
	fprintf(stderr, "Failed to dump VBIOS\n");

	free_ctx_prog(&prog);
	free_ctx_pci(&pci);
	return (EXIT_FAILURE);
}
