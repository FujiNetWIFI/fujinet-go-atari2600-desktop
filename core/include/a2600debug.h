/*
 * a2600debug -- the debugger contract: Stella's own debugger engine (the
 * 112-command DebuggerParser, CpuDebug, RiotDebug, TIADebug, CartDebug and
 * DiStella) behind a C API the four native debugger windows share.
 *
 * Every call runs on the Stella thread and blocks the caller until done;
 * the machine is stopped by a2600debug_stop() (or a breakpoint) and the
 * inspection calls are meant for that state. While it runs they still
 * answer, from a frame boundary.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef A2600DEBUG_H
#define A2600DEBUG_H

#include <stdint.h>

#include "a2600session.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Lazily created; lives as long as the session. */
a2600debug *a2600debug_get(a2600session *s);

/* ---- stop / go ------------------------------------------------------------- */
int  a2600debug_is_stopped(a2600debug *d);
void a2600debug_stop(a2600debug *d);        /* break at the next instruction */
void a2600debug_resume(a2600debug *d);      /* "run" */
/* Why the machine last stopped: message from Stella ("BP: ...", a trap, a
 * fatal error) and the address, -1 if none. Returns length. */
int  a2600debug_stop_reason(a2600debug *d, char *dst, int dstsz, int *address);
/* Bumped on every new stop (a breakpoint, a trap, Pause, a step), so a window
 * can recentre its views on it; a refresh or an unrelated command does not
 * move it. */
uint64_t a2600debug_stop_serial(a2600debug *d);
/* Bumped whenever a command ran or the machine stopped/resumed, so a window
 * knows when to refresh. */
unsigned a2600debug_generation(a2600debug *d);

/* ---- the prompt ------------------------------------------------------------ */
/* Run one DebuggerParser command ("step", "trace", "frame 3", "breakIf
 * {a==$ff}", "help", ...). Output text into dst; returns its length. Stops
 * the machine first if it is running. The commands that would open one of
 * Stella's own file dialogs when given no path (save*, dump) must be given
 * one; exitRom and stepWhile are refused. */
int  a2600debug_command(a2600debug *d, const char *command, char *dst, int dstsz);
/* Completions for a prefix, one per line. Returns the count. */
int  a2600debug_completions(a2600debug *d, const char *prefix, char *dst, int dstsz);

/* ---- stepping shortcuts (the toolbar) --------------------------------------- */
void a2600debug_step(a2600debug *d);         /* one instruction */
void a2600debug_trace(a2600debug *d);        /* step over a JSR */
void a2600debug_scanline(a2600debug *d, int n);
void a2600debug_frame(a2600debug *d, int n);
void a2600debug_rewind(a2600debug *d, int n);
void a2600debug_unwind(a2600debug *d, int n);
/* F8 and Shift+F8: over a JSR (Stella's "trace"), and out of the routine
 * (runToPc of the return address on the stack). */
void a2600debug_step_over(a2600debug *d);
void a2600debug_step_out(a2600debug *d);
/* Run until PC == addr ("runToPc"). */
void a2600debug_run_to(a2600debug *d, uint16_t addr);

/* ---- batari Basic source lines (a2600srcmap.h holds the map) -------------------
 * Step / Over / Out by BASIC line: run until the first instruction of a mapped
 * line (Over: not inside a call; Out: after the routine returned). They return
 * at once, like run_to; the stop reason reads "BASIC line main.bas:7 at $F461".
 * A user breakpoint, a pause or any other stop ends the step. */
void a2600debug_step_line(a2600debug *d);
void a2600debug_step_line_over(a2600debug *d);
void a2600debug_step_line_out(a2600debug *d);
/* The BASIC line the pc is in: 1 and file/line (a2600srcmap indices), else 0. */
int  a2600debug_line_at_pc(a2600debug *d, int *file, int *line);
/* Toggle the execute breakpoint at a line's first instruction (1 set, 0
 * cleared, -1 the line has no code); its state: 0 none, 1 enabled, 2 disabled. */
int  a2600debug_line_break_toggle(a2600debug *d, int file, int line);
int  a2600debug_line_break_state(a2600debug *d, int file, int line);
/* Load a dasm symbol file (X.bas.sym) from any path, replacing the user
 * labels. Returns the labels added, or -1 (msg says why). */
