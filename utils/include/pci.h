#pragma once

#include <sys/types.h>
#include <sys/pciio.h>

#include <stdint.h>

/*
 * Macros
 */

/* Debug logging */
#if defined(DEBUG)
#  define LOG(STR) fprintf(stderr, "(DEBUG) %s:%d:%s: "STR"\n", \
	__FILE__, __LINE__, __func__)
#  define LOGV(FMT, ...) fprintf(stderr, "(DEBUG) %s:%d:%s: "FMT"\n", \
	__FILE__, __LINE__, __func__, __VA_ARGS__)
#else
#  define LOG(STR) (void)0
#  define LOGV(FMT, ...) (void)0
#endif

/* Volatile MMIO crap */
#define MMIO_R8(B, O)     (*(volatile uint8_t *)((uint8_t *)(B) + (O)))
#define MMIO_R16(B, O)    (*(volatile uint16_t *)((uint8_t *)(B) + (O)))
#define MMIO_R32(B, O)    (*(volatile uint32_t *)((uint8_t *)(B) + (O)))
#define MMIO_R64(B, O)    (*(volatile uint64_t *)((uint8_t *)(B) + (O)))

#define MMIO_W8(B, O, V)  (*(volatile uint8_t *)((uint8_t *)(B) + (O))) = (V)
#define MMIO_W16(B, O, V) (*(volatile uint16_t *)((uint8_t *)(B) + (O))) = (V)
#define MMIO_W32(B, O, V) (*(volatile uint32_t *)((uint8_t *)(B) + (O))) = (V)
#define MMIO_W64(B, O, V) (*(volatile uint64_t *)((uint8_t *)(B) + (O))) = (V)

/*
 * Structs
 */

/* PCI context */
struct pci_ctx {
	/* FD to /dev/pci */
	int fd;

	/* PCI device selector */
	struct pcisel sel;

	/* Generic IO */
	struct pci_conf_io pc;

	/* BAR MMAP */
	struct pci_bar_mmap pbm;
};

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
 * Free PCI context members
 */
void
pci_ctx_free(
	struct pci_ctx *pci
	);

/*
 * Parse a PCI selector
 * Adapted from /usr/src/usr.sbin/pciconf/pciconf.c
 *
 * Returns:
 *   0 - Success
 *  -1 - Failure
 */
int
pci_parse_sel(
	const char *str,
	struct pcisel *sel
	);

/*
 * Open a PCI device by selector
 *
 * Returns:
 *   0 - Success
 *  -1 - Failure
 */
int
pci_open(
	struct pci_ctx *pci
	);

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

int
pci_cfg_read(
	struct pci_ctx *pci,
	uint32_t *value,
	size_t offset,
	int width
);

int
pci_cfg_write(
	struct pci_ctx *pci,
	uint32_t *value,
	size_t offset,
	int width
	);

/*
 * Memory map a BAR
 *
 * Returns:
 *   0 - Success
 *  -1 - Failure
 */
int
pci_bar_mmap(
	struct pci_ctx *pci,
	int reg,
	int flags,
	int memattr
	);

/*
 * Memory unmap a BAR
 *
 * Returns:
 *   0 - Success
 *  -1 - Failure
 */
int
pci_bar_munmap(
	struct pci_bar_mmap *pbm
	);

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
int
pci_bar_mmap_unrestricted(
	struct pci_ctx *pci,
	int reg,
	int allow_writes
	);

/*
 * Check if a device in PCI context is a graphics adapter
 *
 * Returns:
 *   1 - Yes
 *   0 - No
 *  -1 - Error
 */
int
pci_is_gpu(
	struct pci_ctx *pci
	);

/*
 * Check if a BAR is prefetchable
 *
 * Returns:
 *  >0 - Yes
 *   0 - No
 *  -1 - Error
 */
int
pci_bar_is_prefetchable(
	struct pci_ctx *pci,
	int reg
	);

/*
 * Check if a BAR is an I/O port
 *
 * Returns:
 *  >0 - Yes
 *   0 - No
 *  -1 - Error
 */
int
pci_bar_is_ioport(
	struct pci_ctx *pci,
	int reg
	);
