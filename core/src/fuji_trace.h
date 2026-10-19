/*
 * fuji_trace -- the FujiNet link's bytes, both ways, decoded into FujiBus
 * packets: SLIP frames, the FujiBus header named, consecutive identical
 * exchanges folded into one entry each way. A ring of the newest 512.
 *
 * Pure: no emulator in here, so a test can drive it with synthetic bytes.
 * Every call locks; feed() runs on the cartridge's worker thread, the rest
 * on whichever thread reads.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef A2600_FUJI_TRACE_H
#define A2600_FUJI_TRACE_H

#include <stddef.h>
#include <stdint.h>

#include "a2600debug.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FUJI_TRACE_MAX 512

typedef struct fuji_trace fuji_trace;

fuji_trace *fuji_trace_new(void);
void fuji_trace_free(fuji_trace *t);
/* dir 1: the bytes the 2600 sent; dir 0: the bytes it received. */
void fuji_trace_feed(fuji_trace *t, int dir, const uint8_t *buf, size_t len);
/* The newest `max` packets, oldest first. Returns the count. */
int  fuji_trace_copy(fuji_trace *t, a2600_fuji_packet *out, int max);
void fuji_trace_clear(fuji_trace *t);
/* Packets and bytes each way since the trace was made ([0] from the 2600,
 * [1] from FujiNet); Clear leaves these alone. */
void fuji_trace_totals(fuji_trace *t, uint64_t packets[2], uint64_t bytes[2]);

#ifdef __cplusplus
}
#endif

#endif /* A2600_FUJI_TRACE_H */