int  a2600debug_load_symbols_path(a2600debug *d, const char *path, char *msg, int msgsz);

/* ---- CPU --------------------------------------------------------------------- */
typedef struct {
    int pc, sp, a, x, y, ps;
    int n, v, b, d, i, z, c;
    int cycles;        /* cycles of the last instruction */
    uint64_t total_cycles;
} a2600debug_cpu;
void a2600debug_cpu_get(a2600debug *d, a2600debug_cpu *out);
typedef enum { A2600_REG_PC, A2600_REG_SP, A2600_REG_A, A2600_REG_X, A2600_REG_Y,
               A2600_REG_PS, A2600_FLAG_N, A2600_FLAG_V, A2600_FLAG_B, A2600_FLAG_D,
               A2600_FLAG_I, A2600_FLAG_Z, A2600_FLAG_C } a2600debug_reg;
void a2600debug_cpu_set(a2600debug *d, int reg, int value);

/* ---- RIOT -------------------------------------------------------------------- */
typedef struct {
    uint8_t swcha, swacnt, swchb, swbcnt;
    uint8_t inpt[6];
    uint8_t intim, timint;
    int tim_clocks, tim_divider;
    int select, reset, color, diff_left_a, diff_right_a;   /* switch positions */
    char dir_left[16], dir_right[16];                      /* joystick directions */
} a2600debug_riot;
void a2600debug_riot_get(a2600debug *d, a2600debug_riot *out);
typedef enum { A2600_RIOT_SWCHA, A2600_RIOT_SWACNT, A2600_RIOT_SWCHB, A2600_RIOT_SWBCNT,
               A2600_RIOT_TIM1T, A2600_RIOT_TIM8T, A2600_RIOT_TIM64T, A2600_RIOT_TIM1024T,
               A2600_RIOT_SELECT, A2600_RIOT_RESET, A2600_RIOT_COLOR,
               A2600_RIOT_DIFF_LEFT, A2600_RIOT_DIFF_RIGHT } a2600debug_riot_reg;
void a2600debug_riot_set(a2600debug *d, int reg, int value);

/* ---- TIA --------------------------------------------------------------------- */
typedef struct {
    uint8_t nusiz0, nusiz1, colup0, colup1, colupf, colubk, ctrlpf;
    uint8_t pf0, pf1, pf2, graphics_p0, graphics_p1;
    uint8_t pos_p0, pos_p1, pos_m0, pos_m1, pos_bl;
    uint8_t hm_p0, hm_p1, hm_m0, hm_m1, hm_bl;
    uint8_t audc0, audc1, audf0, audf1, audv0, audv1;
    char aud_freq0[24], aud_freq1[24];
    int refp0, refp1, enam0, enam1, enabl, vdelp0, vdelp1, vdelbl;
    int resmp0, resmp1, refpf, scorepf, pripf, vsync, vblank;
    uint16_t collisions;      /* bit i = CollisionBit i: M0P1 M0P0 M1P0 M1P1 P0PF P0BL P1PF P1BL M0PF M0BL M1PF M1BL BLPF P0P1 M0M1 */
    int scanlines, scanlines_last, frame_count, frame_cycles, wsync_cycles;
    int clocks_this_line, cycles_this_line;
    int beam_x, beam_y;       /* electron beam, or -1 when off-screen */
} a2600debug_tia;
void a2600debug_tia_get(a2600debug *d, a2600debug_tia *out);
/* Registers settable by name, exactly as TIADebug names them ("colup0",
 * "pf1", "posp0", "refp0", "vsync", ...). Returns 0 or -1. */
int  a2600debug_tia_set(a2600debug *d, const char *reg, int value);
/* Strobes: "wsync" "rsync" "resp0" "resp1" "resm0" "resm1" "resbl" "hmove" "hmclr" "cxclr". */
int  a2600debug_tia_strobe(a2600debug *d, const char *name);
/* TIA colour index (0-255) to XRGB for the current palette. */
uint32_t a2600debug_tia_color(a2600debug *d, uint8_t index);
/* The frame as the TIA sees it right now: the last complete frame, or with
 * `partial` the one being drawn (to the beam position). dst holds
 * A2600SESSION_FB_WIDTH*A2600SESSION_FB_MAX_HEIGHT XRGB pixels; returns the
 * line count. */
