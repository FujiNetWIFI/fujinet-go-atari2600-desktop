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
/* Run until PC == addr ("runToPc"). */
void a2600debug_run_to(a2600debug *d, uint16_t addr);

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
    uint8_t pf0, pf1, pf2, grp0, grp1;
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

/* ---- disassembly --------------------------------------------------------------- */
typedef struct {
    uint16_t address;
    int type;              /* Device::AccessType bits: 1 CODE, 2 GFX, 4 PGFX, 8 COL... 0x10 DATA, 0x40 ROW */
    int is_pc;
    int has_breakpoint;
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

/* ---- breakpoints -------------------------------------------------------------- */
#define A2600DEBUG_ANY_BANK 0xffff
int  a2600debug_breakpoint_toggle(a2600debug *d, uint16_t addr, int bank);
int  a2600debug_breakpoint_check(a2600debug *d, uint16_t addr, int bank);
/* out[i] = address | bank << 16; returns the count. */
int  a2600debug_breakpoint_list(a2600debug *d, uint32_t *out, int max);
void a2600debug_breakpoint_clear(a2600debug *d);
/* Read/write traps on an address range. */
int  a2600debug_trap_read(a2600debug *d, uint16_t addr);
int  a2600debug_trap_write(a2600debug *d, uint16_t addr);

/* ---- states ------------------------------------------------------------------- */
void a2600debug_state_save(a2600debug *d, int slot);
void a2600debug_state_load(a2600debug *d, int slot);

/* ---- files -------------------------------------------------------------------- */
/* The save commands, with the path a native file picker supplied. Message
 * into msg; returns 0 or -1. kind: "dis" "rom" "access" "ses" "snap". */
int  a2600debug_save(a2600debug *d, const char *kind, const char *path,
                     char *msg, int msgsz);

#ifdef __cplusplus
}
#endif

#endif /* A2600DEBUG_H */
