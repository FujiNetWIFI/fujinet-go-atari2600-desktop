/*
 * a2600vidtab -- the Video tab's controller. All four frontends draw the same
 * tab from it, so what they own is widgets: a view and zoom chooser, overlay
 * toggles, a picture, the inspector and five tables. The lessons of the
 * other debuggers' video tabs live here once:
 *
 *   - the machine is only COPIED: a finished frame (picture, the TIA's
 *     registers at the end of every row, every TIA write with its beam
 *     position) is captured once per frame on the emulation thread, and a
 *     stopped machine is read while it is parked. Decoding, scaling and
 *     overlays run on the caller's (UI) thread;
 *   - the picture is rebuilt only when something it shows changed, into one
 *     reused buffer;
 *   - hover and pin are state here, so a mouse move never re-renders the
 *     picture: draw the marks from a2600vid_tab_marks() over the cached image;
 *   - pixels are 0xFFRRGGBB (opaque), whatever the toolkit does with alpha;
 *   - one table of overlays and one env-var parser, so the frontends cannot
 *     disagree.
 *
 * The TIA has no frame buffer and no object list: each of its 160 colour
 * clocks is the priority encoder's answer to "which of playfield, two players,
 * two missiles and the ball is on here". The host records the registers a row
 * ended with and the writes that happened during it; Decoded and Beam replay
 * those over the row (writes take effect at the colour clock they landed at;
 * Stella's few-clock write delays are not modelled), so mid-line changes
 * (asymmetric playfields, 48-pixel sprites, kernel colour bars) show. Display
 * is always the TIA's own finished frame.
 *
 * The picture is 320 wide: every colour clock is two pixels (the 2600's pixel
 * aspect), so a frontend scales it by whole numbers and nothing else.
 *
 * UI thread only. Not thread-safe.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef A2600VIDTAB_H
#define A2600VIDTAB_H

#include <stdint.h>

#include "a2600debug.h"
#include "a2600session.h"
#include "a2600vidcap.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct a2600vid_tab a2600vid_tab;

/* Picture pixels per colour clock. */
#define A2600VID_XSCALE 2

enum a2600vid_view {
    A2600VID_V_DISPLAY = 0,     /* the last finished frame, as the TIA drew it */
    A2600VID_V_DECODED,         /* the frame with each pixel tinted by the object that drew it (see a2600vid_source) */
    A2600VID_V_BEAM,            /* this frame above the beam, redrawn from the registers; the last one dimmed below (stopped) */
    A2600VID_V_VALUES,          /* the raw colour bytes as false colours: hue wheel by the high nibble, brightness by luma */
    A2600VID_V_COUNT
};

/* What Decoded shows. */
enum a2600vid_source {
    A2600VID_D_OBJECTS = 0,     /* the dimmed frame, each pixel tinted by what drew it */
    A2600VID_D_PALETTE,         /* the 128 TIA colours as a grid (16 hues x 8 luminances) */
    A2600VID_D_COUNT
};

/* What can draw a pixel, in the order of a2600vid_swatch.index and of
 * a2600vid_decode_row's object bytes. */
enum a2600vid_object_id {
    A2600VID_O_BK = 0, A2600VID_O_PF, A2600VID_O_P0, A2600VID_O_P1,
    A2600VID_O_M0, A2600VID_O_M1, A2600VID_O_BL, A2600VID_O_COUNT
};
const char *a2600vid_object_name(int object);        /* "BK", "PF", "P0", ... */