int  a2600debug_frame_snapshot(a2600debug *d, uint32_t *dst, int partial);

/* ---- memory ------------------------------------------------------------------ */
/* Bus reads without side effects; writes go through the bus (a RAM edit). */
int  a2600debug_read(a2600debug *d, uint16_t addr, uint8_t *dst, int n);
void a2600debug_write(a2600debug *d, uint16_t addr, uint8_t value);
/* The 128 bytes of zero-page RAM ($80-$FF). */
void a2600debug_ram_get(a2600debug *d, uint8_t out[128]);
/* Patch the cartridge image itself (survives a bank switch). */
int  a2600debug_patch_rom(a2600debug *d, uint16_t addr, uint8_t value);
/* Stella's expression evaluator (registers a x y pc, labels, $hex, @addr,
 * _old/_value ...; `old` and `value` also work): 0 and *out, or -1 with a
 * message in err. */
int  a2600debug_eval(a2600debug *d, const char *expression, long *out,
                     char *err, int errsz);

/* ---- disassembly --------------------------------------------------------------- */
typedef struct {
    uint16_t address;
    int type;              /* Device::AccessType bits: 1 CODE, 2 GFX, 4 PGFX, 8 COL... 0x10 DATA, 0x40 ROW */
    int is_pc;
    int has_breakpoint;    /* an enabled execute breakpoint covers it (in this bank) */
    int bp_disabled;       /* only a disabled one does (the grey dot) */
    char bytes[24];
    char label[32];
    char disasm[48];
    char cycles[8];        /* cycle count of the instruction */
    char cycles_total[12];
} a2600debug_line;
/* Disassemble `bank` (-1 = the bank holding the PC): the whole bank's
 * listing is prepared and `max` lines from line `first` are copied out.
 * Returns the count copied; *total is the listing's length and *pc_line the
 * PC's absolute line index (-1 when the PC is not in this bank), so a
 * window can scroll to it. */
int  a2600debug_disassemble(a2600debug *d, int bank, int first,
                            a2600debug_line *out, int max,
                            int *total, int *pc_line);
int  a2600debug_bank_count(a2600debug *d);
int  a2600debug_current_bank(a2600debug *d);
/* Address of a label, or -1. Label of an address into dst (may be empty). */
int  a2600debug_label_address(a2600debug *d, const char *label);
int  a2600debug_address_label(a2600debug *d, uint16_t addr, char *dst, int dstsz);
/* Load <rom>.sym / <rom>.lst next to the cartridge, as Stella does. */
int  a2600debug_load_symbols(a2600debug *d, char *msg, int msgsz);
/* Cartridge: scheme name and bank state, one line. */
int  a2600debug_cart_info(a2600debug *d, char *dst, int dstsz);
/* Define a label (a Symbols-tab edit); 0, or -1 if refused. */
int  a2600debug_set_label(a2600debug *d, uint16_t addr, const char *label);

/* ---- symbols (user labels: a loaded .sym/.lst, "define", set_label; Stella's
 * system equates (WSYNC ...) are not listed) -------------------------------- */
int  a2600debug_symbols_count(a2600debug *d);
/* Entry `index` (0 .. count-1): address and name. Returns 1, or 0 past the
 * end. The indices are unstable (a load or clear reorders them): filter on
 * every call. */
int  a2600debug_symbols_get(a2600debug *d, int index, uint16_t *addr, char *name, int namesz);
/* Indices of the entries whose name contains `text` (case-insensitive; empty
 * matches all), by address then name. Returns the count written. */
int  a2600debug_symbols_filter(a2600debug *d, const char *text, int *out, int max);
/* Forget every user label; the disassembly drops them at once. */
void a2600debug_clear_symbols(a2600debug *d);

