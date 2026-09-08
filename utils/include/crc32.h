/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Alexey Laurentsyeu <alex@paidbsd.org>
 */

#ifndef _CRC32_H
#define _CRC32_H

#include <stdio.h>

/*
 * Do one step of the slow sum algorithm
 * Requires current CRC32 state
 */
static inline uint32_t
crc32_step(uint32_t crc, const uint8_t byte)
{
	crc ^= byte;

#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
	for (int i = 0; i < 8; i++) {
#else
	for (int i = 8; i > 0; i--) {
#endif
		crc = (crc >> 1) ^ (0xEDB88320 & (-(crc & 1)));
	}

	return (crc);
}

#endif /* _CRC32_H */
