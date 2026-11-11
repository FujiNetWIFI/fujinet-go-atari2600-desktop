/*
 * Watches, range parsing, memory-search queries and breakpoint descriptions,
 * kept in the core so the debugger windows only lay them out.
 *
 * A watch is text the user typed (an address, a label, an expression, or a
 * range lo-hi / lo..hi) shown with its live value. It is stored as text and
 * resolved every time it is shown, so loading symbols later makes a label
 * start resolving.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef A2600_MEMTOOLS_H
#define A2600_MEMTOOLS_H

#include <stdint.h>

#include "a2600debug.h"

#ifdef __cplusplus
extern "C" {
#endif

/* "lo..hi", "lo-hi" (plain numbers or labels on both sides), or one
 * expression (an address, label, label+3, [ptr]). A bare token of 1-4 hex
 * digits ("C000") is hex unless it names a label; "$C000" and expressions
 * work as in the console (MAME's evaluator). Returns 1 and fills lo and hi
 * (inclusive, lo <= hi), 0 if the text means nothing. */
int a2600sym_parse_range(a2600debug *d, const char *text, uint16_t *lo,
                         uint16_t *hi);

/* ---- watches ---- */
#define A2600WATCH_MAX 64
/* The process-wide list, like the symbol tables: it outlives the window. */
int  a2600watch_count(void);
/* Returns the new index, or -1 if empty / full. A watch that does not resolve
 * yet is kept (it shows "?"). */
int  a2600watch_add(const char *text);
void a2600watch_remove(int index);
void a2600watch_clear(void);
const char *a2600watch_text(int index);
unsigned a2600watch_generation(void);
/* "score  $0080  $42 (66)". While the machine runs the value is "-"; a range
 * shows its first 8 bytes. Returns the length. */
int  a2600watch_format(a2600debug *d, int index, char *dst, int dstsz);
/* The range a watch covers (for "show in Memory"). 1 if it resolves. */
int  a2600watch_range(a2600debug *d, int index, uint16_t *lo, uint16_t *hi);

/* ---- memory search ---- */
enum { A2600SEARCH_HEX = 0, A2600SEARCH_TEXT };
#define A2600SEARCH_MAX 32
/* Hex: byte pairs, optional "$" / "0x" / spaces. Text: one byte per
 * character, case-insensitive. Fills pat, n, mask (0xFF) and flags for
 * a2600debug_search. Returns 1, or 0 for empty / malformed text. */
int a2600search_parse(int kind, const char *text, uint8_t *pat, int *n,
                      uint8_t *mask, int *flags);

/* ---- breakpoints ---- */
/* "Write  $0080 (count)  if value != old"; read + write is "Access". */
int a2600bp_describe(a2600debug *d, const a2600debug_breakpoint *b, char *dst,
                     int dstsz);

/* ---- memory map ---- */
/* The name of the region of `map` (from a2600debug_memory_map) holding addr
 * ("RAM (zero page)", "Cartridge"), for labelling Memory rows. Fetch the map
 * once per refresh and look rows up in it. Returns the length, 0 and "" when
 * no region covers addr. A watch's range for "Show in Memory" is
 * a2600watch_range above. */
int a2600mem_region_name(const a2600debug_region *map, int count, uint16_t addr,
                         char *dst, int dstsz);

#ifdef __cplusplus
}
#endif

#endif /* A2600_MEMTOOLS_H */
