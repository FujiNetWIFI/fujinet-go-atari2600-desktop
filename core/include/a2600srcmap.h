/* a2600srcmap.h - batari Basic source lines <-> 6502 addresses.
 *
 * batari Basic (2600basic) writes ".<label> ;;line N;; <text>" ahead of the
 * code of every BASIC source line (N = the line in the .bas file), and dasm's
 * listing (`-lX.bas.lst`) shows each marker next to the address it assembled
 * at. This module reads that listing and answers "which BASIC line is the
 * code at (bank, address)?" and "where does line N start?".
 *
 * Listing addresses are ORG file offsets, not the addresses the 6502 sees:
 * for a multi-bank cart (first assembled ORG is $1000) the bank is
 * (offset >> 12) - 1 and the run-time address is the offset's low 12 bits
 * ($Fxxx); for a 2K/4K cart (first assembled ORG $F000) there is one bank
 * (0). Lines the assembler skipped (flagged '-' in the listing) are ignored.
 * Everything here is keyed by (bank, address & 0xFFF).
 *
 * Compilers older than commit f15dcb6 (batari Basic <= 1.7) write
 * ".L05 ; <text>" with no line number. Those listings are matched to the
 * source by text (whitespace removed, comments stripped), in order.
 *
 * A line "has code" only when the next marker has a larger address; a label
 * or `rem` line shares its address with the following line, which wins.
 *
 * Thread safety: every call locks; is_line_start and lookup are called once
 * per instruction from the emulator thread, so they take no allocation.
 */
#ifndef A2600SRCMAP_H
#define A2600SRCMAP_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct a2600srcmap a2600srcmap;

a2600srcmap* a2600srcmap_create(void);
void a2600srcmap_destroy(a2600srcmap* m);
/* The process-wide map the debugger and the windows share. */
a2600srcmap* a2600srcmap_shared(void);
void a2600srcmap_clear(a2600srcmap* m);

typedef struct a2600srcmap_import_result
{
  int files;              /* .bas files loaded */
  int mapped;             /* source lines that have code */
  int old_format;         /* the listing had no line numbers: matched by text */
  char map_path[512];     /* the .lst used ("" if none was found) */
  char sym_path[512];     /* the .sym beside it ("" if none) */
} a2600srcmap_import_result;

/* path = a .bas file, or a folder (every .bas inside, depth <= 8). Finds the
 * listing: <file>.lst, <stem>.lst, <file>.bas.lst, then any .lst in the
 * folder naming the most loaded files. 0 = ok (even with no map: the source
 * still shows, nothing is mapped), -1 = unreadable path. Builds on the side
 * and swaps in, so a failure leaves the old map. */
int a2600srcmap_import(a2600srcmap* m, const char* path, a2600srcmap_import_result* out);
/* Load (or replace) the listing only; the sources must be imported already.
 * Returns the number of source lines now mapped, or -1. */
int a2600srcmap_load_map(a2600srcmap* m, const char* lst_path);

/* Bumps on every import/load/clear: windows re-render when it changes. */
unsigned a2600srcmap_generation(const a2600srcmap* m);

int a2600srcmap_file_count(const a2600srcmap* m);
const char* a2600srcmap_file_name(const a2600srcmap* m, int file);   /* display name; valid until the next import */
int a2600srcmap_line_count(const a2600srcmap* m, int file);
const char* a2600srcmap_line_text(const a2600srcmap* m, int file, int line);   /* 1-based; no newline; "" past the end */
/* 1 if line has code. */
int a2600srcmap_line_has_code(const a2600srcmap* m, int file, int line);

/* bank = the cartridge's current bank (Stella getBank), addr = the pc. */
int a2600srcmap_lookup(const a2600srcmap* m, int bank, unsigned addr, int* file, int* line);   /* 1 if mapped */
int a2600srcmap_is_line_start(const a2600srcmap* m, int bank, unsigned addr);
/* The lowest run-time address of the line's code ($Fxxx), with its bank in
 * *bank; -1 if the line has no code. */
int a2600srcmap_first_addr(const a2600srcmap* m, int file, int line, int* bank);

#ifdef __cplusplus
}
#endif
#endif
