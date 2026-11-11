/*
 * vidtab.c -- the Video tab's controller (core/include/a2600vidtab.h).
 *
 * The TIA's priority encoder is repeated here for the rows the host captures
 * (a2600vid_decode_row), so the Beam view can redraw the part of this frame
 * the beam has drawn, Decoded can say which object drew each pixel, and the
 * inspector can say what else was on at that colour clock.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "a2600vidtab.h"

#include <ctype.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "a2600vidcap.h"

#define MAXPIX 6000000L         /* the largest picture we will build */
#define MAX_REGS 40
#define MAX_TIMING 14
#define MAX_ROW_POKES 512       /* writes one row's replay will apply */
#define PALETTE_CELL 20         /* pixels of a palette grid cell */
#define OBJECT_TINT 100         /* of 256: an object's colour tints the dimmed frame */
#define DEFAULT_H 192
#define WIDTH_PX (A2600VID_FB_W * A2600VID_XSCALE)

/* TIA write registers by address */
enum {
    REG_VSYNC = 0x00, REG_VBLANK = 0x01, REG_NUSIZ0 = 0x04, REG_NUSIZ1 = 0x05,
    REG_COLUP0 = 0x06, REG_COLUP1 = 0x07, REG_COLUPF = 0x08, REG_COLUBK = 0x09,
    REG_CTRLPF = 0x0A, REG_REFP0 = 0x0B, REG_REFP1 = 0x0C, REG_PF0 = 0x0D,
    REG_PF1 = 0x0E, REG_PF2 = 0x0F, REG_RESP0 = 0x10, REG_RESP1 = 0x11,
    REG_RESM0 = 0x12, REG_RESM1 = 0x13, REG_RESBL = 0x14, REG_GRP0 = 0x1B,
    REG_GRP1 = 0x1C, REG_ENAM0 = 0x1D, REG_ENAM1 = 0x1E, REG_ENABL = 0x1F,
    REG_HMP0 = 0x20, REG_HMBL = 0x24, REG_VDELP0 = 0x25, REG_VDELP1 = 0x26,
    REG_VDELBL = 0x27, REG_RESMP0 = 0x28, REG_RESMP1 = 0x29, REG_HMOVE = 0x2A,
    REG_HMCLR = 0x2B
};

static const int k_zooms[] = { 0, 1, 2, 3, 4, 6, 8, 12, 16 };

enum { BK_FRAME = 0, BK_PALETTE };   /* what the base picture is */

struct a2600vid_tab {
    a2600session *sess;
    a2600debug *dbg;
    int view, source, zoom, view_w, view_h;
    unsigned overlays;
    int env_section, capture_on;

    /* the machine's state as presented */
    a2600vid_capture *cap;      /* working frame */
    a2600vid_capture *last;     /* last captured by the emulation thread */
    uint64_t last_serial;
    int have, has_fb, has_live, stopped, fed, cap_is_last;
    int fb_h;
    uint64_t sig;
    uint32_t rgbtab[256];
    int rgb_valid;

    /* the picture */
    uint32_t *base, *pic;
    size_t base_cap, pic_cap;
    int base_w, base_h, pic_w, pic_h, eff_zoom, kind;
    int dirty;
    char why[200];

    /* tables */
    a2600vid_reg regs[MAX_REGS];
    char prev_val[MAX_REGS][24];
    int nregs, regs_primed;
    a2600vid_swatch swatches[A2600VID_O_COUNT];
    long uses[A2600VID_O_COUNT];
    long colour_uses[128];
    a2600vid_line_row rows[A2600VID_ROWS];
    int nrows, nchanges;
    a2600vid_write *writes;
    int nwrites, dropped;
    char status[400];
    char timing_buf[MAX_TIMING][112];
    const char *timing_ptr[MAX_TIMING];
    int ntiming;

    /* pixels */
    int hover_x, hover_y, pin_x, pin_y;         /* -1: none */
    a2600vid_pixel hover, pin;
};

/* ---- tables of names ----------------------------------------------------------------- */

static const a2600vid_overlay_info k_overlays[] = {
    { A2600VID_OV_BEAM,      "beam",      "Beam" },
    { A2600VID_OV_WRITES,    "writes",    "TIA writes" },
    { A2600VID_OV_CHANGED,   "changed",   "Register changes" },
    { A2600VID_OV_POSITIONS, "positions", "Object positions" },
    { A2600VID_OV_BLANK,     "blank",     "VBLANK rows" },
};
#define N_OVERLAYS ((int)(sizeof k_overlays / sizeof *k_overlays))

const a2600vid_overlay_info *a2600vid_overlays(int *count)
{
    if (count) *count = N_OVERLAYS;
    return k_overlays;
}

static const char *const k_view_names[] = { "Display", "Decoded", "Beam", "Values" };
static const char *const k_view_keys[] = { "display", "decoded", "beam", "values" };
static const char *const k_src_names[] = { "Objects", "Palette" };
static const char *const k_src_keys[] = { "objects", "palette" };
static const char *const k_sec_names[] = { "Pixel", "Registers", "Colours", "Lines", "Writes", "Timing" };
static const char *const k_sec_keys[] = { "pixel", "registers", "colours", "lines", "writes", "timing" };
static const char *const k_obj_names[] = { "BK", "PF", "P0", "P1", "M0", "M1", "BL" };
static const char *const k_obj_regs[] = { "COLUBK", "COLUPF", "COLUP0", "COLUP1", "COLUP0", "COLUP1", "COLUPF" };
static const unsigned k_obj_addr[] = { REG_COLUBK, REG_COLUPF, REG_COLUP0, REG_COLUP1, REG_COLUP0, REG_COLUP1, REG_COLUPF };

const char *a2600vid_view_name(int view) { return view >= 0 && view < A2600VID_V_COUNT ? k_view_names[view] : ""; }
const char *a2600vid_view_key(int view) { return view >= 0 && view < A2600VID_V_COUNT ? k_view_keys[view] : ""; }
const char *a2600vid_source_name(int src) { return src >= 0 && src < A2600VID_D_COUNT ? k_src_names[src] : ""; }
const char *a2600vid_source_key(int src) { return src >= 0 && src < A2600VID_D_COUNT ? k_src_keys[src] : ""; }
const char *a2600vid_section_name(int sec) { return sec >= 0 && sec < A2600VID_S_COUNT ? k_sec_names[sec] : ""; }
const char *a2600vid_section_key(int sec) { return sec >= 0 && sec < A2600VID_S_COUNT ? k_sec_keys[sec] : ""; }
const char *a2600vid_object_name(int obj) { return obj >= 0 && obj < A2600VID_O_COUNT ? k_obj_names[obj] : ""; }

const int *a2600vid_zooms(int *count)
{
    if (count) *count = (int)(sizeof k_zooms / sizeof *k_zooms);
    return k_zooms;
}

const char *a2600vid_reg_name(unsigned reg)
{
    static const char *const names[0x2D] = {
        "VSYNC", "VBLANK", "WSYNC", "RSYNC", "NUSIZ0", "NUSIZ1", "COLUP0", "COLUP1", "COLUPF", "COLUBK",
        "CTRLPF", "REFP0", "REFP1", "PF0", "PF1", "PF2", "RESP0", "RESP1", "RESM0", "RESM1", "RESBL",
        "AUDC0", "AUDC1", "AUDF0", "AUDF1", "AUDV0", "AUDV1", "GRP0", "GRP1", "ENAM0", "ENAM1", "ENABL",
        "HMP0", "HMP1", "HMM0", "HMM1", "HMBL", "VDELP0", "VDELP1", "VDELBL", "RESMP0", "RESMP1", "HMOVE",
        "HMCLR", "CXCLR" };
    static char other[64][8];
    reg &= 0x3F;
    if (reg < sizeof names / sizeof *names) return names[reg];
    snprintf(other[reg], sizeof other[reg], "$%02X", reg);
    return other[reg];
}

/* ---- small helpers ---------------------------------------------------------------------- */

static uint64_t fnv(uint64_t hash, const void *data, size_t len)
{
    const uint8_t *bytes = data;
    while (len--) { hash ^= *bytes++; hash *= 1099511628211ull; }
    return hash;
}

static uint32_t blend(uint32_t dst, uint32_t rgb, int alpha)
{
    const uint32_t dst_r = (dst >> 16) & 255, dst_g = (dst >> 8) & 255, dst_b = dst & 255;
    const uint32_t src_r = (rgb >> 16) & 255, src_g = (rgb >> 8) & 255, src_b = rgb & 255;
    const uint32_t red = (dst_r * (uint32_t)(256 - alpha) + src_r * (uint32_t)alpha) >> 8;
    const uint32_t grn = (dst_g * (uint32_t)(256 - alpha) + src_g * (uint32_t)alpha) >> 8;
    const uint32_t blu = (dst_b * (uint32_t)(256 - alpha) + src_b * (uint32_t)alpha) >> 8;
    return 0xff000000u | red << 16 | grn << 8 | blu;
}

static int fits_zoom(int wide, int high, int zoom)
{
    return (long)wide * zoom * (long)high * zoom <= MAXPIX;
}

static void why_set(a2600vid_tab *tab, const char *text)
{
    snprintf(tab->why, sizeof tab->why, "%s", text);
}

static int ensure(uint32_t **buf, size_t *cap, size_t need)
{
    if (need > *cap) {
        uint32_t *grown = realloc(*buf, need * sizeof **buf);
        if (!grown) return 0;
        *buf = grown;
        *cap = need;
    }
    return 1;
}

static int clampi(int val, int low, int high) { return val < low ? low : val > high ? high : val; }

/* false colours for a TIA colour byte: a hue wheel by the high nibble (0 is
 * grey), brightness by the luminance bits */
