/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Alexey Laurentsyeu <alex@paidbsd.org>
 */

#ifndef _BUILD_DEBUG_H
#define _BUILD_DEBUG_H

#ifdef DEBUG
#include <stdio.h>
#include <errno.h>

#  define LOG(STR) fprintf(stderr, "(DEBUG) %s:%d:%s: "STR"\n", \
	__FILE__, __LINE__, __func__)
#  define LOGV(FMT, ...) fprintf(stderr, "(DEBUG) %s:%d:%s: "FMT"\n", \
	__FILE__, __LINE__, __func__, __VA_ARGS__)
#else
#  define LOG(STR) (void)0
#  define LOGV(FMT, ...) (void)0
#endif

#endif /* _BUILD_DEBUG_H */
