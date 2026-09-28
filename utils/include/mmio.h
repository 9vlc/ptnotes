/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Alexey Laurentsyeu <alex@paidbsd.org>
 */

#ifndef _MMIO_H
#define _MMIO_H

#include <stdint.h>

#define MMIO_R8(B, O)     (*(volatile uint8_t *)((uint8_t *)(B) + (O)))
#define MMIO_R16(B, O)    (*(volatile uint16_t *)((uint8_t *)(B) + (O)))
#define MMIO_R32(B, O)    (*(volatile uint32_t *)((uint8_t *)(B) + (O)))
#define MMIO_R64(B, O)    (*(volatile uint64_t *)((uint8_t *)(B) + (O)))

#define MMIO_W8(B, O, V)  (*(volatile uint8_t *)((uint8_t *)(B) + (O))) = (V)
#define MMIO_W16(B, O, V) (*(volatile uint16_t *)((uint8_t *)(B) + (O))) = (V)
#define MMIO_W32(B, O, V) (*(volatile uint32_t *)((uint8_t *)(B) + (O))) = (V)
#define MMIO_W64(B, O, V) (*(volatile uint64_t *)((uint8_t *)(B) + (O))) = (V)

#endif /* _MMIO_H */
