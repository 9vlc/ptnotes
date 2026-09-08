/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Alexey Laurentsyeu <alex@paidbsd.org>
 */

#ifndef _VBIOSDUMP_H
#define _VBIOSDUMP_H

#define MAX_VBIOS_SIZE 1024 * 1024 * 10

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

/* Nvidia GPU MMIO SPI offset; OpROM not always in the beginning */
#define IOREG_NVIDIA_SPI_OFF	0x300000
#define IOREG_NVIDIA_SPI_SIZE	0x100000

/*
 * Structs
 */

#pragma pack(push, 1)
/* Legacy BIOS OpROM */
struct oprom_hdr_legacy {
	/* 0x00; 55 AA */
	uint8_t magic[2];

	/* 0x02; Image size in 512-byte chunks including header */
	uint8_t size;

	/* 0x03; x86 Init entry point (jump to actual code) */
	uint8_t init_vector[3];

	/* 0x06; Reserved */
	uint8_t reserved1[18];

	/* 0x18; Offset to PCIR structure */
	uint16_t pcir_off;

	/* 0x1A */
};

/* UEFI OpROM */
struct oprom_hdr_efi {
	/* 0x00; 55 AA */
	uint8_t magic[2];

	/* 0x02; Image size in 512-byte chunks including header */
	uint16_t size;

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
	uint8_t indicator;

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

#endif /* _VBIOSDUMP_H */