/* ---- breakpoints and traps ---------------------------------------------------- */
/* The core keeps the table (Stella has no ids, no hit counts, no disabled
 * state): an enabled entry is installed in Stella (BreakpointMap, a hidden
 * breakIf or a trap), a disabled one is only kept here. Anything set at the
 * console ("break", "breakIf", "trapWrite" ...) is adopted into the table, and
 * one removed there ("clearBreaks", "delBreakIf" ...) leaves it. After a
 * cartridge swap the enabled entries are installed again. */
#define A2600DEBUG_ANY_BANK 0xffff
typedef enum {
    A2600DEBUG_BP_EXEC = 1, A2600DEBUG_BP_READ = 2, A2600DEBUG_BP_WRITE = 4,
    /* Read and write as one entry (Stella's "trap"): READ | WRITE */
    A2600DEBUG_BP_ACCESS = 6
} a2600debug_bp_type;
typedef struct {
    int id;                /* 1, 2, 3 ... (never reused) */
    int type;              /* a2600debug_bp_type bits */
    uint16_t start, end;   /* EXEC: the address (end >= start: any pc in it) */
    int bank;              /* EXEC only: a bank, or A2600DEBUG_ANY_BANK */
    int enabled;
    unsigned hits;         /* stops it caused, counted here */
    /* A Stella expression ("a==$ff", "_value != _old"); `old` and `value`
     * are accepted for `_old` and `_value`. A condition-only break the
     * console set ("breakIf {x>3}") is EXEC $0000-$FFFF. */
    char condition[128];
} a2600debug_breakpoint;
/* An execute breakpoint (no condition) at addr in bank: added if absent,
 * removed if present. Returns 1 when one is now set. */
int  a2600debug_breakpoint_toggle(a2600debug *d, uint16_t addr, int bank);
/* An enabled execute breakpoint at addr in bank. */
int  a2600debug_breakpoint_check(a2600debug *d, uint16_t addr, int bank);
/* The enabled plain execute breakpoints: out[i] = address | bank << 16;
 * returns the count. (a2600debug_bp_list has them all.) */
int  a2600debug_breakpoint_list(a2600debug *d, uint32_t *out, int max);
/* Clears every entry, Stella's breakpoints and traps included. */
void a2600debug_breakpoint_clear(a2600debug *d);
/* Read/write traps as Stella has them on an address. */
int  a2600debug_trap_read(a2600debug *d, uint16_t addr);
int  a2600debug_trap_write(a2600debug *d, uint16_t addr);

/* Adds an entry (enabled): EXEC at start (bank), READ/WRITE/ACCESS over
 * start..end. An identical entry is returned, enabled, instead of a copy.
 * Returns the id, or -1 if the condition does not parse. */
int  a2600debug_bp_add(a2600debug *d, int type, uint16_t start, uint16_t end,
                       int bank, const char *condition);
void a2600debug_bp_remove(a2600debug *d, int id);
void a2600debug_bp_enable(a2600debug *d, int id, int enabled);
/* All entries in id order; returns the count (up to max). */
int  a2600debug_bp_list(a2600debug *d, a2600debug_breakpoint *out, int max);
/* Zero every hit count. */
void a2600debug_bp_clear_hits(a2600debug *d);

/* ---- the trace ring ------------------------------------------------------------ */
typedef struct {
    uint16_t pc;
    uint8_t a, x, y, p, sp;  /* the registers before the instruction ran */
    uint8_t bytes[3];        /* its first bytes (0 where unreadable) */
    int16_t bank;            /* the cartridge bank holding pc */
    uint64_t cycles;         /* system cycles */
    int16_t scanline, dot;   /* the beam: frame line, colour clock 0-227 (0-67 is HBLANK) */
} a2600debug_trace_entry;
/* The ring keeps the newest 65536; allocated on the first enable and filled
 * from Stella's instruction tap (reinstalled after a cartridge swap). */