uint32_t a2600vid_fallback_colour(uint8_t index)
{
    const int hue = index >> 4, lum = (index >> 1) & 7;
    const float bright = 0.22f + 0.78f * (float)lum / 7.0f;
    float red, grn, blu;
    if (hue == 0) {
        red = grn = blu = bright;
    } else {
        const float angle = (float)(hue - 1) * 6.0f / 15.0f;       /* 0..6 around the wheel */
        const int sector = (int)angle;
        const float frac = angle - (float)sector;
        const float sat = 0.85f;
        const float low = bright * (1.0f - sat), fall = bright * (1.0f - sat * frac), rise = bright * (1.0f - sat * (1.0f - frac));
        switch (sector) {
        case 0: red = bright; grn = rise; blu = low; break;
        case 1: red = fall; grn = bright; blu = low; break;
        case 2: red = low; grn = bright; blu = rise; break;
        case 3: red = low; grn = fall; blu = bright; break;
        case 4: red = rise; grn = low; blu = bright; break;
        default: red = bright; grn = low; blu = fall; break;
        }
    }
    return 0xff000000u | (uint32_t)(red * 255.0f) << 16 | (uint32_t)(grn * 255.0f) << 8 | (uint32_t)(blu * 255.0f);
}

/* the console's colour for a TIA colour byte; the debugger's table when there is one */
static uint32_t console_rgb(a2600vid_tab *tab, uint8_t index)
{
    if (!tab->rgb_valid) {
        for (int idx = 0; idx < 256; idx++)
            tab->rgbtab[idx] = tab->dbg ? (a2600debug_tia_color(tab->dbg, (uint8_t)idx) | 0xff000000u)
                                        : a2600vid_fallback_colour((uint8_t)idx);
        tab->rgb_valid = 1;
    }
    return tab->rgbtab[index];
}

/* ---- the TIA's priority encoder -------------------------------------------------------- */

typedef struct {
    uint8_t colup0, colup1, colupf, colubk, pf0, pf1, pf2, ctrlpf;
    uint8_t grp0, grp1, grp0_old, grp1_old, nusiz0, nusiz1;
    uint8_t enam0, enam1, enabl, enabl_old;
    uint8_t refp0, refp1, vdelp0, vdelp1, vdelbl, resmp0, resmp1, vblank;
} regstate;

/* store a register's value, nothing else */
static void reg_store(regstate *state, unsigned reg, uint8_t val)
{
    switch (reg) {
    case REG_VBLANK: state->vblank = (val >> 1) & 1; break;
    case REG_NUSIZ0: state->nusiz0 = val; break;
    case REG_NUSIZ1: state->nusiz1 = val; break;
    case REG_COLUP0: state->colup0 = val; break;
    case REG_COLUP1: state->colup1 = val; break;
    case REG_COLUPF: state->colupf = val; break;
    case REG_COLUBK: state->colubk = val; break;
    case REG_CTRLPF: state->ctrlpf = val; break;
    case REG_REFP0: state->refp0 = (val >> 3) & 1; break;
    case REG_REFP1: state->refp1 = (val >> 3) & 1; break;
    case REG_PF0: state->pf0 = val; break;
    case REG_PF1: state->pf1 = val; break;
    case REG_PF2: state->pf2 = val; break;
    case REG_GRP0: state->grp0 = val; break;
    case REG_GRP1: state->grp1 = val; break;
    case REG_ENAM0: state->enam0 = (val >> 1) & 1; break;
    case REG_ENAM1: state->enam1 = (val >> 1) & 1; break;
    case REG_ENABL: state->enabl = (val >> 1) & 1; break;
    case REG_VDELP0: state->vdelp0 = val & 1; break;
    case REG_VDELP1: state->vdelp1 = val & 1; break;
    case REG_VDELBL: state->vdelbl = val & 1; break;
    case REG_RESMP0: state->resmp0 = (val >> 1) & 1; break;
    case REG_RESMP1: state->resmp1 = (val >> 1) & 1; break;
    default: break;
    }
}

/* a write as the TIA takes it: the store, and the delayed copies a graphics write latches */
static void reg_apply(regstate *state, unsigned reg, uint8_t val)
{
    if (reg == REG_GRP0) state->grp1_old = state->grp1;
    if (reg == REG_GRP1) { state->grp0_old = state->grp0; state->enabl_old = state->enabl; }
    reg_store(state, reg, val);
}

static void state_from_line(regstate *state, const a2600vid_line *line)
{
    memset(state, 0, sizeof *state);
    state->colup0 = line->colup0; state->colup1 = line->colup1;
    state->colupf = line->colupf; state->colubk = line->colubk;
    state->pf0 = line->pf0; state->pf1 = line->pf1; state->pf2 = line->pf2; state->ctrlpf = line->ctrlpf;
    state->grp0 = line->grp0; state->grp1 = line->grp1;
    state->grp0_old = line->grp0_old; state->grp1_old = line->grp1_old;
    state->nusiz0 = line->nusiz0; state->nusiz1 = line->nusiz1;
    state->enam0 = line->enam0; state->enam1 = line->enam1;
    state->enabl = line->enabl; state->enabl_old = line->enabl_old;
    state->refp0 = !!(line->flags & A2600VID_F_REFP0); state->refp1 = !!(line->flags & A2600VID_F_REFP1);
    state->vdelp0 = !!(line->flags & A2600VID_F_VDELP0); state->vdelp1 = !!(line->flags & A2600VID_F_VDELP1);
    state->vdelbl = !!(line->flags & A2600VID_F_VDELBL);
    state->resmp0 = !!(line->flags & A2600VID_F_RESMP0); state->resmp1 = !!(line->flags & A2600VID_F_RESMP1);
    state->vblank = !!(line->flags & A2600VID_F_VBLANK);
}

/* NUSIZ copies of an object: the offsets of the copies from the main one */
static int copy_offsets(unsigned nusiz, int offsets[3])
{
    switch (nusiz & 7) {
    case 1: offsets[0] = 0; offsets[1] = 16; return 2;
    case 2: offsets[0] = 0; offsets[1] = 32; return 2;
    case 3: offsets[0] = 0; offsets[1] = 16; offsets[2] = 32; return 3;
    case 4: offsets[0] = 0; offsets[1] = 64; return 2;
    case 6: offsets[0] = 0; offsets[1] = 32; offsets[2] = 64; return 3;
    default: offsets[0] = 0; return 1;
    }
}

/* the offset of a colour clock from an object's main copy, wrapping at the line */
static int object_offset(int clock, int pos)
{
    return (clock - pos + 2 * A2600VID_FB_W) % A2600VID_FB_W;
}

static int player_on(uint8_t grp, unsigned nusiz, int reflect, int clock, int pos)
{
    const int scale = (nusiz & 7) == 5 ? 2 : (nusiz & 7) == 7 ? 4 : 1;
    int offsets[3];
    const int ncopies = copy_offsets(nusiz, offsets);
    const int off = object_offset(clock, pos);
    for (int num = 0; num < ncopies; num++) {
        const int rel = off - offsets[num];
        if (rel >= 0 && rel < 8 * scale) {
            const int bit = rel / scale;
            return (grp >> (reflect ? bit : 7 - bit)) & 1;
        }
    }
    return 0;
}

static int missile_on(unsigned nusiz, int clock, int pos)
{
    const int width = 1 << ((nusiz >> 4) & 3);
    int offsets[3];
    const int ncopies = copy_offsets(nusiz, offsets);
    const int off = object_offset(clock, pos);
    for (int num = 0; num < ncopies; num++) {
        const int rel = off - offsets[num];
        if (rel >= 0 && rel < width) return 1;
    }
    return 0;
}

static int playfield_on(const regstate *state, int clock)
{
    int quad = (clock % 80) / 4;
    if (clock >= 80 && (state->ctrlpf & 1)) quad = 19 - quad;
    if (quad < 4) return (state->pf0 >> (4 + quad)) & 1;
    if (quad < 12) return (state->pf1 >> (7 - (quad - 4))) & 1;
    return (state->pf2 >> (quad - 12)) & 1;
}

typedef struct { int16_t col; uint8_t reg, value, old; } row_poke;

/* the writes of one row, by colour clock (stable) */
static int row_pokes(const a2600vid_capture *cap, int row, row_poke out[MAX_ROW_POKES])
{
    int count = 0;
    for (int num = 0; num < cap->npokes && num < A2600VID_POKES; num++) {
        const a2600vid_poke *poke = &cap->pokes[num];
        int slot;
        if (poke->row != row || count >= MAX_ROW_POKES) continue;
        slot = count++;
        while (slot > 0 && out[slot - 1].col > poke->col) { out[slot] = out[slot - 1]; slot--; }
        out[slot].col = poke->col; out[slot].reg = poke->reg; out[slot].value = poke->value; out[slot].old = poke->old;
    }
    return count;
}

static const a2600vid_line *cap_line(const a2600vid_capture *cap, int row)
{
    return row >= 0 && row < A2600VID_ROWS && row < cap->fb_h && cap->lines[row].valid ? &cap->lines[row] : NULL;
}

