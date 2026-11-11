/*
 * vidtab_test -- the Video tab's controller (core/src/vidtab.c) fed
 * synthetic frames: no machine, no ROMs. The TIA's priority encoder replayed
 * from end-of-row registers and the writes of a row (player copies, sizes,
 * reflection, missiles, ball, playfield, priority, score mode, mid-row
 * writes, a position strobe, the HMOVE comb), the views, the inspector, the
 * tables, register ages, and a fuzz over random captures, every view,
 * source, overlay and zoom.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "a2600vidcap.h"
#include "a2600vidtab.h"

/* MinGW has no setenv/unsetenv; _putenv_s does the same job there. */
static void set_env(const char *name, const char *value)
{
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

static int fails;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); fails++; } \
                              else printf("ok: %s\n", msg); } while (0)

#define FBH 192
#define VIS_TOP 40
#define WIDE 320

#define COL_P0 0x1E
#define COL_P1 0x6E
#define COL_PF 0x4E
#define COL_BK 0x80

static a2600vid_capture cap;

static uint32_t console(uint8_t byte)
{
    return a2600vid_fallback_colour(byte);
}

static void make_cap(a2600vid_capture *out, int fbh)
{
    memset(out, 0, sizeof *out);
    out->fb_w = A2600VID_FB_W; out->fb_h = fbh;
    out->vis_top = VIS_TOP;
    out->total_lines = 262;
    out->vpos = 200; out->hpos = 100;
    out->beam_row = fbh;
    out->frame = 5;
    out->serial = 1;
    for (int pix = 0; pix < A2600VID_FB_W * fbh; pix++) { out->pixels[pix] = 0x00123456u; out->idx[pix] = COL_BK; }
    for (int row = 0; row < fbh; row++) {
        a2600vid_line *line = &out->lines[row];
        line->valid = 1;
        line->colup0 = COL_P0; line->colup1 = COL_P1; line->colupf = COL_PF; line->colubk = COL_BK;
        line->pos[A2600VID_P0] = 40; line->pos[A2600VID_P1] = 100;
        line->pos[A2600VID_M0] = 30; line->pos[A2600VID_M1] = 120; line->pos[A2600VID_BL] = 60;
        line->hmove[0] = line->hmove[1] = line->hmove[2] = line->hmove[3] = line->hmove[4] = 8;
    }
}

static a2600vid_poke *add_poke(a2600vid_capture *out, int row, int col, int reg, int value, int old)
{
    a2600vid_poke *poke = &out->pokes[out->npokes++];
    poke->row = (int16_t)row; poke->col = (int16_t)col;
    poke->reg = (uint8_t)reg; poke->value = (uint8_t)value; poke->old = (uint8_t)old;
    if (row >= 0) out->lines[row].pokes++;
    return poke;
}

static void settle(a2600vid_tab *tab, int view, int live)
{
    a2600vid_tab_set_view(tab, view);
    a2600vid_tab_feed(tab, &cap, live);
    a2600vid_tab_refresh(tab, 1);
}

static uint8_t colour_row[A2600VID_FB_W], object_row[A2600VID_FB_W], covers_row[A2600VID_FB_W];
static int decode(int row)
{
    return a2600vid_decode_row(&cap, row, colour_row, object_row, covers_row);
}
static int count_object(int object)
{
    int num = 0;
    for (int clock = 0; clock < A2600VID_FB_W; clock++) num += object_row[clock] == object;
    return num;
}