void a2600debug_trace_enable(a2600debug *d, int on);
int  a2600debug_trace_enabled(a2600debug *d);
/* Newest first; returns entries copied. */
int  a2600debug_trace_read(a2600debug *d, a2600debug_trace_entry *out, int max);
void a2600debug_trace_clear(a2600debug *d);
/* Entries held. */
int  a2600debug_trace_count(a2600debug *d);
/* Pure (memtools.c): "F017  JSR $F020  A=00 X=01 Y=00 P=34 SP=FF". */
int  a2600debug_trace_format(const a2600debug_trace_entry *t, char *dst, int dstsz);
enum { A2600DEBUG_TRACE_ALL = 0, A2600DEBUG_TRACE_BRANCHES, A2600DEBUG_TRACE_RANGE };
/* 1 if the entry passes the filter (BRANCHES: jumps, calls, returns and
 * conditional branches; RANGE: pc in lo..hi, either way round). Pure. */
int  a2600debug_trace_match(const a2600debug_trace_entry *t, int filter, uint16_t lo, uint16_t hi);

/* ---- memory search and the memory map ----------------------------------------- */
#define A2600DEBUG_SEARCH_NOCASE 1
/* The 6507's bus is 13 bits: $0000-$1FFF. Stopped only (-1 while running).
 * Finds up to max start addresses in from..to (clamped to $1FFF) as mapped
 * now, where (mem & mask) == (pat & mask) for all n bytes (`to` bounds the
 * start, not the end). Returns the count. */
int  a2600debug_search(a2600debug *d, const uint8_t *pat, int n, uint8_t mask,
                       int flags, uint16_t from, uint16_t to, uint16_t *out, int max);
/* The next match after `from` (before it if backward), wrapping once.
 * Returns 1 and *found, 0 for none, -1 while running. */
int  a2600debug_search_next(a2600debug *d, const uint8_t *pat, int n, uint8_t mask,
                            int flags, uint16_t from, int backward, uint16_t *found);

typedef struct {
    uint16_t lo, hi;
    int readable, writable;
    char name[48];
} a2600debug_region;
/* Ordered, non-overlapping regions covering $0000-$1FFF exactly: TIA, RIOT
 * RAM and I/O, their mirrors, and the cartridge by bank ("Cartridge bank 2"),
 * as the page table maps them now. Returns the count (up to max). */
int  a2600debug_memory_map(a2600debug *d, a2600debug_region *out, int max);

/* ---- sound ---------------------------------------------------------------------- */
typedef struct {
    uint8_t audc[2], audf[2], audv[2];   /* the TIA's two channels */
    char freq[2][24];                    /* Stella's frequency text ("1.9 kHz") */
} a2600debug_sound;
void a2600debug_sound_get(a2600debug *d, a2600debug_sound *out);

/* ---- states ------------------------------------------------------------------- */
void a2600debug_state_save(a2600debug *d, int slot);
void a2600debug_state_load(a2600debug *d, int slot);

/* ---- files -------------------------------------------------------------------- */
/* The save commands, with the path a native file picker supplied. Message
 * into msg; returns 0 or -1. kind: "dis" "rom" "access" "ses" "snap". */
int  a2600debug_save(a2600debug *d, const char *kind, const char *path,
                     char *msg, int msgsz);

/* ---- FujiNet: the cartridge's link, as FujiBus packets ------------------------ */
/* Always on from session start. Consecutive identical exchanges fold into
 * one entry each way, counted in `repeat` (and shown as "x3" in text). */
typedef struct {
    uint64_t serial;        /* bumped again when a repeat folds in */
    int      to_host;       /* 1: FujiNet -> 2600, 0: 2600 -> FujiNet */
    int      device, command, length;   /* from the FujiBus header, -1 if short */
    int      nbytes;
    uint8_t  data[64];      /* the decoded packet's first bytes */
    char     text[96];      /* "2600 -> FujiNet 70 F4 READ HOST SLOTS (6 bytes)" */
    uint32_t repeat;
} a2600_fuji_packet;
/* The newest `max` packets, oldest first. Returns the count. */
int  a2600debug_fujinet_trace(a2600debug *d, a2600_fuji_packet *out, int max);
void a2600debug_fujinet_trace_clear(a2600debug *d);
/* The link's state as text lines: FujiNet, the cartridge, its link status,
 * whether a game was booted, the traffic so far. Returns the length. */
int  a2600debug_fujinet_state(a2600debug *d, char *dst, int dstsz);

#ifdef __cplusplus
}
#endif

#endif /* A2600DEBUG_H */