int a2600vid_decode_row(const a2600vid_capture *cap, int row, uint8_t colour[A2600VID_FB_W],
                        uint8_t object[A2600VID_FB_W], uint8_t covers[A2600VID_FB_W])
{
    const a2600vid_line *line = cap ? cap_line(cap, row) : NULL;
    const a2600vid_line *prev = cap ? cap_line(cap, row - 1) : NULL;
    row_poke pokes[MAX_ROW_POKES];
    int npokes, next = 0, comb = 0, switch_col[A2600VID_NOBJ], before[A2600VID_NOBJ];
    uint64_t seen = 0;
    regstate state;

    if (!line) return 0;
    npokes = row_pokes(cap, row, pokes);

    /* the registers as the row began: the end-of-row ones with each written
     * register put back to what its first write replaced */
    state_from_line(&state, line);
    for (int num = 0; num < npokes; num++)
        if (pokes[num].reg < 64 && !(seen >> pokes[num].reg & 1)) {
            seen |= 1ull << pokes[num].reg;
            reg_store(&state, pokes[num].reg, pokes[num].old);
        }
    /* the delayed copies change only when the other player is written */
    if (prev && (seen >> REG_GRP1 & 1)) { state.grp0_old = prev->grp0_old; state.enabl_old = prev->enabl_old; }
    if (prev && (seen >> REG_GRP0 & 1)) state.grp1_old = prev->grp1_old;
    for (int obj = 0; obj < A2600VID_NOBJ; obj++) {
        before[obj] = prev ? prev->pos[obj] : line->pos[obj];
        switch_col[obj] = -1000;
    }
    for (int num = 0; num < npokes; num++) {
        const unsigned reg = pokes[num].reg;
        if (reg >= REG_RESP0 && reg <= REG_RESBL) switch_col[reg - REG_RESP0] = pokes[num].col + 4;
        if (reg == REG_HMOVE && pokes[num].col < -50) comb = 1;
    }

    for (int clock = 0; clock < A2600VID_FB_W; clock++) {
        int pos[A2600VID_NOBJ], on[A2600VID_O_COUNT] = { 0 };
        unsigned mask = 0;
        int top, pf_colour;
        uint8_t shown;

        while (next < npokes && pokes[next].col <= clock) {
            reg_apply(&state, pokes[next].reg, pokes[next].value);
            next++;
        }
        for (int obj = 0; obj < A2600VID_NOBJ; obj++)
            pos[obj] = clock < switch_col[obj] ? before[obj] : line->pos[obj];

        on[A2600VID_O_PF] = playfield_on(&state, clock);
        on[A2600VID_O_P0] = player_on(state.vdelp0 ? state.grp0_old : state.grp0, state.nusiz0, state.refp0, clock, pos[A2600VID_P0]);
        on[A2600VID_O_P1] = player_on(state.vdelp1 ? state.grp1_old : state.grp1, state.nusiz1, state.refp1, clock, pos[A2600VID_P1]);
        on[A2600VID_O_M0] = state.enam0 && !state.resmp0 && missile_on(state.nusiz0, clock, pos[A2600VID_M0]);
        on[A2600VID_O_M1] = state.enam1 && !state.resmp1 && missile_on(state.nusiz1, clock, pos[A2600VID_M1]);
        {
            const int width = 1 << ((state.ctrlpf >> 4) & 3);
            const int off = object_offset(clock, pos[A2600VID_BL]);
            on[A2600VID_O_BL] = (state.vdelbl ? state.enabl_old : state.enabl) && off < width;
        }
        for (int obj = A2600VID_O_PF; obj < A2600VID_O_COUNT; obj++)
            if (on[obj]) mask |= 1u << obj;

        /* the playfield's colour: score mode paints each half in a player's */
        pf_colour = state.colupf;
        if ((state.ctrlpf & 2) && !(state.ctrlpf & 4)) pf_colour = clock < 80 ? state.colup0 : state.colup1;

        top = A2600VID_O_BK;
        shown = state.colubk;
        if (state.ctrlpf & 4) {
            if (on[A2600VID_O_PF]) { top = A2600VID_O_PF; shown = (uint8_t)pf_colour; }
            else if (on[A2600VID_O_BL]) { top = A2600VID_O_BL; shown = state.colupf; }
            else if (on[A2600VID_O_P0]) { top = A2600VID_O_P0; shown = state.colup0; }
            else if (on[A2600VID_O_M0]) { top = A2600VID_O_M0; shown = state.colup0; }
            else if (on[A2600VID_O_P1]) { top = A2600VID_O_P1; shown = state.colup1; }
            else if (on[A2600VID_O_M1]) { top = A2600VID_O_M1; shown = state.colup1; }
        } else {
            if (on[A2600VID_O_P0]) { top = A2600VID_O_P0; shown = state.colup0; }
            else if (on[A2600VID_O_M0]) { top = A2600VID_O_M0; shown = state.colup0; }
            else if (on[A2600VID_O_P1]) { top = A2600VID_O_P1; shown = state.colup1; }
            else if (on[A2600VID_O_M1]) { top = A2600VID_O_M1; shown = state.colup1; }
            else if (on[A2600VID_O_PF]) { top = A2600VID_O_PF; shown = (uint8_t)pf_colour; }
            else if (on[A2600VID_O_BL]) { top = A2600VID_O_BL; shown = state.colupf; }
        }
        if (state.vblank || (comb && clock < 8)) { top = A2600VID_O_BK; shown = 0; mask = 0; }
        if (colour) colour[clock] = shown;
        if (object) object[clock] = (uint8_t)top;
        if (covers) covers[clock] = (uint8_t)mask;
    }
    return 1;
}

/* ---- new / free / settings -------------------------------------------------------------- */

a2600vid_tab *a2600vid_tab_new(a2600session *sess)
{
    a2600vid_tab *tab = calloc(1, sizeof *tab);
    const char *env;
    if (!tab) return NULL;
    tab->sess = sess;
    tab->dbg = sess ? a2600debug_get(sess) : NULL;
    tab->cap = calloc(1, sizeof *tab->cap);
    tab->last = calloc(1, sizeof *tab->last);
    tab->writes = calloc(A2600VID_POKES, sizeof *tab->writes);
    if (!tab->cap || !tab->last || !tab->writes) {
        a2600vid_tab_free(tab);
        return NULL;
    }
    tab->view = A2600VID_V_DISPLAY;
    tab->source = A2600VID_D_OBJECTS;
    tab->overlays = A2600VID_OV_BEAM;
    tab->zoom = 0;
    tab->fb_h = DEFAULT_H;
    tab->view_w = WIDTH_PX * 2;
    tab->view_h = DEFAULT_H * 2;
    tab->hover_x = tab->hover_y = tab->pin_x = tab->pin_y = -1;
    tab->dirty = 1;

    if ((env = getenv("A2600_DEBUGGER_VIDEO_VIEW")) && *env) {
        int hit = 0;
        for (int src = 0; src < A2600VID_D_COUNT; src++)
            if (!strcasecmp(env, k_src_keys[src])) { tab->view = A2600VID_V_DECODED; tab->source = src; hit = 1; break; }
        for (int view = 0; !hit && view < A2600VID_V_COUNT; view++)
            if (!strncasecmp(env, k_view_keys[view], strlen(env))) { tab->view = view; break; }
    }
    if ((env = getenv("A2600_DEBUGGER_VIDEO_OVERLAYS"))) {
        char buf[200], *tok, *rest = NULL;
        unsigned over = 0;
        snprintf(buf, sizeof buf, "%s", env);
        for (tok = strtok_r(buf, ", ", &rest); tok; tok = strtok_r(NULL, ", ", &rest)) {
            if (!strcasecmp(tok, "all")) over |= A2600VID_OV_ALL;
            else if (!strcasecmp(tok, "none")) over = 0;
            else
                for (int num = 0; num < N_OVERLAYS; num++)
                    if (!strcasecmp(tok, k_overlays[num].name)) over |= k_overlays[num].bit;
        }
        tab->overlays = over;
    }
    if ((env = getenv("A2600_DEBUGGER_VIDEO_ZOOM")) && *env) {
        if (!strcasecmp(env, "fit")) tab->zoom = 0;
        else {
            const int zoom = atoi(env);
            for (size_t num = 1; num < sizeof k_zooms / sizeof *k_zooms; num++)
                if (k_zooms[num] == zoom) tab->zoom = zoom;
        }
    }
    tab->env_section = A2600VID_S_PIXEL;
    if ((env = getenv("A2600_DEBUGGER_VIDEO_SECTION")) && *env) {
        for (int sec = 0; sec < A2600VID_S_COUNT; sec++)
            if (!strncasecmp(env, k_sec_keys[sec], strlen(env))) { tab->env_section = sec; break; }
    }
    if (sess) {
        a2600vid_capture_enable(sess, 1);
        tab->capture_on = 1;
    }
    return tab;
}

void a2600vid_tab_free(a2600vid_tab *tab)
{
    if (!tab) return;
    if (tab->capture_on)
        a2600vid_capture_enable(tab->sess, 0);
    free(tab->cap);
    free(tab->last);
    free(tab->writes);
    free(tab->base);
    free(tab->pic);
    free(tab);
}

void a2600vid_tab_set_view(a2600vid_tab *tab, int view)
{
    if (tab && view >= 0 && view < A2600VID_V_COUNT && view != tab->view) { tab->view = view; tab->dirty = 1; }
}
int a2600vid_tab_view(const a2600vid_tab *tab) { return tab ? tab->view : 0; }
void a2600vid_tab_set_source(a2600vid_tab *tab, int src)
{
    if (tab && src >= 0 && src < A2600VID_D_COUNT && src != tab->source) { tab->source = src; tab->dirty = 1; }
}
int a2600vid_tab_source(const a2600vid_tab *tab) { return tab ? tab->source : 0; }
void a2600vid_tab_set_overlays(a2600vid_tab *tab, unsigned over)
{
    if (tab && (over & A2600VID_OV_ALL) != tab->overlays) { tab->overlays = over & A2600VID_OV_ALL; tab->dirty = 1; }
}
unsigned a2600vid_tab_overlays(const a2600vid_tab *tab) { return tab ? tab->overlays : 0; }
void a2600vid_tab_set_zoom(a2600vid_tab *tab, int zoom)
{
    if (tab && zoom >= 0 && zoom <= 16 && zoom != tab->zoom) { tab->zoom = zoom; tab->dirty = 1; }
}
int a2600vid_tab_zoom(const a2600vid_tab *tab) { return tab ? tab->zoom : 0; }
void a2600vid_tab_set_viewport(a2600vid_tab *tab, int wide, int high)
{
    if (!tab) return;
    if (wide < 1) wide = 1;
    if (high < 1) high = 1;
    if (wide != tab->view_w || high != tab->view_h) {
        tab->view_w = wide; tab->view_h = high;
        if (tab->zoom == 0) tab->dirty = 1;
    }
}
int a2600vid_tab_env_section(const a2600vid_tab *tab) { return tab ? tab->env_section : 0; }

/* ---- feeding ----------------------------------------------------------------------------- */

static void set_geometry(a2600vid_tab *tab)
{
    const a2600vid_capture *cap = tab->cap;
    tab->fb_h = cap->fb_h > 0 ? cap->fb_h : DEFAULT_H;
    if (tab->fb_h > A2600VID_ROWS) tab->fb_h = A2600VID_ROWS;
}