/* Overlays: one table, bit per entry. */
enum {
    A2600VID_OV_BEAM      = 1u << 0,    /* where the beam is (stopped) */
    A2600VID_OV_WRITES    = 1u << 1,    /* a mark at every TIA write, by kind, at the colour clock it landed on */
    A2600VID_OV_CHANGED   = 1u << 2,    /* rows where any TIA register differs from the row above */
    A2600VID_OV_POSITIONS = 1u << 3,    /* a tick at each object's position on every row */
    A2600VID_OV_BLANK     = 1u << 4,    /* rows with VBLANK on (black) */
    A2600VID_OV_ALL       = 0x1Fu
};
typedef struct {
    unsigned bit;
    const char *name;           /* the env name: beam, writes, ... */
    const char *label;          /* the checkbox text */
} a2600vid_overlay_info;
const a2600vid_overlay_info *a2600vid_overlays(int *count);
const char *a2600vid_view_name(int view);            /* "Display", ... */
const char *a2600vid_view_key(int view);             /* "display", ... (env) */
const char *a2600vid_source_name(int source);        /* "Objects", "Palette" */
const char *a2600vid_source_key(int source);         /* "objects", "palette" (env) */

/* Right-hand sections. */
enum { A2600VID_S_PIXEL = 0, A2600VID_S_REGISTERS, A2600VID_S_COLOURS,
       A2600VID_S_LINES, A2600VID_S_WRITES, A2600VID_S_TIMING, A2600VID_S_COUNT };
const char *a2600vid_section_name(int section);
const char *a2600vid_section_key(int section);

/* sess is the session the tab captures from, asks whether the machine is
 * stopped, and runs Break on... commands on (through its debugger); NULL
 * (tests) makes every Break on... return -1 and the machine count as running.
 * The tab asks the host to capture frames until it is freed. */
a2600vid_tab *a2600vid_tab_new(a2600session *sess);
void          a2600vid_tab_free(a2600vid_tab *tab);

/* ---- what to show (they mark the picture stale) ------------------------------------ */
void     a2600vid_tab_set_view(a2600vid_tab *tab, int view);
int      a2600vid_tab_view(const a2600vid_tab *tab);
void     a2600vid_tab_set_source(a2600vid_tab *tab, int source);   /* of Decoded */
int      a2600vid_tab_source(const a2600vid_tab *tab);
void     a2600vid_tab_set_overlays(a2600vid_tab *tab, unsigned overlays);
unsigned a2600vid_tab_overlays(const a2600vid_tab *tab);
/* 1, 2, 3, 4, 6, 8, 12, 16, or 0 for "fit the viewport" (the largest whole zoom that fits both ways) */
void     a2600vid_tab_set_zoom(a2600vid_tab *tab, int zoom);
int      a2600vid_tab_zoom(const a2600vid_tab *tab);
/* the room the picture has, for Fit; call when the widget resizes */
void     a2600vid_tab_set_viewport(a2600vid_tab *tab, int width, int height);
/* the zoom steps the chooser offers (0 = Fit first) */
const int *a2600vid_zooms(int *count);

/*
 * The scripted-capture variables, read once by a2600vid_tab_new:
 *   A2600_DEBUGGER_VIDEO_VIEW      display decoded beam values, or a Decoded source:
 *                                  objects palette
 *   A2600_DEBUGGER_VIDEO_OVERLAYS  beam,writes,changed,positions,blank | all | none
 *   A2600_DEBUGGER_VIDEO_ZOOM      1..16 or fit
 *   A2600_DEBUGGER_VIDEO_SECTION   pixel registers colours lines writes timing   (names, never indexes)
 * Returns the starting section, A2600VID_S_PIXEL when unset.
 */
int a2600vid_tab_env_section(const a2600vid_tab *tab);

/* ---- refresh ------------------------------------------------------------------------ */
#define A2600VID_TAB_PICTURE 1u  /* repaint the picture (a2600vid_tab_picture) */
#define A2600VID_TAB_TABLES  2u  /* refill the status line and the sections */
#define A2600VID_TAB_OFF     4u  /* no machine: show a2600vid_tab_why() instead of a picture */

/* Take the machine's state, update the trackers, and say what to redraw.
 * force: redraw everything. */
unsigned a2600vid_tab_refresh(a2600vid_tab *tab, int force);

/* For tests: use this frame instead of the machine. live: the machine is
 * stopped (the Beam view works; cap->beam_row says how far the beam has
 * drawn the frame); 0: a running machine, so no Beam view. */
void a2600vid_tab_feed(a2600vid_tab *tab, const a2600vid_capture *cap, int live);

