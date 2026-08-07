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

#include <pci.h>

#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/mman.h>

#include <fcntl.h>
#include <errno.h>
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

/*
 * Free PCI context members
 */
void
pci_ctx_free(
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

	if (pci->pbm.pbm_map_base) {
		if (munmap(pci->pbm.pbm_map_base,
			pci->pbm.pbm_map_length) < 0) {

			LOG("Failure munmapping BAR!");
		}
		memset(&pci->pbm, 0, sizeof(struct pci_bar_mmap));
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
int
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
int
pci_open(
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

	LOGV("PCIOCGETCONF status = %d", pc->status);

	if (pc->status == PCI_GETCONF_ERROR) {
		errno = ENXIO;
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

int
pci_cfg_read(
	struct pci_ctx *pci,
	uint32_t *value,
	size_t offset,
	int width
	)
{
	if ((width != 1 && width != 2 && width != 4) || (offset % width) != 0) {
		LOG("Unaligned config space access");
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

int
pci_cfg_write(
	struct pci_ctx *pci,
	uint32_t *value,
	size_t offset,
	int width
	)
{
	if ((width != 1 && width != 2 && width != 4) || (offset % width) != 0) {
		LOG("Unaligned config space access");
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
int
pci_bar_mmap(
	struct pci_ctx *pci,
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
int
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
 * TODO: Support 64-bit BARs (they *sort of work*?)
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

	/* Need different mask for 0x30 */
	uint32_t base_mask = ((reg == 0x30) ? ~0x7FF : ~0xF);

	/* Save the register value */
	uint32_t bar_save;
	READ(&bar_save, reg, 4);

	/* Bail if it's all zeroes */
	if (bar_save == 0) {
		errno = 0;
		return (-1);
	}

	/* Get the base address from it */
	uint32_t bar_base = bar_save & base_mask;
	pci->pbm.pbm_bar_off = 0; /* Hmmm */

	/* Write all ones, PCI returns BAR length */
	uint32_t tmp = 0xFFFFFFFF;
	WRITE(&tmp, reg, 4);
	READ(&tmp, reg, 4);
	tmp &= base_mask;

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
		close(memfd);
		perror("mmap(/dev/mem)");
		return (-1);
	}

	return (0);
#undef READ
#undef WRITE
}

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
int
pci_bar_is_prefetchable(
	struct pci_ctx *pci,
	int reg
	)
{
	uint32_t b;
	if (pci_cfg_read(pci, &b, reg, 4) < 0) {
		return (-1);
	}

	return ((b & 0x8) != 0);
}

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
	)
{
	uint32_t b;
	if (pci_cfg_read(pci, &b, reg, 4) < 0) {
		return (-1);
	}

	return ((b & 0x1) != 0);
}