void a2600vid_tab_feed(a2600vid_tab *tab, const a2600vid_capture *cap, int live)
{
    if (!tab || !cap) return;
    memcpy(tab->cap, cap, sizeof *tab->cap);
    tab->fed = 1;
    tab->have = 1;
    tab->has_fb = cap->serial != 0 && cap->fb_w > 0 && cap->fb_h > 0;
    tab->has_live = live != 0;
    tab->stopped = live != 0;
    if (!live) tab->cap->beam_row = tab->cap->fb_h;
    set_geometry(tab);
}

/* Take what the machine has. 0 if there is no machine. */
static int acquire(a2600vid_tab *tab)
{
    int changed;

    if (tab->fed)
        return tab->have;
    tab->have = 0;
    if (!tab->sess || !a2600vid_capture_running(tab->sess))
        return 0;
    changed = a2600vid_capture_get(tab->sess, tab->last, &tab->last_serial);
    tab->stopped = tab->dbg && a2600debug_is_stopped(tab->dbg);
    tab->has_fb = tab->last_serial != 0;

    if (tab->stopped || !tab->has_fb) {
        /* a parked machine is ours to read; before the first finished frame
         * the frame in progress is all there is */
        if (!a2600vid_capture_fill(tab->sess, tab->cap))
            return 0;
        tab->has_live = tab->stopped;
        tab->cap_is_last = 0;
        if (!tab->stopped)
            tab->cap->beam_row = tab->cap->fb_h;
        tab->has_fb = tab->has_fb || tab->cap->serial != 0;
    } else {
        tab->has_live = 0;
        if (changed || !tab->cap_is_last)
            memcpy(tab->cap, tab->last, sizeof *tab->cap);
        tab->cap_is_last = 1;
    }
    tab->have = 1;
    set_geometry(tab);
    return 1;
}

static uint64_t signature(const a2600vid_tab *tab)
{
    const a2600vid_capture *cap = tab->cap;
    const size_t picture = (size_t)cap->fb_w * (size_t)(cap->fb_h > 0 ? cap->fb_h : 0);
    uint64_t hash = 1469598103934665603ull;
    hash = fnv(hash, cap->lines, sizeof *cap->lines * (size_t)clampi(cap->fb_h, 0, A2600VID_ROWS));
    hash = fnv(hash, cap->idx, picture < sizeof cap->idx ? picture : sizeof cap->idx);
    hash = fnv(hash, cap->pokes, sizeof *cap->pokes * (size_t)clampi(cap->npokes, 0, A2600VID_POKES));
    hash = fnv(hash, &cap->now, sizeof cap->now);
    hash = fnv(hash, &cap->serial, sizeof cap->serial);
    hash = fnv(hash, &cap->frame, sizeof cap->frame);
    hash = fnv(hash, &cap->vpos, sizeof cap->vpos);
    hash = fnv(hash, &cap->hpos, sizeof cap->hpos);
    hash = fnv(hash, &cap->beam_row, sizeof cap->beam_row);
    hash = fnv(hash, &cap->fb_h, sizeof cap->fb_h);
    hash = fnv(hash, &tab->has_live, sizeof tab->has_live);
    hash = fnv(hash, &tab->has_fb, sizeof tab->has_fb);
    return hash;
}

/* ---- the base picture (1x height, 2 pixels a colour clock) -------------------------------- */

static const uint32_t k_object_tints[A2600VID_O_COUNT] = {
    0xff000000, 0xff30c060, 0xffff5a30, 0xff3a80ff, 0xffffb090, 0xff90c0ff, 0xffffe040
};

typedef struct { uint32_t *pix; int wide, high; } canvas;

static void put_clock(uint32_t *row_px, int clock, uint32_t rgb)
{
    row_px[clock * A2600VID_XSCALE] = rgb;
    row_px[clock * A2600VID_XSCALE + 1] = rgb;
}

/* the frame's pixel as a colour byte (the real one), or the decoded one */
static void frame_row(const a2600vid_tab *tab, int row, uint32_t *dst, int dim)
{
    const a2600vid_capture *cap = tab->cap;
    for (int clock = 0; clock < A2600VID_FB_W; clock++) {
        uint32_t rgb = tab->has_fb ? (cap->pixels[(size_t)row * A2600VID_FB_W + (size_t)clock] | 0xff000000u) : 0xff000000u;
        if (dim) rgb = blend(rgb, 0, 150);
        put_clock(dst, clock, rgb);
    }
}

/* one row redrawn from its registers; values: false colours of the bytes */
static void synth_row(a2600vid_tab *tab, int row, uint32_t *dst, int values)
{
    uint8_t colour[A2600VID_FB_W];
    if (!a2600vid_decode_row(tab->cap, row, colour, NULL, NULL)) {
        for (int pix = 0; pix < WIDTH_PX; pix++) dst[pix] = 0xff000000u;
        return;
    }
    for (int clock = 0; clock < A2600VID_FB_W; clock++)
        put_clock(dst, clock, values ? a2600vid_fallback_colour(colour[clock]) : console_rgb(tab, colour[clock]));
}

static void count_uses(a2600vid_tab *tab)
{
    const a2600vid_capture *cap = tab->cap;
    uint8_t object[A2600VID_FB_W];
    memset(tab->uses, 0, sizeof tab->uses);
    memset(tab->colour_uses, 0, sizeof tab->colour_uses);
    for (int row = 0; row < tab->fb_h; row++) {
        if (a2600vid_decode_row(cap, row, NULL, object, NULL))
            for (int clock = 0; clock < A2600VID_FB_W; clock++)
                tab->uses[object[clock]]++;
        if (tab->has_fb)
            for (int clock = 0; clock < A2600VID_FB_W; clock++)
                tab->colour_uses[(cap->idx[(size_t)row * A2600VID_FB_W + (size_t)clock] >> 1) & 127]++;
    }
}

static void px_blend(canvas *canv, int pos_x, int pos_y, uint32_t rgb, int alpha)
{
    if (pos_x >= 0 && pos_y >= 0 && pos_x < canv->wide && pos_y < canv->high)
        canv->pix[(size_t)pos_y * (size_t)canv->wide + (size_t)pos_x] =
            blend(canv->pix[(size_t)pos_y * (size_t)canv->wide + (size_t)pos_x], rgb, alpha);
}
static void hline(canvas *canv, int pos_y, int from, int upto, uint32_t rgb, int alpha)
{
    for (int pos_x = from; pos_x < upto; pos_x++) px_blend(canv, pos_x, pos_y, rgb, alpha);
}
static void fill_rect(canvas *canv, int left, int top, int wide, int high, uint32_t rgb, int alpha)
{
    for (int row = 0; row < high; row++)
        for (int col = 0; col < wide; col++) px_blend(canv, left + col, top + row, rgb, alpha);
}

static uint32_t palette_swatch_rgb(a2600vid_tab *tab, int col, int row)
{
    return console_rgb(tab, (uint8_t)(col << 4 | row << 1));
}

static int build_palette(a2600vid_tab *tab)
{
    const int wide = 16 * PALETTE_CELL, high = 8 * PALETTE_CELL;
    if (!ensure(&tab->base, &tab->base_cap, (size_t)wide * (size_t)high)) return 0;
    for (int pos_y = 0; pos_y < high; pos_y++)
        for (int pos_x = 0; pos_x < wide; pos_x++) {
            uint32_t rgb = palette_swatch_rgb(tab, pos_x / PALETTE_CELL, pos_y / PALETTE_CELL);
            if (pos_x % PALETTE_CELL == 0 || pos_y % PALETTE_CELL == 0) rgb = blend(rgb, 0, 90);
            tab->base[(size_t)pos_y * (size_t)wide + (size_t)pos_x] = rgb;
        }
    tab->base_w = wide; tab->base_h = high;
    tab->kind = BK_PALETTE;
    return 1;
}

static int build_base(a2600vid_tab *tab)
{
    const a2600vid_capture *cap = tab->cap;
    const int wide = WIDTH_PX, high = tab->fb_h;

    if (tab->view == A2600VID_V_DECODED && tab->source == A2600VID_D_PALETTE)
        return build_palette(tab);
    if (!ensure(&tab->base, &tab->base_cap, (size_t)wide * (size_t)high)) return 0;
    tab->base_w = wide; tab->base_h = high;
    tab->kind = BK_FRAME;
    switch (tab->view) {
    case A2600VID_V_DISPLAY:
        if (!tab->has_fb) {
            why_set(tab, "No frame has finished yet. Let the machine run for a moment.");
            return 0;
        }
        for (int row = 0; row < high; row++) frame_row(tab, row, &tab->base[(size_t)row * (size_t)wide], 0);
        return 1;
    case A2600VID_V_BEAM:
        if (!tab->has_live) {
            why_set(tab, "The Beam view needs the machine stopped: it shows this frame as far as the beam has drawn it, and the last one dimmed below.");
            return 0;
        }
        for (int row = 0; row < high; row++) {
            uint32_t *dst = &tab->base[(size_t)row * (size_t)wide];
            if (row < cap->beam_row) synth_row(tab, row, dst, 0);
            else if (tab->has_fb) frame_row(tab, row, dst, 1);
            else for (int pix = 0; pix < wide; pix++) dst[pix] = 0xff000000u;
        }
        return 1;
    case A2600VID_V_VALUES:
        for (int row = 0; row < high; row++) {
            uint32_t *dst = &tab->base[(size_t)row * (size_t)wide];
            if (tab->has_fb)
                for (int clock = 0; clock < A2600VID_FB_W; clock++)
                    put_clock(dst, clock, a2600vid_fallback_colour(cap->idx[(size_t)row * A2600VID_FB_W + (size_t)clock]));
            else
                synth_row(tab, row, dst, 1);
        }
        return 1;
    default:
        break;
    }
    /* Decoded, Objects: the dimmed frame, each pixel tinted by what drew it */
    for (int row = 0; row < high; row++) {
        uint32_t *dst = &tab->base[(size_t)row * (size_t)wide];
        uint8_t object[A2600VID_FB_W];
        frame_row(tab, row, dst, 1);
        if (!a2600vid_decode_row(cap, row, NULL, object, NULL)) continue;
        for (int clock = 0; clock < A2600VID_FB_W; clock++)
            if (object[clock] != A2600VID_O_BK)
                for (int sub = 0; sub < A2600VID_XSCALE; sub++) {
                    uint32_t *pix = &dst[clock * A2600VID_XSCALE + sub];
                    *pix = blend(*pix, k_object_tints[object[clock]], OBJECT_TINT);
                }
    }
    return 1;
}