int main(void)
{
    a2600vid_tab *tab;
    const uint32_t *pic;
    int wide, high;

    set_env("A2600_DEBUGGER_VIDEO_VIEW", "palette");
    set_env("A2600_DEBUGGER_VIDEO_OVERLAYS", "beam,writes");
    set_env("A2600_DEBUGGER_VIDEO_ZOOM", "3");
    set_env("A2600_DEBUGGER_VIDEO_SECTION", "wri");
    tab = a2600vid_tab_new(NULL);
    CHECK(tab && a2600vid_tab_view(tab) == A2600VID_V_DECODED && a2600vid_tab_source(tab) == A2600VID_D_PALETTE &&
          a2600vid_tab_zoom(tab) == 3 && a2600vid_tab_overlays(tab) == (A2600VID_OV_BEAM | A2600VID_OV_WRITES) &&
          a2600vid_tab_env_section(tab) == A2600VID_S_WRITES, "env variables name the view, overlays, zoom, section");
    a2600vid_tab_free(tab);
    set_env("A2600_DEBUGGER_VIDEO_VIEW", "beam");
    set_env("A2600_DEBUGGER_VIDEO_OVERLAYS", "none");
    set_env("A2600_DEBUGGER_VIDEO_ZOOM", "fit");
    set_env("A2600_DEBUGGER_VIDEO_SECTION", "");
    tab = a2600vid_tab_new(NULL);
    CHECK(tab && a2600vid_tab_view(tab) == A2600VID_V_BEAM && a2600vid_tab_overlays(tab) == 0 && a2600vid_tab_zoom(tab) == 0 &&
          a2600vid_tab_env_section(tab) == A2600VID_S_PIXEL, "view names, none, fit");
    a2600vid_tab_free(tab);
    set_env("A2600_DEBUGGER_VIDEO_VIEW", "");
    set_env("A2600_DEBUGGER_VIDEO_OVERLAYS", "all");
    tab = a2600vid_tab_new(NULL);
    CHECK(a2600vid_tab_overlays(tab) == A2600VID_OV_ALL, "overlays all");
    a2600vid_tab_free(tab);
    set_env("A2600_DEBUGGER_VIDEO_OVERLAYS", "beam");
    set_env("A2600_DEBUGGER_VIDEO_ZOOM", "");

    /* ---- the priority encoder ---- */
    make_cap(&cap, FBH);
    CHECK(!a2600vid_decode_row(&cap, FBH + 5, colour_row, object_row, covers_row) && !a2600vid_decode_row(&cap, -1, NULL, NULL, NULL),
          "a row with no record decodes to nothing");
    CHECK(decode(10) && count_object(A2600VID_O_BK) == A2600VID_FB_W && colour_row[77] == COL_BK, "an empty row is all background");

    cap.lines[20].grp0 = 0x81;
    decode(20);
    CHECK(object_row[40] == A2600VID_O_P0 && object_row[47] == A2600VID_O_P0 && object_row[41] == A2600VID_O_BK &&
          object_row[39] == A2600VID_O_BK && object_row[48] == A2600VID_O_BK && colour_row[40] == COL_P0 &&
          (covers_row[40] & (1u << A2600VID_O_P0)), "player 0: GRP0 $81 at position 40 is clocks 40 and 47");
    cap.lines[21].grp0 = 0x81; cap.lines[21].nusiz0 = 1;
    decode(21);
    CHECK(object_row[40] == A2600VID_O_P0 && object_row[47] == A2600VID_O_P0 && object_row[56] == A2600VID_O_P0 &&
          object_row[63] == A2600VID_O_P0 && count_object(A2600VID_O_P0) == 4, "NUSIZ 1: a second copy 16 clocks on");
    cap.lines[22].grp0 = 0x80;
    decode(22);
    CHECK(object_row[40] == A2600VID_O_P0 && count_object(A2600VID_O_P0) == 1, "GRP0 $80 is the first pixel");
    cap.lines[22].flags = A2600VID_F_REFP0;
    decode(22);
    CHECK(object_row[47] == A2600VID_O_P0 && object_row[40] == A2600VID_O_BK && count_object(A2600VID_O_P0) == 1, "REFP0 reflects it");
    cap.lines[23].grp0 = 0x80; cap.lines[23].nusiz0 = 5;
    decode(23);
    CHECK(object_row[40] == A2600VID_O_P0 && object_row[41] == A2600VID_O_P0 && count_object(A2600VID_O_P0) == 2, "NUSIZ 5: double size");
    cap.lines[24].grp0 = 0x80; cap.lines[24].nusiz0 = 7;
    decode(24);
    CHECK(count_object(A2600VID_O_P0) == 4 && object_row[43] == A2600VID_O_P0, "NUSIZ 7: quad size");
    cap.lines[25].grp0 = 0x80; cap.lines[25].nusiz0 = 6;
    decode(25);
    CHECK(object_row[40] == A2600VID_O_P0 && object_row[72] == A2600VID_O_P0 && object_row[104] == A2600VID_O_P0 &&
          count_object(A2600VID_O_P0) == 3, "NUSIZ 6: three copies, medium");
    cap.lines[26].grp0 = 0x80; cap.lines[26].pos[A2600VID_P0] = 156; cap.lines[26].grp0 = 0xF0;
    decode(26);
    CHECK(object_row[156] == A2600VID_O_P0 && object_row[159] == A2600VID_O_P0 && count_object(A2600VID_O_P0) == 4,
          "a player at the right edge keeps its pixels");
    cap.lines[27].grp0 = 0xFF; cap.lines[27].pos[A2600VID_P0] = 156;
    decode(27);
    CHECK(object_row[0] == A2600VID_O_P0 && object_row[3] == A2600VID_O_P0 && object_row[4] == A2600VID_O_BK,
          "...and wraps to the left edge");

    cap.lines[30].enam0 = 1; cap.lines[30].nusiz0 = 0x20;
    decode(30);
    CHECK(object_row[30] == A2600VID_O_M0 && object_row[33] == A2600VID_O_M0 && object_row[34] == A2600VID_O_BK &&
          count_object(A2600VID_O_M0) == 4 && colour_row[30] == COL_P0, "missile 0: 4 wide, in COLUP0");
    cap.lines[30].flags = A2600VID_F_RESMP0;
    decode(30);
    CHECK(count_object(A2600VID_O_M0) == 0, "RESMP0 hides it");
    cap.lines[31].enam1 = 1;
    decode(31);
    CHECK(object_row[120] == A2600VID_O_M1 && colour_row[120] == COL_P1, "missile 1");
    cap.lines[32].enabl = 1; cap.lines[32].ctrlpf = 0x30;
    decode(32);
    CHECK(object_row[60] == A2600VID_O_BL && object_row[67] == A2600VID_O_BL && object_row[68] == A2600VID_O_BK &&
          colour_row[60] == COL_PF, "the ball: 8 wide, in COLUPF");
    cap.lines[33].enabl_old = 1; cap.lines[33].flags = A2600VID_F_VDELBL;
    decode(33);
    CHECK(count_object(A2600VID_O_BL) == 1, "VDELBL selects the delayed copy");

    cap.lines[40].pf0 = 0xF0;
    decode(40);
    CHECK(object_row[0] == A2600VID_O_PF && object_row[15] == A2600VID_O_PF && object_row[16] == A2600VID_O_BK &&
          object_row[80] == A2600VID_O_PF && object_row[95] == A2600VID_O_PF && count_object(A2600VID_O_PF) == 32 &&
          colour_row[0] == COL_PF, "PF0 $F0 is clocks 0-15, repeated on the right");
    cap.lines[41].pf0 = 0xF0; cap.lines[41].ctrlpf = 1;
    decode(41);
    CHECK(object_row[144] == A2600VID_O_PF && object_row[159] == A2600VID_O_PF && object_row[80] == A2600VID_O_BK &&
          count_object(A2600VID_O_PF) == 32, "CTRLPF bit 0 mirrors it");
    cap.lines[42].pf1 = 0x80; cap.lines[42].pf2 = 0x01;
    decode(42);
    CHECK(object_row[16] == A2600VID_O_PF && object_row[19] == A2600VID_O_PF && object_row[20] == A2600VID_O_BK &&
          object_row[48] == A2600VID_O_PF && count_object(A2600VID_O_PF) == 16, "PF1 bit 7 is clocks 16-19, PF2 bit 0 clocks 48-51");

    cap.lines[44].pf0 = 0xF0; cap.lines[44].grp0 = 0xFF; cap.lines[44].pos[A2600VID_P0] = 4;
    decode(44);
    CHECK(object_row[4] == A2600VID_O_P0 && object_row[0] == A2600VID_O_PF && object_row[12] == A2600VID_O_PF,
          "a player is in front of the playfield");
    cap.lines[44].ctrlpf = 4;
    decode(44);
    CHECK(object_row[4] == A2600VID_O_PF && object_row[0] == A2600VID_O_PF && object_row[16] == A2600VID_O_BK &&
          (covers_row[4] & (1u << A2600VID_O_P0)), "CTRLPF bit 2: the playfield is in front (the player is still 'on')");
    cap.lines[45].pf0 = 0xF0; cap.lines[45].ctrlpf = 2;
    decode(45);
    CHECK(colour_row[0] == COL_P0 && colour_row[80] == COL_P1, "score mode paints the halves in the players' colours");
    cap.lines[46].flags = A2600VID_F_VBLANK; cap.lines[46].grp0 = 0xFF;
    decode(46);
    CHECK(count_object(A2600VID_O_BK) == A2600VID_FB_W && colour_row[40] == 0 && covers_row[40] == 0, "VBLANK: the row is black");

    /* writes during the row */
    cap.lines[60].colubk = 0x44;
    add_poke(&cap, 60, 80, 9, 0x44, 0x22);
    decode(60);
    CHECK(colour_row[79] == 0x22 && colour_row[80] == 0x44, "COLUBK written at clock 80: two colours across the row");
    cap.lines[61].grp0 = 0xFF; cap.lines[61].pos[A2600VID_P0] = 30;
    add_poke(&cap, 61, 20, 0x1B, 0xFF, 0x00);
    decode(61);
    CHECK(object_row[30] == A2600VID_O_P0 && object_row[37] == A2600VID_O_P0, "GRP0 written before the player's clocks draws it");
    cap.lines[62].grp0 = 0xFF; cap.lines[62].pos[A2600VID_P0] = 30;
    add_poke(&cap, 62, 50, 0x1B, 0xFF, 0x00);
    decode(62);
    CHECK(count_object(A2600VID_O_P0) == 0, "...after them it draws nothing this row");
    cap.lines[63].grp0 = 0xFF; cap.lines[63].pos[A2600VID_P0] = 90;
    add_poke(&cap, 63, 60, 0x10, 0, 0);
    decode(63);
    CHECK(object_row[30] == A2600VID_O_P0 && object_row[37] == A2600VID_O_P0 && object_row[38] == A2600VID_O_BK &&
          object_row[90] == A2600VID_O_P0 && object_row[97] == A2600VID_O_P0 && count_object(A2600VID_O_P0) == 16,
          "a RESP0 strobe: the previous row's position before it, the new one after");
    cap.lines[66].grp0 = 0xFF;
    add_poke(&cap, 66, -59, 0x2A, 0, 0);
    cap.lines[66].pos[A2600VID_P0] = 2;
    decode(66);
    CHECK(colour_row[0] == 0 && colour_row[7] == 0 && colour_row[8] == COL_P0, "HMOVE at the start of the row blanks the first 8 clocks");
    {
        const int before = cap.npokes;
        add_poke(&cap, -1, -30, 0x00, 2, 0);
        add_poke(&cap, 70, 10, 0x06, 0x30, 0x1E);
        CHECK(cap.npokes == before + 2, "(writes outside the picture are recorded with row -1)");
    }

    /* ---- Display ---- */
    make_cap(&cap, FBH);
    tab = a2600vid_tab_new(NULL);
    a2600vid_tab_set_overlays(tab, 0);
    a2600vid_tab_set_zoom(tab, 1);
    settle(tab, A2600VID_V_DISPLAY, 0);
    pic = a2600vid_tab_picture(tab, &wide, &high);
    CHECK(pic && wide == WIDE && high == FBH && pic[0] == 0xff123456u && pic[WIDE * FBH - 1] == 0xff123456u,
          "Display is the captured frame, two pixels a colour clock");
    cap.pixels[77 * A2600VID_FB_W + 11] = 0x00abcdefu;
    settle(tab, A2600VID_V_DISPLAY, 0);
    pic = a2600vid_tab_picture(tab, &wide, &high);
    CHECK(pic && pic[77 * wide + 22] == 0xffabcdefu && pic[77 * wide + 23] == 0xffabcdefu && pic[77 * wide + 24] == 0xff123456u,
          "...pixel for pixel");
    a2600vid_tab_set_zoom(tab, 3);
    a2600vid_tab_refresh(tab, 1);
    pic = a2600vid_tab_picture(tab, &wide, &high);
    CHECK(pic && wide == WIDE * 3 && high == FBH * 3 && a2600vid_tab_effective_zoom(tab) == 3, "zoom 3");
    a2600vid_tab_set_zoom(tab, 0);
    a2600vid_tab_set_viewport(tab, 700, 450);
    a2600vid_tab_refresh(tab, 1);
    CHECK(a2600vid_tab_effective_zoom(tab) == 2, "fit: the largest whole zoom that fits both ways");
    a2600vid_tab_set_zoom(tab, 1);

    /* ---- Decoded: tint, not fill ---- */
    make_cap(&cap, FBH);
    cap.lines[20].grp0 = 0x81;
    settle(tab, A2600VID_V_DECODED, 0);
    a2600vid_tab_set_source(tab, A2600VID_D_OBJECTS);
    a2600vid_tab_refresh(tab, 1);
    pic = a2600vid_tab_picture(tab, &wide, &high);
    {
        const uint32_t dim = 0xff000000u | (((0x12u * 106) >> 8) << 16) | (((0x34u * 106) >> 8) << 8) | ((0x56u * 106) >> 8);
        CHECK(pic && pic[20 * wide + 5] == dim && pic[20 * wide + 84] == dim && pic[19 * wide + 80] == dim,
              "Decoded: the rest of the frame is the dimmed capture");
        CHECK(pic && pic[20 * wide + 80] != dim && pic[20 * wide + 81] == pic[20 * wide + 80] && pic[20 * wide + 94] == pic[20 * wide + 80] &&
              pic[20 * wide + 82] == dim, "...an object's pixels are tinted (clock 40 and 47, 2 pixels each)");
        CHECK(pic && pic[20 * wide + 80] != 0xffff5a30u, "...not an opaque fill");
    }
    a2600vid_tab_set_source(tab, A2600VID_D_PALETTE);
    a2600vid_tab_refresh(tab, 1);
    pic = a2600vid_tab_picture(tab, &wide, &high);
    CHECK(pic && wide == 320 && high == 160, "palette: 16 x 8 swatches");
    a2600vid_tab_hover(tab, 3 * 20 + 5, 2 * 20 + 5);
    CHECK(a2600vid_tab_pixel(tab) && a2600vid_tab_pixel(tab)->colour == 0x34 && strstr(a2600vid_tab_pixel(tab)->text, "$34") &&
          a2600vid_tab_pixel(tab)->rgb == console(0x34), "palette: the cell is the colour byte ($34 = hue 3, luminance 2)");
    {
        int hov[4], pin[4];
        CHECK(a2600vid_tab_marks(tab, hov, pin) == 1 && hov[0] == 60 && hov[1] == 40 && hov[2] == 20 && hov[3] == 20, "palette: the mark is the cell");
    }
    a2600vid_tab_hover(tab, -1, -1);
    a2600vid_tab_set_source(tab, A2600VID_D_OBJECTS);

    /* ---- Values ---- */
    make_cap(&cap, FBH);
    cap.idx[30 * A2600VID_FB_W + 50] = 0x1E;
    settle(tab, A2600VID_V_VALUES, 0);
    pic = a2600vid_tab_picture(tab, &wide, &high);
    CHECK(pic && wide == WIDE && pic[30 * wide + 100] == console(0x1E) && pic[30 * wide + 101] == console(0x1E) &&
          pic[30 * wide + 102] == console(COL_BK) && console(0x1E) != console(COL_BK), "Values: the colour bytes as false colours");

    /* ---- Beam ---- */
    make_cap(&cap, FBH);
    cap.lines[20].grp0 = 0x81;
    settle(tab, A2600VID_V_BEAM, 0);
    CHECK(a2600vid_tab_picture(tab, &wide, &high) == NULL && strlen(a2600vid_tab_why(tab)) > 10, "Beam needs a stopped machine and says why");
    cap.beam_row = 100;
    a2600vid_tab_feed(tab, &cap, 1);
    a2600vid_tab_refresh(tab, 1);
    pic = a2600vid_tab_picture(tab, &wide, &high);
    CHECK(pic && pic[20 * wide + 80] == console(COL_P0) && pic[20 * wide + 82] == console(COL_BK) &&
          pic[20 * wide + 82] != 0xff123456u, "Beam: above the beam the frame is redrawn from the registers");
    CHECK(pic && pic[150 * wide + 5] != 0xff123456u && (pic[150 * wide + 5] & 0xff) < 0x56 && (pic[150 * wide + 5] & 0xff) > 0,
          "Beam: below it the last frame, dimmed");
    {
        const char *const *tlines;
        CHECK(a2600vid_tab_timing(tab, &tlines) >= 8 && strchr(tlines[0], '\t'), "timing lines are label<TAB>value");
        CHECK(a2600vid_tab_beam_permille(tab) == 200 * 1000 / 262, "beam per-mille");
    }

    /* ---- overlays and zoom: every one, opaque ---- */
    a2600vid_tab_set_overlays(tab, A2600VID_OV_ALL);
    a2600vid_tab_set_zoom(tab, 4);
    cap.lines[50].flags = A2600VID_F_VBLANK; cap.lines[51].colubk = 0x22;
    add_poke(&cap, 51, 70, 9, 0x22, 0x80); add_poke(&cap, 52, -5, 0x10, 0, 0);
    settle(tab, A2600VID_V_DISPLAY, 1);
    pic = a2600vid_tab_picture(tab, &wide, &high);
    CHECK(pic && wide == WIDE * 4 && high == FBH * 4, "zoom 4 with every overlay");
    {
        int opaque = 1;
        for (int pix = 0; pic && pix < wide * high; pix++) if ((pic[pix] >> 24) != 0xff) opaque = 0;
        CHECK(opaque, "every pixel opaque");
    }
    CHECK(pic && pic[100 * 4 * wide + 3] != 0xff123456u, "the beam line is drawn");
    for (int view = 0; view < A2600VID_V_COUNT; view++) {
        a2600vid_tab_set_source(tab, view % A2600VID_D_COUNT);
        settle(tab, view, 1);
    }
    CHECK(1, "every view and source with every overlay, stopped");
    a2600vid_tab_set_overlays(tab, 0);
    a2600vid_tab_set_zoom(tab, 1);
    a2600vid_tab_set_source(tab, A2600VID_D_OBJECTS);

    /* ---- the inspector ---- */
    make_cap(&cap, FBH);
    cap.lines[20].grp0 = 0x81; cap.lines[20].pf0 = 0xF0; cap.lines[20].colubk = 0x44;
    add_poke(&cap, 20, 12, 0x09, 0x44, 0x80);
    settle(tab, A2600VID_V_DISPLAY, 0);
    a2600vid_tab_hover(tab, 2 * 40 + 1, 20);
    {
        const a2600vid_pixel *pix = a2600vid_tab_pixel(tab);
        CHECK(pix && pix->active && pix->clock == 40 && pix->row == 20 && pix->scanline == 20 + VIS_TOP &&
              pix->object == A2600VID_O_P0 && pix->value == COL_P0 && pix->writes == 1 &&
              (pix->covers & (1u << A2600VID_O_P0)) && !(pix->covers & (1u << A2600VID_O_PF)),
              "the inspector: clock, row, scanline, the object that drew the pixel");
        CHECK(pix && strstr(pix->text, "Drawn by P0") && strstr(pix->text, "colour $1E") && strstr(pix->text, "COLUBK=$44@12") &&
              strstr(pix->text, "1 write this row") && strstr(pix->text, "Positions P0 40"), "...and its text");
    }
    a2600vid_tab_hover(tab, 2 * 8, 20);
    CHECK(a2600vid_tab_pixel(tab) && a2600vid_tab_pixel(tab)->object == A2600VID_O_PF && a2600vid_tab_pixel_object(tab) == A2600VID_O_PF &&
          strstr(a2600vid_tab_pixel(tab)->text, "Drawn by PF"), "a playfield pixel");
    a2600vid_tab_hover(tab, 2 * 70, 20);
    CHECK(a2600vid_tab_pixel(tab) && strstr(a2600vid_tab_pixel(tab)->text, "nothing (background)"), "a pixel nothing covers says so");
    a2600vid_tab_hover(tab, 5, FBH - 1);
    CHECK(a2600vid_tab_pixel(tab) && a2600vid_tab_pixel(tab)->row == FBH - 1 && a2600vid_tab_pixel(tab)->active, "the last row");
    CHECK(a2600vid_tab_break_on_pixel(tab) == -1 && a2600vid_tab_break_on_swatch(tab, 2) == -1 &&
          a2600vid_tab_break_on_register(tab, 9) == -1, "no debugger: no write traps");
    a2600vid_tab_pin(tab, 10, 20);
    CHECK(a2600vid_tab_pixel(tab) && a2600vid_tab_pixel(tab)->pic_x == 10, "pin");
    a2600vid_tab_hover(tab, 50, 50);
    CHECK(a2600vid_tab_pixel(tab)->pic_x == 10, "the pin wins over the hover");
    {
        int hov[4], pin[4];
        CHECK(a2600vid_tab_marks(tab, hov, pin) == 3 && pin[0] == 10 && pin[1] == 20 && pin[2] == 2 && pin[3] == 1 && hov[0] == 50,
              "marks: a colour clock wide, a row high");
    }
    a2600vid_tab_pin(tab, 10, 20);
    CHECK(a2600vid_tab_pixel(tab)->pic_x == 50, "the same spot unpins");
    a2600vid_tab_unpin(tab);
    a2600vid_tab_hover(tab, -1, -1);
    CHECK(a2600vid_tab_pixel(tab) == NULL, "the pointer left");

    /* ---- the tables ---- */
    make_cap(&cap, FBH);
    for (int row = 100; row < FBH; row++) cap.lines[row].colubk = 0x22;
    for (int row = 150; row < FBH; row++) cap.lines[row].pf1 = 0x0F;
    for (int row = 170; row < 180; row++) cap.lines[row].pos[A2600VID_P1] = (uint8_t)(100 + row - 170);
    add_poke(&cap, 100, 5, 9, 0x22, 0x80);
    add_poke(&cap, -1, -20, 0x01, 0, 0x02);
    cap.pokes_dropped = 3;
    settle(tab, A2600VID_V_DISPLAY, 0);
    {
        const a2600vid_line_row *rows;
        const a2600vid_write *writes;
        const int num = a2600vid_tab_lines(tab, &rows);
        int colour = 0, playfield = 0, position = 0;
        for (int idx = 0; idx < num; idx++) {
            if ((rows[idx].changed_mask & A2600VID_LC_COLOUR) && rows[idx].row == 100) colour++;
            if ((rows[idx].changed_mask & A2600VID_LC_PLAYFIELD) && rows[idx].row == 150) playfield++;
            if ((rows[idx].changed_mask & A2600VID_LC_POSITION) && rows[idx].row >= 171 && rows[idx].row <= 180) position++;
        }
        CHECK(colour == 1 && playfield == 1 && position == 10, "the Lines table finds the colour, playfield and position changes");
        CHECK(num > 0 && rows[0].row == 0 && rows[0].scanline == VIS_TOP && rows[1].row == 100, "...the first row, then only the changes");
        CHECK(a2600vid_tab_writes(tab, &writes) == 2 && !strcmp(writes[0].name, "COLUBK") && writes[0].value == 0x22 &&
              writes[0].old == 0x80 && writes[0].clock == 5 && writes[0].row == 100 && writes[0].scanline == 100 + VIS_TOP &&
              writes[1].row == -1 && writes[1].scanline == -1 && !strcmp(writes[1].name, "VBLANK") && a2600vid_tab_writes_dropped(tab) == 3,
              "the Writes table: register, values, clock, row, scanline");
    }
    {
        const a2600vid_swatch *swatch;
        long sum = 0;
        const int num = a2600vid_tab_swatches(tab, &swatch);
        for (int idx = 0; idx < num; idx++) sum += swatch[idx].uses;
        CHECK(num == A2600VID_O_COUNT && sum == (long)A2600VID_FB_W * FBH, "7 swatches whose uses add up to every pixel of every drawn row");
        CHECK(num == 7 && !strcmp(swatch[0].name, "BK") && !strcmp(swatch[0].reg_name, "COLUBK") && swatch[0].address == 9 &&
              swatch[0].rgb == console(swatch[0].value) && swatch[2].address == 6 && swatch[4].address == 6 &&
              !strcmp(swatch[4].reg_name, "COLUP0"), "swatch names, registers and colours");
    }
    {
        const a2600vid_reg *regs;
        const int num = a2600vid_tab_regs(tab, &regs);
        CHECK(num >= 20 && !strcmp(regs[0].name, "COLUP0") && !strcmp(regs[0].value, "1E") && strstr(regs[0].meaning, "hue 1"),
              "registers with meanings");
        {
            int chk_idx;
            for (chk_idx = 0; chk_idx < FBH; chk_idx++) cap.lines[chk_idx].colup0 = 0x2E;
        }
        a2600vid_tab_feed(tab, &cap, 0);
        a2600vid_tab_refresh(tab, 1);
        a2600vid_tab_regs(tab, &regs);
        CHECK(regs[0].age == 0 && regs[1].age > 0, "a changed register has age 0, others older");
        a2600vid_tab_refresh(tab, 1);
        a2600vid_tab_regs(tab, &regs);
        CHECK(regs[0].age == 1, "age counts refreshes");
    }
    {
        char buf[16384];
        for (int sec = 0; sec < A2600VID_S_COUNT; sec++)
            CHECK(a2600vid_tab_text(tab, sec, buf, sizeof buf) > 5, "every section has text");
        a2600vid_tab_text(tab, A2600VID_S_WRITES, buf, sizeof buf);
        CHECK(strstr(buf, "COLUBK") && strstr(buf, "outside the picture") && strstr(buf, "3 older"), "the Writes text");
    }
    CHECK(!strcmp(a2600vid_reg_name(0x1B), "GRP0") && !strcmp(a2600vid_reg_name(0x2E), "$2E") &&
          !strcmp(a2600vid_object_name(A2600VID_O_BL), "BL"), "names");
    a2600vid_tab_unpin(tab);

    /* ---- an empty frame ---- */
    {
        a2600vid_capture *empty = calloc(1, sizeof *empty);
        empty->serial = 1;
        a2600vid_tab_feed(tab, empty, 0);
        a2600vid_tab_set_view(tab, A2600VID_V_DISPLAY);
        a2600vid_tab_refresh(tab, 1);
        CHECK(a2600vid_tab_picture(tab, &wide, &high) == NULL && strlen(a2600vid_tab_why(tab)) > 5, "no frame yet says why");
        for (int view = 0; view < A2600VID_V_COUNT; view++) {
            a2600vid_tab_set_view(tab, view);
            a2600vid_tab_refresh(tab, 1);
            a2600vid_tab_hover(tab, 5, 5);
        }
        CHECK(1, "an empty capture does not crash any view");
        free(empty);
    }
    a2600vid_tab_free(tab);

    /* ---- fuzz ---- */
    tab = a2600vid_tab_new(NULL);
    srand(7);
    for (int iter = 0; iter < 150; iter++) {
        static const int heights[] = { 192, 262, 100, 320, 1, 0 };
        static const int zooms[] = { 0, 1, 2, 3, 4, 6, 8, 12, 16 };
        const int fbh = heights[rand() % 6];
        make_cap(&cap, fbh);
        cap.vis_top = rand() % 60;
        for (int row = 0; row < A2600VID_ROWS; row++) {
            a2600vid_line *line = &cap.lines[row];
            uint8_t *raw = (uint8_t *)line;
            for (size_t byte = 0; byte < sizeof *line; byte++) raw[byte] = (uint8_t)rand();
            line->valid = (uint8_t)(rand() % 8 != 0);
            for (int obj = 0; obj < A2600VID_NOBJ; obj++) line->pos[obj] = (uint8_t)(rand() % 200);
        }
        cap.npokes = rand() % 600;
        for (int num = 0; num < cap.npokes; num++) {
            cap.pokes[num].row = (int16_t)(rand() % 340 - 10);
            cap.pokes[num].col = (int16_t)(rand() % 240 - 70);
            cap.pokes[num].reg = (uint8_t)(rand() % 70);
            cap.pokes[num].value = (uint8_t)rand();
            cap.pokes[num].old = (uint8_t)rand();
        }
        for (int pix = 0; pix < A2600VID_FB_W * fbh; pix++) { cap.pixels[pix] = (uint32_t)rand(); cap.idx[pix] = (uint8_t)rand(); }
        cap.beam_row = rand() % (fbh + 1);
        cap.total_lines = rand() % 400;
        cap.hpos = rand() % 300; cap.vpos = rand() % 400;
        cap.serial = (uint64_t)(rand() % 3);
        cap.now.valid = (uint8_t)(rand() & 1);
        a2600vid_tab_set_view(tab, rand() % A2600VID_V_COUNT);
        a2600vid_tab_set_source(tab, rand() % A2600VID_D_COUNT);
        a2600vid_tab_set_overlays(tab, (unsigned)rand());
        a2600vid_tab_set_zoom(tab, zooms[rand() % 9]);
        a2600vid_tab_feed(tab, &cap, rand() & 1);
        a2600vid_tab_refresh(tab, 1);
        pic = a2600vid_tab_picture(tab, &wide, &high);
        for (int hop = 0; hop < 4 && pic; hop++) {
            a2600vid_tab_hover(tab, rand() % (wide + 4) - 2, rand() % (high + 4) - 2);
            if (hop == 1) a2600vid_tab_pin(tab, rand() % wide, rand() % high);
        }
        a2600vid_tab_refresh(tab, 0);
        {
            char buf[2048];
            for (int sec = 0; sec < A2600VID_S_COUNT; sec++) a2600vid_tab_text(tab, sec, buf, sizeof buf);
        }
        for (int row = -2; row < 4; row++) a2600vid_decode_row(&cap, row * 90, NULL, NULL, NULL);
    }
    CHECK(1, "fuzz: 150 random captures through every view survived");
    a2600vid_tab_free(tab);

    printf("%s\n", fails ? "FAILED" : "all ok");
    return fails ? 1 : 0;
}