/* The console colour (0-255) when no debugger can say (tests, no machine):
 * a hue by the high nibble, brightness by the low one. */
uint32_t a2600vid_fallback_colour(uint8_t index);

/*
 * The TIA's priority encoder for one visible row, as the tab uses it: for
 * each of the 160 colour clocks the TIA colour byte, the object that won
 * (a2600vid_object_id) and a mask of every object that was on there (bit =
 * 1 << object id). The row's end-of-row registers are replayed backwards and
 * forwards through the writes recorded for it. Returns 0 if the row has no
 * record. Any pointer but cap may be NULL.
 */
int a2600vid_decode_row(const a2600vid_capture *cap, int row, uint8_t colour[A2600VID_FB_W],
                        uint8_t object[A2600VID_FB_W], uint8_t covers[A2600VID_FB_W]);

/* ---- the picture ------------------------------------------------------------------- */
/*
 * The rendered picture, 0xFFRRGGBB, valid until the next refresh or set_*. NULL
 * when this view cannot be drawn: a2600vid_tab_why() says why, and the tab
 * should show that text.
 */
const uint32_t *a2600vid_tab_picture(a2600vid_tab *tab, int *width, int *height);
const char     *a2600vid_tab_why(const a2600vid_tab *tab);
int             a2600vid_tab_effective_zoom(const a2600vid_tab *tab);

/* ---- the tables -------------------------------------------------------------------- */
const char *a2600vid_tab_status(const a2600vid_tab *tab);      /* two lines, '\n' between */

typedef struct {
    const char *name;           /* "COLUP0", "PF1", ... */
    char value[24];             /* "5B", "$1800" */
    char meaning[112];
    int age;                    /* refreshes since it last changed (A2600VID_REG_FADE+: not recent) */
} a2600vid_reg;
#define A2600VID_REG_FADE 12
/* The registers of the row the beam is on (stopped: the live registers), else the last row drawn. */
int a2600vid_tab_regs(const a2600vid_tab *tab, const a2600vid_reg **out);

/* One entry per object: what colour register it is drawn with, and how much of the frame it drew. */
typedef struct {
    int index;                  /* a2600vid_object_id */
    char name[4];               /* "BK", "PF", "P0", "P1", "M0", "M1", "BL" */
    const char *reg_name;       /* "COLUBK", "COLUPF", "COLUP0", "COLUP1", "COLUP0" (M0), "COLUP1" (M1), "COLUPF" (BL) */
    unsigned address;           /* the colour register's TIA address ($06-$09) */
    uint8_t value;              /* its TIA colour byte as written */
    uint32_t rgb;               /* 0xFFRRGGBB through a2600debug_tia_color */
    long uses;                  /* screen pixels of this frame this object drew */
    int inspected;              /* the inspected pixel was drawn by it */
} a2600vid_swatch;
int a2600vid_tab_swatches(const a2600vid_tab *tab, const a2600vid_swatch **out);
/* A write trap on that object's colour register ("trapWrite"); 0, or -1 */
int a2600vid_tab_break_on_swatch(a2600vid_tab *tab, int index);
/* A write trap on any TIA register by its address (0-63); 0, or -1 */
int a2600vid_tab_break_on_register(a2600vid_tab *tab, unsigned reg);
/* TIA write register name for an address: "COLUP0", "GRP1", ... ("$2E" for none) */
const char *a2600vid_reg_name(unsigned reg);

/* The per-row table: one entry for the first drawn row and for each row whose
 * registers differ from the row above. */
enum { A2600VID_LC_COLOUR = 1, A2600VID_LC_PLAYFIELD = 2, A2600VID_LC_GRAPHICS = 4,
       A2600VID_LC_POSITION = 8, A2600VID_LC_WRITES = 16 };