/* ---- overlays ---------------------------------------------------------------------------- */

static int line_changed(const a2600vid_line *prev, const a2600vid_line *line)
{
    int mask = 0;
    if (!prev || !line) return 0;
    if (prev->colup0 != line->colup0 || prev->colup1 != line->colup1 || prev->colupf != line->colupf ||
        prev->colubk != line->colubk)
        mask |= A2600VID_LC_COLOUR;
    if (prev->pf0 != line->pf0 || prev->pf1 != line->pf1 || prev->pf2 != line->pf2 || prev->ctrlpf != line->ctrlpf)
        mask |= A2600VID_LC_PLAYFIELD;
    if (prev->grp0 != line->grp0 || prev->grp1 != line->grp1 || prev->nusiz0 != line->nusiz0 ||
        prev->nusiz1 != line->nusiz1 || prev->enam0 != line->enam0 || prev->enam1 != line->enam1 ||
        prev->enabl != line->enabl || prev->flags != line->flags)
        mask |= A2600VID_LC_GRAPHICS;
    if (memcmp(prev->pos, line->pos, sizeof line->pos) != 0) mask |= A2600VID_LC_POSITION;
    return mask;
}

static uint32_t write_colour(unsigned reg)
{
    if (reg >= REG_COLUP0 && reg <= REG_COLUBK) return 0xffffd000u;
    if (reg == REG_CTRLPF || (reg >= REG_PF0 && reg <= REG_PF2)) return 0xff30e0ffu;
    if (reg == REG_NUSIZ0 || reg == REG_NUSIZ1 || reg == REG_REFP0 || reg == REG_REFP1 ||
        (reg >= REG_GRP0 && reg <= REG_ENABL) || (reg >= REG_VDELP0 && reg <= REG_VDELBL))
        return 0xffff50ffu;
    if ((reg >= REG_RESP0 && reg <= REG_RESBL) || reg == REG_RESMP0 || reg == REG_RESMP1) return 0xffff3030u;
    if ((reg >= REG_HMP0 && reg <= REG_HMBL) || reg == REG_HMOVE || reg == REG_HMCLR) return 0xffff9020u;
    return 0xffffffffu;
}

static void draw_overlays(a2600vid_tab *tab, canvas *canv, int zoom)
{
    const a2600vid_capture *cap = tab->cap;
    const int xzoom = zoom * A2600VID_XSCALE;

    for (int row = 0; row < tab->base_h; row++) {
        const a2600vid_line *line = cap_line(cap, row);
        if (!line) continue;
        if ((tab->overlays & A2600VID_OV_BLANK) && (line->flags & A2600VID_F_VBLANK))
            fill_rect(canv, 0, row * zoom, tab->base_w * zoom, zoom, 0xffc02020u, 110);
        if ((tab->overlays & A2600VID_OV_CHANGED) && row > 0 && line_changed(cap_line(cap, row - 1), line))
            hline(canv, row * zoom, 0, tab->base_w * zoom, 0xff00e0ffu, 230);
        if (tab->overlays & A2600VID_OV_POSITIONS)
            for (int obj = 0; obj < A2600VID_NOBJ; obj++)
                fill_rect(canv, line->pos[obj] * xzoom, row * zoom, 1 > zoom / 2 ? 1 : zoom / 2,
                          zoom, k_object_tints[A2600VID_O_P0 + obj], 230);
    }
    if (tab->overlays & A2600VID_OV_WRITES)
        for (int num = 0; num < cap->npokes && num < A2600VID_POKES; num++) {
            const a2600vid_poke *poke = &cap->pokes[num];
            if (poke->row < 0 || poke->row >= tab->base_h) continue;
            fill_rect(canv, clampi(poke->col, 0, A2600VID_FB_W - 1) * xzoom, poke->row * zoom,
                      xzoom > 3 ? xzoom : 3, zoom, write_colour(poke->reg), 220);
        }
    if ((tab->overlays & A2600VID_OV_BEAM) && tab->has_live) {
        const int row = cap->beam_row, beam_x = cap->hpos - 68;
        if (row >= 0 && row < tab->base_h) {
            hline(canv, row * zoom, 0, tab->base_w * zoom, 0xffff4040u, 230);
            if (beam_x >= 0 && beam_x < A2600VID_FB_W)
                fill_rect(canv, beam_x * xzoom, row * zoom, xzoom, zoom, 0xffffffffu, 230);
        }
    }
}

static int pick_zoom(const a2600vid_tab *tab)
{
    int zoom = tab->zoom;
    if (zoom == 0) {
        zoom = 1;
        for (size_t num = 1; num < sizeof k_zooms / sizeof *k_zooms; num++)
            if (tab->base_w * k_zooms[num] <= tab->view_w && tab->base_h * k_zooms[num] <= tab->view_h)
                zoom = k_zooms[num];
    }
    while (zoom > 1 && !fits_zoom(tab->base_w, tab->base_h, zoom)) {
        int lower = 1;
        for (size_t num = 1; num < sizeof k_zooms / sizeof *k_zooms; num++)
            if (k_zooms[num] < zoom) lower = k_zooms[num];
        zoom = lower;
    }
    return zoom;
}

static void build_picture(a2600vid_tab *tab)
{
    int zoom;
    canvas canv;

    tab->why[0] = '\0';
    tab->pic_w = tab->pic_h = 0;
    if (!build_base(tab))
        return;
    zoom = pick_zoom(tab);
    tab->eff_zoom = zoom;
    tab->pic_w = tab->base_w * zoom;
    tab->pic_h = tab->base_h * zoom;
    if (!ensure(&tab->pic, &tab->pic_cap, (size_t)tab->pic_w * (size_t)tab->pic_h)) {
        tab->pic_w = tab->pic_h = 0;
        why_set(tab, "Out of memory.");
        return;
    }
    if (zoom == 1)
        memcpy(tab->pic, tab->base, (size_t)tab->base_w * (size_t)tab->base_h * sizeof *tab->pic);
    else
        for (int pos_y = 0; pos_y < tab->pic_h; pos_y++) {
            const uint32_t *src = &tab->base[(size_t)(pos_y / zoom) * (size_t)tab->base_w];
            uint32_t *dst = &tab->pic[(size_t)pos_y * (size_t)tab->pic_w];
            for (int pos_x = 0; pos_x < tab->pic_w; pos_x++)
                dst[pos_x] = src[pos_x / zoom];
        }
    canv.pix = tab->pic; canv.wide = tab->pic_w; canv.high = tab->pic_h;
    if (tab->kind == BK_FRAME)
        draw_overlays(tab, &canv, zoom);
}

/* ---- the inspector ----------------------------------------------------------------------- */

