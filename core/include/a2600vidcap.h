/*
 * a2600vidcap -- a displayed frame as the TIA left it, for the Video tab
 * (a2600vidtab.h). The TIA has no line buffer: the picture is whatever the
 * registers said as each scanline went by, so the host records, per visible
 * row, the register set at the END of the row (TIA::setLineTap) and every TIA
 * write with the beam position it landed at (TIA::setPokeTap). Taken once per
 * finished frame on the emulation thread while a tab exists
 * (StellaHost::captureEnable), so a reader sees one consistent frame while
 * the game runs; a stopped machine is read live (a2600vid_capture_fill).
 *
 * Rows are VISIBLE rows (frame row 0 = scanline vis_top); the picture is
 * 160 colour clocks wide.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef A2600VIDCAP_H
#define A2600VIDCAP_H

#include <stdint.h>

#include "a2600session.h"

#ifdef __cplusplus
extern "C" {
#endif

#define A2600VID_FB_W     160   /* colour clocks across the picture */
#define A2600VID_ROWS     320   /* visible rows held (TIAConstants::frameBufferHeight) */
#define A2600VID_POKES    4096  /* TIA writes kept for a frame */

/* a2600vid_line.flags */
#define A2600VID_F_REFP0  0x01u
#define A2600VID_F_REFP1  0x02u
#define A2600VID_F_VDELP0 0x04u
#define A2600VID_F_VDELP1 0x08u
#define A2600VID_F_VDELBL 0x10u
#define A2600VID_F_RESMP0 0x20u     /* missile 0 locked to player 0 */
#define A2600VID_F_RESMP1 0x40u
#define A2600VID_F_VBLANK 0x80u     /* VBLANK on: the row is black */

/* The objects, in a2600vid_line.pos[] and hmove[] order. */
enum { A2600VID_P0 = 0, A2600VID_P1, A2600VID_M0, A2600VID_M1, A2600VID_BL, A2600VID_NOBJ };

/* The registers of one row, as written at its end. */
typedef struct {
    uint8_t valid;              /* the TIA drew this row */
    uint8_t colup0, colup1, colupf, colubk;
    uint8_t pf0, pf1, pf2, ctrlpf;
    uint8_t grp0, grp1;         /* the registers as written */
    uint8_t grp0_old, grp1_old; /* the delayed copies VDELP0/1 selects */
    uint8_t nusiz0, nusiz1;
    uint8_t enam0, enam1, enabl, enabl_old;   /* 0/1 */
    uint8_t flags;              /* A2600VID_F_* */
    uint8_t pos[A2600VID_NOBJ]; /* first pixel of each object's main copy, 0..159 */
    uint8_t hmove[A2600VID_NOBJ]; /* HMP0 HMP1 HMM0 HMM1 HMBL, the register's high nibble */
    uint8_t audc[2], audf[2], audv[2];
    uint16_t pokes;             /* TIA writes during the row */
    uint16_t collisions;        /* collision latches, bit i = TiaState::cx[i] (P0PF P0BL M1P0 M0P0 P0P1
                                 * P1PF P1BL M1P1 M0P1 M0PF M0BL M0M1 M1PF M1BL BLPF) */
} a2600vid_line;

/* One write to a TIA register. y is the visible row, -1 outside the
 * picture (vblank, overscan, vsync); col runs -68 (the left edge of hblank)
 * to 159. */
typedef struct {
    int16_t col, row;
    uint8_t reg, value, old, pad;
} a2600vid_poke;

typedef struct a2600vid_capture {
    a2600vid_line lines[A2600VID_ROWS];     /* index = visible row */
    uint32_t pixels[A2600VID_FB_W * A2600VID_ROWS];  /* XRGB, fb_w x fb_h, packed: the last finished frame */
    uint8_t idx[A2600VID_FB_W * A2600VID_ROWS];  /* the same frame as TIA colour bytes */
    a2600vid_poke pokes[A2600VID_POKES];    /* oldest first, the frame being drawn (live) or the last one */
    a2600vid_line now;                      /* live: the registers right now (valid = machine stopped) */
    int npokes, pokes_dropped;
    int fb_w, fb_h;                         /* 160 x visible rows */
    int vis_top;                            /* scanline of row 0 */
    int total_lines;                        /* scanlines of the last frame (262 NTSC, 312 PAL) */
    int pal;                                /* PAL layout (50 Hz) */
    int vpos, hpos;                         /* the beam: scanlines into this frame, colour clock in the line (0..227) */
    int beam_row;                           /* rows of THIS frame drawn so far (fb_h: all; 0: none yet) */
    int wsync_cycles;                       /* CPU cycles held by WSYNC in the last frame */
    uint64_t frame;                         /* frames the TIA has completed */
    uint64_t serial;                        /* 0: nothing captured yet */
} a2600vid_capture;

/* ---- the host's capture, for core/src/vidtab.c ------------------------------------ */
int  a2600vid_capture_running(a2600session *s);
/* ref-counted: the host records while at least one user is on */
void a2600vid_capture_enable(a2600session *s, int on);
/* the newest finished frame when its serial differs from *serial (updated);
 * 1 if copied */
int  a2600vid_capture_get(a2600session *s, a2600vid_capture *dst, uint64_t *serial);
/* the machine as it is right now (rows above the beam from this frame, the
 * rest from the last; the last finished frame as picture); 1 on success.
 * Runs on the emulation thread between frames or at once when stopped. */
int  a2600vid_capture_fill(a2600session *s, a2600vid_capture *dst);

#ifdef __cplusplus
}
#endif

#endif