typedef struct {
    int row;                    /* frame row */
    int scanline;               /* the screen scanline (row + the picture's top) */
    uint8_t colup0, colup1, colupf, colubk;
    uint8_t pf0, pf1, pf2, ctrlpf;
    uint8_t grp0, grp1, nusiz0, nusiz1;
    uint8_t enables;            /* bit0 ENAM0, bit1 ENAM1, bit2 ENABL */
    uint8_t flags;              /* A2600VID_F_* */
    uint8_t pos[A2600VID_NOBJ]; /* P0 P1 M0 M1 BL */
    int writes;                 /* TIA writes during the row */
    int changed_mask;           /* A2600VID_LC_*: what differs from the row above */
} a2600vid_line_row;
int a2600vid_tab_lines(const a2600vid_tab *tab, const a2600vid_line_row **out);

/* The writes the frame made, oldest first (the Writes section): the frame the
 * rows belong to, including the vblank and overscan writes around it. */
typedef struct {
    const char *name;           /* "COLUBK", ... */
    unsigned reg;               /* 0-63 */
    uint8_t value, old;
    int clock;                  /* colour clock -68..159 (hblank is -68..-1) */
    int row;                    /* frame row, -1 outside the picture */
    int scanline;               /* the screen scanline of that row, -1 when row is -1 */
} a2600vid_write;
int a2600vid_tab_writes(const a2600vid_tab *tab, const a2600vid_write **out);
int a2600vid_tab_writes_dropped(const a2600vid_tab *tab);   /* lost to the host's ring */

/* the timing section, as ready-made "label\tvalue" lines */
int  a2600vid_tab_timing(const a2600vid_tab *tab, const char *const **lines);
/* where the beam is in the whole frame, 0..1000 (0 when unknown) */
int  a2600vid_tab_beam_permille(const a2600vid_tab *tab);

/* A section as plain text (the console's `video` command shares it). */
int  a2600vid_tab_text(a2600vid_tab *tab, int section, char *dst, int dstsz);

/* ---- hover, pin ---------------------------------------------------------------------- */
typedef struct {
    int valid;
    int active;                 /* a row the TIA drew (not the palette grid, not past the capture) */
    int pic_x, pic_y;           /* picture pixel (before zoom) */
    int clock;                  /* colour clock 0..159, -1 in the palette grid */
    int row;                    /* frame row, -1 in a picture that is not the frame */
    int scanline;
    int object;                 /* a2600vid_object_id of the winner, -1 if none (palette grid) */
    unsigned covers;            /* 1 << object id for every object on at this clock */
    uint8_t colour;             /* the TIA colour byte of the pixel (what was drawn) */
    uint8_t value;              /* the colour register that produced it, as written (0 when blank) */
    uint32_t rgb;               /* 0xFFRRGGBB */
    int writes;                 /* TIA writes during the row */
    char text[768];             /* ready-made inspector text */
} a2600vid_pixel;

/* pic_x, pic_y in picture pixels at the current zoom; -1,-1 when the pointer left.
 * Returns 1 when the inspector changed. */
int  a2600vid_tab_hover(a2600vid_tab *tab, int pic_x, int pic_y);
void a2600vid_tab_pin(a2600vid_tab *tab, int pic_x, int pic_y);    /* a click; the same spot unpins */
void a2600vid_tab_unpin(a2600vid_tab *tab);
/* the pinned pixel if any, else the hovered one, else NULL */
const a2600vid_pixel *a2600vid_tab_pixel(const a2600vid_tab *tab);
/* rectangles x,y,w,h in picture pixels (zoomed) for the marks drawn over the
 * picture; bit0 of the result: hover valid, bit1: pin valid */
int  a2600vid_tab_marks(const a2600vid_tab *tab, int hover[4], int pin[4]);
/* Write traps on the registers behind the inspected pixel: the object's
 * colour register, its graphics register (GRPn / PFn+CTRLPF / ENAMn / ENABL)
 * and its position strobe. Not added twice. 0, or -1. */
int  a2600vid_tab_break_on_pixel(a2600vid_tab *tab);
/* the object (a2600vid_object_id) of the inspected pixel, or -1 */
int  a2600vid_tab_pixel_object(const a2600vid_tab *tab);

#ifdef __cplusplus
}
#endif

#endif