/* append to an inspector text, never past its end; the new length */
static int add_text(char *text, size_t size, int len, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static int add_text(char *text, size_t size, int len, const char *fmt, ...)
{
    va_list args;
    int added;
    if ((size_t)len >= size - 1) return len;
    va_start(args, fmt);
    added = vsnprintf(text + len, size - (size_t)len, fmt, args);
    va_end(args);
    return added < 0 ? len : (size_t)(len + added) >= size ? (int)size - 1 : len + added;
}

static const a2600vid_line *ref_line(const a2600vid_tab *tab, int *row_out);

static void locate_palette(a2600vid_tab *tab, int pic_x, int pic_y, a2600vid_pixel *out)
{
    const int hue = clampi(pic_x / PALETTE_CELL, 0, 15), lum = clampi(pic_y / PALETTE_CELL, 0, 7);
    const int byte = hue << 4 | lum << 1;
    out->active = 1;
    out->object = -1;
    out->colour = (uint8_t)byte;
    out->rgb = console_rgb(tab, (uint8_t)byte);
    snprintf(out->text, sizeof out->text, "TIA colour $%02X: hue %d, luminance %d\n#%06X\nOn screen in %ld pixels of this frame",
             byte, hue, lum, out->rgb & 0xffffff, tab->colour_uses[byte >> 1]);
}

static void locate(a2600vid_tab *tab, int pos_x, int pos_y, a2600vid_pixel *out)
{
    const a2600vid_capture *cap = tab->cap;
    const int zoom = tab->eff_zoom ? tab->eff_zoom : 1;
    const a2600vid_line *line;
    uint8_t colour[A2600VID_FB_W], object[A2600VID_FB_W], covers[A2600VID_FB_W];
    int pic_x, pic_y, clock, len, writes = 0;

    memset(out, 0, sizeof *out);
    out->row = -1; out->clock = -1; out->object = -1;
    if (!tab->pic_w || pos_x < 0 || pos_y < 0 || pos_x >= tab->pic_w || pos_y >= tab->pic_h)
        return;
    pic_x = pos_x / zoom; pic_y = pos_y / zoom;
    out->valid = 1;
    out->pic_x = pic_x; out->pic_y = pic_y;
    if (tab->kind == BK_PALETTE) {
        locate_palette(tab, pic_x, pic_y, out);
        return;
    }
    clock = pic_x / A2600VID_XSCALE;
    out->row = pic_y;
    out->clock = clock;
    out->scanline = pic_y + cap->vis_top;
    out->rgb = tab->base ? tab->base[(size_t)pic_y * (size_t)tab->base_w + (size_t)pic_x] : 0xff000000u;
    line = cap_line(cap, pic_y);
    len = snprintf(out->text, sizeof out->text, "Colour clock %d, row %d, scanline %d\n", clock, out->row, out->scanline);
    if (tab->has_fb) out->colour = cap->idx[(size_t)pic_y * A2600VID_FB_W + (size_t)clock];
    if (!line || !a2600vid_decode_row(cap, pic_y, colour, object, covers)) {
        add_text(out->text, sizeof out->text, len,
                 "The TIA drew nothing recorded here (the machine has not got this far)\nFrame colour $%02X   #%06X",
                 out->colour, out->rgb & 0xffffff);
        return;
    }
    out->active = 1;
    out->object = object[clock];
    out->covers = covers[clock];
    out->value = colour[clock];
    for (int num = 0; num < cap->npokes && num < A2600VID_POKES; num++)
        if (cap->pokes[num].row == pic_y) writes++;
    out->writes = writes;
    len = add_text(out->text, sizeof out->text, len, "Drawn by %s", k_obj_names[out->object]);
    if (out->object == A2600VID_O_BK && (line->flags & A2600VID_F_VBLANK))
        len = add_text(out->text, sizeof out->text, len, " (VBLANK is on: black)");
    len = add_text(out->text, sizeof out->text, len, "   colour $%02X   #%06X\nOn here:", out->value,
                   console_rgb(tab, out->value) & 0xffffff);
    for (int obj = A2600VID_O_PF; obj < A2600VID_O_COUNT; obj++)
        if (out->covers & (1u << obj)) len = add_text(out->text, sizeof out->text, len, " %s", k_obj_names[obj]);
    if (!out->covers) len = add_text(out->text, sizeof out->text, len, " nothing (background)");
    len = add_text(out->text, sizeof out->text, len, "\n");
    if (tab->has_fb && (out->colour & 0xFE) != (out->value & 0xFE))
        len = add_text(out->text, sizeof out->text, len,
                       "The frame shows $%02X here: a write or delay the replay does not model\n", out->colour);
    len = add_text(out->text, sizeof out->text, len,
                   "Row registers: COLUP0 %02X COLUP1 %02X COLUPF %02X COLUBK %02X\n"
                   "PF %02X %02X %02X CTRLPF %02X (%s%s%s)   GRP0 %02X GRP1 %02X\n"
                   "Positions P0 %d P1 %d M0 %d M1 %d BL %d   NUSIZ %02X %02X\n",
                   line->colup0, line->colup1, line->colupf, line->colubk, line->pf0, line->pf1, line->pf2,
                   line->ctrlpf, (line->ctrlpf & 1) ? "reflect" : "repeat",
                   (line->ctrlpf & 2) ? ", score" : "", (line->ctrlpf & 4) ? ", priority" : "",
                   line->grp0, line->grp1, line->pos[0], line->pos[1], line->pos[2], line->pos[3], line->pos[4],
                   line->nusiz0, line->nusiz1);
    {
        int shown = 0;
        len = add_text(out->text, sizeof out->text, len, "%d write%s this row", writes, writes == 1 ? "" : "s");
        for (int num = 0; num < cap->npokes && num < A2600VID_POKES && shown < 6; num++) {
            const a2600vid_poke *poke = &cap->pokes[num];
            if (poke->row != pic_y) continue;
            len = add_text(out->text, sizeof out->text, len, "%s %s=$%02X@%d", shown ? "," : ":",
                           a2600vid_reg_name(poke->reg), poke->value, poke->col);
            shown++;
        }
        if (shown < writes) add_text(out->text, sizeof out->text, len, ", ...");
    }
}

int a2600vid_tab_hover(a2600vid_tab *tab, int pos_x, int pos_y)
{
    char old[sizeof tab->hover.text];
    if (!tab) return 0;
    snprintf(old, sizeof old, "%s", tab->hover.valid ? tab->hover.text : "");
    tab->hover_x = pos_x; tab->hover_y = pos_y;
    locate(tab, pos_x, pos_y, &tab->hover);
    return strcmp(old, tab->hover.valid ? tab->hover.text : "") != 0;
}

void a2600vid_tab_pin(a2600vid_tab *tab, int pos_x, int pos_y)
{
    int zoom;
    if (!tab) return;
    zoom = tab->eff_zoom ? tab->eff_zoom : 1;
    if (tab->pin_x >= 0 && pos_x / zoom == tab->pin_x / zoom && pos_y / zoom == tab->pin_y / zoom) {
        a2600vid_tab_unpin(tab);
        return;
    }
    tab->pin_x = pos_x; tab->pin_y = pos_y;
    locate(tab, pos_x, pos_y, &tab->pin);
    if (!tab->pin.valid) a2600vid_tab_unpin(tab);
}

void a2600vid_tab_unpin(a2600vid_tab *tab)
{
    if (!tab) return;
    tab->pin_x = tab->pin_y = -1;
    memset(&tab->pin, 0, sizeof tab->pin);
}

const a2600vid_pixel *a2600vid_tab_pixel(const a2600vid_tab *tab)
{
    if (!tab) return NULL;
    if (tab->pin.valid) return &tab->pin;
    if (tab->hover.valid) return &tab->hover;
    return NULL;
}

static void mark_rect(const a2600vid_tab *tab, const a2600vid_pixel *pix, int out[4])
{
    const int zoom = tab->eff_zoom ? tab->eff_zoom : 1;
    if (tab->kind == BK_PALETTE) {
        out[0] = pix->pic_x / PALETTE_CELL * PALETTE_CELL * zoom;
        out[1] = pix->pic_y / PALETTE_CELL * PALETTE_CELL * zoom;
        out[2] = PALETTE_CELL * zoom; out[3] = PALETTE_CELL * zoom;
    } else {
        /* one colour clock wide, one row high */
        out[0] = pix->pic_x / A2600VID_XSCALE * A2600VID_XSCALE * zoom;
        out[1] = pix->pic_y * zoom;
        out[2] = A2600VID_XSCALE * zoom; out[3] = zoom;
    }
}

int a2600vid_tab_marks(const a2600vid_tab *tab, int hover[4], int pin[4])
{
    int res = 0;
    if (!tab) return 0;
    if (tab->hover.valid) { mark_rect(tab, &tab->hover, hover); res |= 1; }
    if (tab->pin.valid) { mark_rect(tab, &tab->pin, pin); res |= 2; }
    return res;
}

/* ---- breaking ---------------------------------------------------------------------------- */

int a2600vid_tab_break_on_register(a2600vid_tab *tab, unsigned reg)
{
    char cmd[32], out[256];
    if (!tab || !tab->dbg || reg > 0x3F) return -1;
    snprintf(cmd, sizeof cmd, "trapWrite $%02X", reg);
    out[0] = '\0';
    a2600debug_command(tab->dbg, cmd, out, sizeof out);
    /* Stella toggles: a trap that was there has just been removed */
    if (strstr(out, "removed")) {
        out[0] = '\0';
        a2600debug_command(tab->dbg, cmd, out, sizeof out);
    }
    return strstr(out, "added") ? 0 : -1;
}

int a2600vid_tab_break_on_swatch(a2600vid_tab *tab, int index)
{
    if (!tab || index < 0 || index >= A2600VID_O_COUNT) return -1;
    return a2600vid_tab_break_on_register(tab, k_obj_addr[index]);
}

int a2600vid_tab_break_on_pixel(a2600vid_tab *tab)
{
    const a2600vid_pixel *pix = a2600vid_tab_pixel(tab);
    static const unsigned k_regs[A2600VID_O_COUNT][5] = {
        { REG_COLUBK, 0, 0, 0, 0 },
        { REG_COLUPF, REG_PF0, REG_PF1, REG_PF2, REG_CTRLPF },
        { REG_COLUP0, REG_GRP0, REG_NUSIZ0, REG_RESP0, 0 },
        { REG_COLUP1, REG_GRP1, REG_NUSIZ1, REG_RESP1, 0 },
        { REG_COLUP0, REG_ENAM0, REG_NUSIZ0, REG_RESM0, 0 },
        { REG_COLUP1, REG_ENAM1, REG_NUSIZ1, REG_RESM1, 0 },
        { REG_COLUPF, REG_ENABL, REG_CTRLPF, REG_RESBL, 0 },
    };
    int status = -1;
    if (!tab || !tab->dbg || !pix || !pix->active) return -1;
    if (pix->object < 0 || pix->object >= A2600VID_O_COUNT) return -1;
    for (int num = 0; num < 5; num++)
        if (num == 0 || k_regs[pix->object][num]) {
            if (a2600vid_tab_break_on_register(tab, k_regs[pix->object][num]) == 0) status = 0;
        }
    return status;
}

int a2600vid_tab_pixel_object(const a2600vid_tab *tab)
{
    const a2600vid_pixel *pix = a2600vid_tab_pixel(tab);
    return pix && pix->valid && pix->active ? pix->object : -1;
}

/* ---- the tables ---------------------------------------------------------------------------- */

static void reg_set(a2600vid_tab *tab, int *count, const char *name, const char *value,
                    const char *fmt, ...) __attribute__((format(printf, 5, 6)));
static void reg_set(a2600vid_tab *tab, int *count, const char *name, const char *value,
                    const char *fmt, ...)
{
    a2600vid_reg *reg;
    va_list args;
    int prev_age;
    if (*count >= MAX_REGS) return;
    reg = &tab->regs[*count];
    prev_age = reg->age;
    reg->name = name;
    snprintf(reg->value, sizeof reg->value, "%s", value);
    va_start(args, fmt);
    vsnprintf(reg->meaning, sizeof reg->meaning, fmt, args);
    va_end(args);
    if (!tab->regs_primed || strcmp(tab->prev_val[*count], reg->value) != 0)
        reg->age = tab->regs_primed ? 0 : 99;
    else
        reg->age = prev_age < 99 ? prev_age + 1 : 99;
    memcpy(tab->prev_val[*count], reg->value, sizeof tab->prev_val[*count]);
    (*count)++;
}

static const a2600vid_line k_blank_line;

/* the row whose registers the tables show: the live registers when stopped,
 * else the last row the TIA drew */
static const a2600vid_line *ref_line(const a2600vid_tab *tab, int *row_out)
{
    const a2600vid_capture *cap = tab->cap;
    if (tab->has_live && cap->now.valid) {
        if (row_out) *row_out = clampi(cap->beam_row, 0, tab->fb_h - 1);
        return &cap->now;
    }
    for (int row = tab->fb_h - 1; row >= 0; row--)
        if (cap_line(cap, row)) { if (row_out) *row_out = row; return cap_line(cap, row); }
    if (row_out) *row_out = -1;
    return &k_blank_line;
}

static const char *nusiz_text(unsigned nusiz)
{
    static const char *const names[] = { "one copy", "two copies close", "two copies, medium", "three copies close",
                                         "two copies, wide", "double-size player", "three copies, medium",
                                         "quad-size player" };
    return names[nusiz & 7];
}

static void bits_text(unsigned val, char out[9])
{
    for (int bit = 0; bit < 8; bit++) out[bit] = (val >> (7 - bit)) & 1 ? '#' : '.';
    out[8] = '\0';
}

static void build_tables(a2600vid_tab *tab)
{
    const a2600vid_capture *cap = tab->cap;
    const a2600vid_line *ref = ref_line(tab, NULL);
    int count = 0, rows_written;
    char val[24], bits[9];
    uint32_t rgb;

    /* registers (ages carry over from the previous build) */
#define COLOUR_REG(NAME, FIELD) \
    snprintf(val, sizeof val, "%02X", ref->FIELD); \
    rgb = console_rgb(tab, ref->FIELD); \
    reg_set(tab, &count, NAME, val, "hue %d, luminance %d   #%06X", ref->FIELD >> 4, (ref->FIELD >> 1) & 7, rgb & 0xffffff)
    COLOUR_REG("COLUP0", colup0);
    COLOUR_REG("COLUP1", colup1);
    COLOUR_REG("COLUPF", colupf);
    COLOUR_REG("COLUBK", colubk);
#undef COLOUR_REG
    snprintf(val, sizeof val, "%02X", ref->pf0);
    bits_text((unsigned)((ref->pf0 >> 4) & 15) << 4, bits);
    reg_set(tab, &count, "PF0", val, "pixels 0-15: %.4s (bits 4-7, left to right)", bits);
    snprintf(val, sizeof val, "%02X", ref->pf1);
    bits_text(ref->pf1, bits);
    reg_set(tab, &count, "PF1", val, "pixels 16-47: %s (bit 7 first)", bits);
    snprintf(val, sizeof val, "%02X", ref->pf2);
    {
        char rev[9];
        for (int bit = 0; bit < 8; bit++) rev[bit] = (ref->pf2 >> bit) & 1 ? '#' : '.';
        rev[8] = '\0';
        reg_set(tab, &count, "PF2", val, "pixels 48-79: %s (bit 0 first)", rev);
    }
    snprintf(val, sizeof val, "%02X", ref->ctrlpf);
    reg_set(tab, &count, "CTRLPF", val, "%s right half%s%s, ball %d pixel%s",
            (ref->ctrlpf & 1) ? "mirrored" : "repeated", (ref->ctrlpf & 2) ? ", score mode" : "",
            (ref->ctrlpf & 4) ? ", playfield and ball in front" : "", 1 << ((ref->ctrlpf >> 4) & 3),
            ((ref->ctrlpf >> 4) & 3) ? "s" : "");
    snprintf(val, sizeof val, "%02X", ref->grp0);
    bits_text(ref->grp0, bits);
    reg_set(tab, &count, "GRP0", val, "%s%s (delayed copy %02X)", bits, (ref->flags & A2600VID_F_VDELP0) ? ", VDELP0 shows the delayed one" : "", ref->grp0_old);
    snprintf(val, sizeof val, "%02X", ref->grp1);
    bits_text(ref->grp1, bits);
    reg_set(tab, &count, "GRP1", val, "%s%s (delayed copy %02X)", bits, (ref->flags & A2600VID_F_VDELP1) ? ", VDELP1 shows the delayed one" : "", ref->grp1_old);
    snprintf(val, sizeof val, "%02X", ref->nusiz0);
    reg_set(tab, &count, "NUSIZ0", val, "player 0: %s; missile 0 %d pixel%s wide", nusiz_text(ref->nusiz0),
            1 << ((ref->nusiz0 >> 4) & 3), ((ref->nusiz0 >> 4) & 3) ? "s" : "");
    snprintf(val, sizeof val, "%02X", ref->nusiz1);
    reg_set(tab, &count, "NUSIZ1", val, "player 1: %s; missile 1 %d pixel%s wide", nusiz_text(ref->nusiz1),
            1 << ((ref->nusiz1 >> 4) & 3), ((ref->nusiz1 >> 4) & 3) ? "s" : "");
    snprintf(val, sizeof val, "%d%d%d", ref->enam0, ref->enam1, ref->enabl);
    reg_set(tab, &count, "ENAM0/1,ENABL", val, "missile 0 %s, missile 1 %s, ball %s%s", ref->enam0 ? "on" : "off",
            ref->enam1 ? "on" : "off", ref->enabl ? "on" : "off", (ref->flags & A2600VID_F_VDELBL) ? " (VDELBL: delayed copy)" : "");
    snprintf(val, sizeof val, "%d%d", !!(ref->flags & A2600VID_F_REFP0), !!(ref->flags & A2600VID_F_REFP1));
    reg_set(tab, &count, "REFP0/1", val, "player 0 %s, player 1 %s", (ref->flags & A2600VID_F_REFP0) ? "reflected" : "normal",
            (ref->flags & A2600VID_F_REFP1) ? "reflected" : "normal");
    snprintf(val, sizeof val, "%d%d%d", !!(ref->flags & A2600VID_F_VDELP0), !!(ref->flags & A2600VID_F_VDELP1),
             !!(ref->flags & A2600VID_F_VDELBL));
    reg_set(tab, &count, "VDELP0/1,BL", val, "vertical delay of player 0, player 1, ball");
    snprintf(val, sizeof val, "%d%d", !!(ref->flags & A2600VID_F_RESMP0), !!(ref->flags & A2600VID_F_RESMP1));
    reg_set(tab, &count, "RESMP0/1", val, "missiles locked to their players (hidden)");
    snprintf(val, sizeof val, "%d %d %d %d %d", ref->pos[0], ref->pos[1], ref->pos[2], ref->pos[3], ref->pos[4]);
    reg_set(tab, &count, "POS", val, "first pixel of P0, P1, M0, M1 and the ball (set by RESxx, moved by HMOVE)");
    snprintf(val, sizeof val, "%X %X %X %X %X", ref->hmove[0], ref->hmove[1], ref->hmove[2], ref->hmove[3], ref->hmove[4]);
    reg_set(tab, &count, "HM", val, "HMP0 HMP1 HMM0 HMM1 HMBL (the high nibble; 8 = no motion)");
    snprintf(val, sizeof val, "%d", !!(ref->flags & A2600VID_F_VBLANK));
    reg_set(tab, &count, "VBLANK", val, "%s", (ref->flags & A2600VID_F_VBLANK) ? "on: the beam is blanked (black)" : "off");
    snprintf(val, sizeof val, "%X %02X %X", ref->audc[0], ref->audf[0], ref->audv[0]);
    reg_set(tab, &count, "AUD0", val, "AUDC0, AUDF0, AUDV0");
    snprintf(val, sizeof val, "%X %02X %X", ref->audc[1], ref->audf[1], ref->audv[1]);
    reg_set(tab, &count, "AUD1", val, "AUDC1, AUDF1, AUDV1");
    snprintf(val, sizeof val, "%04X", ref->collisions);
    reg_set(tab, &count, "CX", val, "collision latches set (bit mask, cleared by CXCLR)");
    tab->nregs = count;
    tab->regs_primed = 1;

    /* colours: one per object */
    count_uses(tab);
    {
        const a2600vid_pixel *pix = a2600vid_tab_pixel(tab);
        for (int obj = 0; obj < A2600VID_O_COUNT; obj++) {
            static const size_t k_offsets[A2600VID_O_COUNT] = {
                offsetof(a2600vid_line, colubk), offsetof(a2600vid_line, colupf), offsetof(a2600vid_line, colup0),
                offsetof(a2600vid_line, colup1), offsetof(a2600vid_line, colup0), offsetof(a2600vid_line, colup1),
                offsetof(a2600vid_line, colupf) };
            const uint8_t value = ((const uint8_t *)ref)[k_offsets[obj]];
            a2600vid_swatch *swatch = &tab->swatches[obj];
            swatch->index = obj;
            snprintf(swatch->name, sizeof swatch->name, "%s", k_obj_names[obj]);
            swatch->reg_name = k_obj_regs[obj];
            swatch->address = k_obj_addr[obj];
            swatch->value = value;
            swatch->rgb = console_rgb(tab, value);
            swatch->uses = tab->uses[obj];
            swatch->inspected = pix && pix->valid && pix->active && pix->object == obj;
        }
    }

    /* rows: where any register changes */
    tab->nrows = tab->nchanges = 0;
    rows_written = 0;
    for (int row = 0; row < tab->fb_h; row++) {
        const a2600vid_line *cur = cap_line(cap, row), *prev = cap_line(cap, row - 1);
        int mask = 0;
        if (!cur) continue;
        mask = prev ? line_changed(prev, cur) : 0;
        if (mask) tab->nchanges++;
        if (cur->pokes) { mask |= A2600VID_LC_WRITES; rows_written++; }
        if (tab->nrows == 0 || (mask & ~A2600VID_LC_WRITES)) {
            a2600vid_line_row *out = &tab->rows[tab->nrows++];
            out->row = row;
            out->scanline = row + cap->vis_top;
            out->colup0 = cur->colup0; out->colup1 = cur->colup1; out->colupf = cur->colupf; out->colubk = cur->colubk;
            out->pf0 = cur->pf0; out->pf1 = cur->pf1; out->pf2 = cur->pf2; out->ctrlpf = cur->ctrlpf;
            out->grp0 = cur->grp0; out->grp1 = cur->grp1; out->nusiz0 = cur->nusiz0; out->nusiz1 = cur->nusiz1;
            out->enables = (uint8_t)(cur->enam0 | cur->enam1 << 1 | cur->enabl << 2);
            out->flags = cur->flags;
            memcpy(out->pos, cur->pos, sizeof out->pos);
            out->writes = cur->pokes;
            out->changed_mask = mask;
        }
    }

    /* writes */
    tab->nwrites = 0;
    tab->dropped = cap->pokes_dropped;
    for (int num = 0; num < cap->npokes && num < A2600VID_POKES; num++) {
        const a2600vid_poke *poke = &cap->pokes[num];
        a2600vid_write *out = &tab->writes[tab->nwrites++];
        out->reg = poke->reg;
        out->name = a2600vid_reg_name(poke->reg);
        out->value = poke->value; out->old = poke->old;
        out->clock = poke->col; out->row = poke->row;
        out->scanline = poke->row >= 0 ? poke->row + cap->vis_top : -1;
    }

    /* timing */
    tab->ntiming = 0;
#define TL(...) do { if (tab->ntiming < MAX_TIMING) { \
        snprintf(tab->timing_buf[tab->ntiming], sizeof tab->timing_buf[0], __VA_ARGS__); \
        tab->timing_ptr[tab->ntiming] = tab->timing_buf[tab->ntiming]; tab->ntiming++; } } while (0)
    TL("Beam\tscanline %d of %d, colour clock %d of 228", cap->vpos, cap->total_lines, cap->hpos);
    TL("Beam position\t%d.%d%% of the frame", a2600vid_tab_beam_permille(tab) / 10, a2600vid_tab_beam_permille(tab) % 10);
    TL("VBLANK\t%s",
       tab->has_live ? (ref->flags & A2600VID_F_VBLANK ? "on, the beam is blanked" : "off, the beam is drawing the picture")
                     : "not while running");
    TL("Frame\t%llu", (unsigned long long)cap->frame);
    TL("Standard\t%s, %d lines", cap->pal ? "PAL" : "NTSC", cap->total_lines);
    TL("Picture\t%d x %d, scanlines %d-%d", A2600VID_FB_W, tab->fb_h, cap->vis_top, cap->vis_top + tab->fb_h - 1);
    TL("Rows drawn\t%d of %d%s", clampi(cap->beam_row, 0, tab->fb_h), tab->fb_h, tab->has_live ? " (this frame)" : "");
    TL("TIA writes\t%d%s%d", tab->nwrites, tab->dropped ? ", lost to the ring: " : "", tab->dropped);
    TL("WSYNC\t%d CPU cycles held in the last frame", cap->wsync_cycles);
    TL("Changes this frame\t%d row%s", tab->nchanges, tab->nchanges == 1 ? "" : "s");
    TL("Rows with writes\t%d", rows_written);
    TL("Machine\t%s", tab->stopped ? "stopped" : "running");
