/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Alexey Laurentsyeu <alex@paidbsd.org>
 */

#ifndef _PCISAVE_H
#define _PCISAVE_H

#include <stdint.h>

#define CFG_SZ		(256)
#define CFG_MASK_SZ	(CFG_SZ / 8 / 4)
#define CFG_EXT_SZ	(4096)
#define CFG_EXT_MASK_SZ	(CFG_EXT_SZ / 8 / 4)

#define HDR_VERSION 3
#pragma pack(push, 1)
struct pci_save {
	/* 0x00; 0x9 + "SAV" */
	char magic[4];

	/* 0x04; Header version */
	uint16_t version;

	/* 0x06; PCI vendor ID */
	uint16_t vendor;

	/* 0x08; PCI device ID */
	uint16_t device;

	/* 0x0A; PCI subsystem vendor ID */
	uint16_t subvendor;

	/* 0x0C; PCI subsystem device ID */
	uint16_t subdevice;

	/* 0x0E; CRC32 checksum */
	uint32_t checksum;

	/* 0x12; Extended config space mask */
	uint8_t config_mask[CFG_EXT_MASK_SZ];

	/* 0x92; Extended config space */
	uint8_t config[CFG_EXT_SZ];

	/* 0x1092 */
};
#pragma pack(pop)

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

#endif /* _PCISAVE_H */