#undef TL

    snprintf(tab->status, sizeof tab->status,
             "%s view%s, %d change%s in the frame, %d TIA write%s\n%s%s",
             k_view_names[tab->view],
             tab->view == A2600VID_V_DECODED ? (tab->source == A2600VID_D_OBJECTS ? " (objects)" : " (palette)") : "",
             tab->nchanges, tab->nchanges == 1 ? "" : "s", tab->nwrites, tab->nwrites == 1 ? "" : "s",
             tab->stopped ? "Stopped" : "Running: showing the last finished frame",
             tab->stopped ? (tab->has_fb ? "" : " (no frame has finished yet)") : "");
}

/* ---- refresh -------------------------------------------------------------------------------- */

unsigned a2600vid_tab_refresh(a2600vid_tab *tab, int force)
{
    uint64_t sig;
    unsigned res = 0;
    int fresh;

    if (!tab) return 0;
    if (!acquire(tab)) {
        why_set(tab, "The machine is not running.");
        tab->pic_w = tab->pic_h = 0;
        return A2600VID_TAB_OFF | A2600VID_TAB_PICTURE | A2600VID_TAB_TABLES;
    }
    sig = signature(tab);
    fresh = force || sig != tab->sig;
    if (fresh)
        tab->sig = sig;
    if (fresh || tab->dirty) {
        build_picture(tab);
        tab->dirty = 0;
        res |= A2600VID_TAB_PICTURE;
    }
    if (fresh) {
        if (tab->hover_x >= 0) locate(tab, tab->hover_x, tab->hover_y, &tab->hover);
        if (tab->pin_x >= 0) locate(tab, tab->pin_x, tab->pin_y, &tab->pin);
        build_tables(tab);
        res |= A2600VID_TAB_TABLES;
    }
    return res;
}

const uint32_t *a2600vid_tab_picture(a2600vid_tab *tab, int *wide, int *high)
{
    if (!tab || !tab->pic_w) return NULL;
    if (wide) *wide = tab->pic_w;
    if (high) *high = tab->pic_h;
    return tab->pic;
}
const char *a2600vid_tab_why(const a2600vid_tab *tab) { return tab ? tab->why : ""; }
int a2600vid_tab_effective_zoom(const a2600vid_tab *tab) { return tab ? tab->eff_zoom : 1; }
const char *a2600vid_tab_status(const a2600vid_tab *tab) { return tab ? tab->status : ""; }
int a2600vid_tab_regs(const a2600vid_tab *tab, const a2600vid_reg **out)
{
    if (out) *out = tab->regs;
    return tab->nregs;
}
int a2600vid_tab_swatches(const a2600vid_tab *tab, const a2600vid_swatch **out)
{
    /* the inspected pixel moves between refreshes */
    const a2600vid_pixel *pix = a2600vid_tab_pixel(tab);
    a2600vid_swatch *swatch = ((a2600vid_tab *)tab)->swatches;
    for (int obj = 0; obj < A2600VID_O_COUNT; obj++)
        swatch[obj].inspected = pix && pix->valid && pix->active && pix->object == obj;
    if (out) *out = tab->swatches;
    return tab->have ? A2600VID_O_COUNT : 0;
}
int a2600vid_tab_lines(const a2600vid_tab *tab, const a2600vid_line_row **out)
{
    if (out) *out = tab->rows;
    return tab->nrows;
}
int a2600vid_tab_writes(const a2600vid_tab *tab, const a2600vid_write **out)
{
    if (out) *out = tab->writes;
    return tab->nwrites;
}
int a2600vid_tab_writes_dropped(const a2600vid_tab *tab) { return tab ? tab->dropped : 0; }
int a2600vid_tab_timing(const a2600vid_tab *tab, const char *const **lines)
{
    if (lines) *lines = (const char *const *)tab->timing_ptr;
    return tab->ntiming;
}
int a2600vid_tab_beam_permille(const a2600vid_tab *tab)
{
    return tab && tab->have && tab->cap->total_lines > 0 ? tab->cap->vpos * 1000 / tab->cap->total_lines : 0;
}

int a2600vid_tab_text(a2600vid_tab *tab, int section, char *dst, int dstsz)
{
    int len = 0;
#define EMIT(...) do { if (len < dstsz) len += snprintf(dst + len, (size_t)(dstsz - len), __VA_ARGS__); } while (0)
    if (!dst || dstsz <= 0) return 0;
    dst[0] = '\0';
    if (!tab || !tab->have) { EMIT("The machine is not running.\n"); return len < dstsz ? len : dstsz - 1; }
    switch (section) {
    case A2600VID_S_REGISTERS:
        for (int num = 0; num < tab->nregs; num++)
            EMIT("%-13s %-10s %s\n", tab->regs[num].name, tab->regs[num].value, tab->regs[num].meaning);
        break;
    case A2600VID_S_COLOURS:
        for (int obj = 0; obj < A2600VID_O_COUNT; obj++)
            EMIT("%-3s %-7s $%02X = $%02X  #%06X  %ld pixels\n", tab->swatches[obj].name, tab->swatches[obj].reg_name,
                 tab->swatches[obj].address, tab->swatches[obj].value, tab->swatches[obj].rgb & 0xffffff,
                 tab->swatches[obj].uses);
        break;
    case A2600VID_S_LINES:
        for (int num = 0; num < tab->nrows; num++) {
            const a2600vid_line_row *row = &tab->rows[num];
            EMIT("row %3d scanline %3d  COLU %02X %02X %02X %02X  PF %02X %02X %02X/%02X  GRP %02X %02X  NUSIZ %02X %02X  "
                 "pos %3d %3d %3d %3d %3d  writes %2d  %s%s%s%s\n",
                 row->row, row->scanline, row->colup0, row->colup1, row->colupf, row->colubk, row->pf0, row->pf1,
                 row->pf2, row->ctrlpf, row->grp0, row->grp1, row->nusiz0, row->nusiz1, row->pos[0], row->pos[1],
                 row->pos[2], row->pos[3], row->pos[4], row->writes,
                 (row->changed_mask & A2600VID_LC_COLOUR) ? "colour " : "",
                 (row->changed_mask & A2600VID_LC_PLAYFIELD) ? "playfield " : "",
                 (row->changed_mask & A2600VID_LC_GRAPHICS) ? "graphics " : "",
                 (row->changed_mask & A2600VID_LC_POSITION) ? "position" : "");
        }
        break;
    case A2600VID_S_WRITES:
        for (int num = 0; num < tab->nwrites; num++) {
            const a2600vid_write *entry = &tab->writes[num];
            EMIT("%-7s = $%02X (was $%02X)  clock %4d  %s", entry->name, entry->value, entry->old, entry->clock,
                 entry->row < 0 ? "outside the picture\n" : "");
            if (entry->row >= 0) EMIT("row %d, scanline %d\n", entry->row, entry->scanline);
        }
        if (tab->dropped) EMIT("(%d older writes lost)\n", tab->dropped);
        break;
    case A2600VID_S_TIMING:
        for (int num = 0; num < tab->ntiming; num++) EMIT("%s\n", tab->timing_buf[num]);
        break;
    default:
        EMIT("%s\n", tab->status);
        if (a2600vid_tab_pixel(tab)) EMIT("%s\n", a2600vid_tab_pixel(tab)->text);
        break;
    }
#undef EMIT
    return len < dstsz ? len : dstsz - 1;
}
