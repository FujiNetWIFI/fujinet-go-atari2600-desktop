/*
 * vidtab_live_test -- the Video tab against the real machine: FujiNet's
 * CONFIG client (real TIA registers, real writes, no ROMs). Running: the last
 * finished frame, the tables and the inspector. Stopped: Display is exactly
 * the TIA's finished frame (what a2600debug_frame_snapshot draws), and the
 * Beam view, the frame redrawn from the registers and the writes the host
 * recorded, matches it (reported as a percentage of pixels: the replay does
 * not model Stella's few-clock write delays). Write traps from a swatch.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "a2600debug.h"
#include "a2600session.h"
#include "a2600vidcap.h"
#include "a2600vidtab.h"
#include "test_tmpdir.h"

static int failures;
static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) failures++;
}

static void sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static int wait_stopped(a2600debug *dbg, int want, int timeout_ms)
{
    for (int waited = 0; waited < timeout_ms; waited += 5) {
        if (!!a2600debug_is_stopped(dbg) == want) return 1;
        sleep_ms(5);
    }
    return 0;
}

static uint32_t snapshot[A2600SESSION_FB_WIDTH * A2600SESSION_FB_MAX_HEIGHT];
static a2600vid_capture running_cap, stopped_cap;

/* the share of pixels the replay got right in rows [0, rows) of a capture */
static double replay_match(const a2600vid_capture *cap, int rows, int *tested)
{
    long same = 0, total = 0;
    uint8_t colour[A2600VID_FB_W];
    for (int row = 0; row < rows; row++) {
        if (!a2600vid_decode_row(cap, row, colour, NULL, NULL)) continue;
        for (int clock = 0; clock < A2600VID_FB_W; clock++) {
            same += (colour[clock] & 0xFE) == (cap->idx[(size_t)row * A2600VID_FB_W + (size_t)clock] & 0xFE);
            total++;
        }
    }
    if (tested) *tested = (int)total;
    return total ? (double)same / (double)total : 0.0;
}

int main(void)
{
    char cfg[512], data[512], out[1024];
    a2600session_paths paths;
    a2600session *sess;
    a2600session_start_opts opts;
    a2600debug *dbg;
    a2600vid_tab *tab;
    const uint32_t *pic;
    int wide, high;

    test_tmpdir(cfg, sizeof cfg, "vcfg");
    test_tmpdir(data, sizeof data, "vdata");
    memset(&paths, 0, sizeof paths);
    paths.config_dir = cfg; paths.data_dir = data; paths.fujinet_lib = "";
    sess = a2600session_new(&paths);
    if (!sess) return 1;
    a2600session_default_opts(sess, &opts);
    opts.enable_fujinet = 0; opts.enable_audio = 0; opts.enable_gamepad = 0;
    check(a2600session_start(sess, &opts) == 0, "session starts (CONFIG)");
    dbg = a2600session_debugger(sess);
    tab = a2600vid_tab_new(sess);
    check(tab != NULL, "tab");
    {
        const uint64_t start = a2600session_frame_count(sess);
        for (int waited = 0; waited < 8000 && a2600session_frame_count(sess) < start + 240; waited += 10) sleep_ms(10);
    }

    /* running */
    a2600vid_tab_set_zoom(tab, 1);
    a2600vid_tab_set_overlays(tab, 0);
    check((a2600vid_tab_refresh(tab, 1) & A2600VID_TAB_PICTURE) != 0, "refresh while running");
    pic = a2600vid_tab_picture(tab, &wide, &high);
    check(pic && wide == 320 && high >= 150, "Display (running) is 320 wide");
    {
        uint64_t serial = 0;
        int valid = 0, drawn = 0;
        check(a2600vid_capture_get(sess, &running_cap, &serial) == 1 && serial != 0, "the host captured a finished frame");
        check(a2600vid_capture_get(sess, &running_cap, &serial) == 0, "...and a second get with the same serial copies nothing");
        for (int row = 0; row < running_cap.fb_h; row++) valid += running_cap.lines[row].valid;
        printf("  %d x %d, %d of %d rows recorded, %d TIA writes, top scanline %d, %d lines\n", running_cap.fb_w, running_cap.fb_h,
               valid, running_cap.fb_h, running_cap.npokes, running_cap.vis_top, running_cap.total_lines);
        check(valid == running_cap.fb_h && running_cap.npokes > 20, "every row recorded its registers, and the frame's writes");
        for (int pix = 0; pix < running_cap.fb_w * running_cap.fb_h; pix++) drawn += running_cap.idx[pix] != 0;
        check(drawn > 100, "the CONFIG frame has colours");
    }
    {
        const a2600vid_line_row *rows;
        const a2600vid_reg *regs;
        const a2600vid_write *writes;
        char buf[4096];
        check(a2600vid_tab_lines(tab, &rows) > 0, "the Lines table has rows");
        check(a2600vid_tab_regs(tab, &regs) >= 20, "registers");
        check(a2600vid_tab_writes(tab, &writes) > 20, "writes");
        check(a2600vid_tab_text(tab, A2600VID_S_TIMING, buf, sizeof buf) > 20 && strstr(buf, "Standard"), "timing text");
    }
    {
        double share;
        int total = 0;
        a2600vid_tab_refresh(tab, 1);
        share = replay_match(&running_cap, running_cap.fb_h, &total);
        printf("  the replay matches %.2f%% of %d pixels of the running capture\n", share * 100.0, total);
        check(share > 0.90, "the priority-encoder replay matches the finished frame (>90%)");
    }
    a2600vid_tab_set_view(tab, A2600VID_V_BEAM);
    a2600vid_tab_refresh(tab, 1);
    check(a2600vid_tab_picture(tab, &wide, &high) == NULL && strlen(a2600vid_tab_why(tab)) > 10, "Beam while running says why");
    a2600vid_tab_set_view(tab, A2600VID_V_DISPLAY);

    /* stopped */
    a2600debug_stop(dbg);
    check(wait_stopped(dbg, 1, 3000), "stopped");
    a2600debug_frame(dbg, 1);
    check(wait_stopped(dbg, 1, 3000), "stopped after a frame");
    sleep_ms(50);
    {
        int diff = 0, hh;
        a2600vid_tab_refresh(tab, 1);
        pic = a2600vid_tab_picture(tab, &wide, &high);
        hh = a2600debug_frame_snapshot(dbg, snapshot, 0);
        for (int row = 0; pic && row < high; row++)
            for (int clock = 0; clock < A2600VID_FB_W; clock++)
                for (int sub = 0; sub < 2; sub++)
                    diff += pic[row * wide + clock * 2 + sub] != (snapshot[row * A2600SESSION_FB_WIDTH + clock] | 0xff000000u);
        printf("  Display %dx%d, frame snapshot %d rows: %d pixels differ\n", wide, high, hh, diff);
        check(pic && wide == 320 && high == hh && diff == 0, "Display at a stop is exactly the TIA's finished frame");
    }
    /* stopped mid-frame: rows above the beam are this frame's */
    for (int step = 0; step < 6; step++) {
        a2600debug_scanline(dbg, 40);
        wait_stopped(dbg, 1, 3000);
        sleep_ms(30);
        if (a2600vid_capture_fill(sess, &stopped_cap) == 1 && stopped_cap.beam_row > 80 && stopped_cap.beam_row < stopped_cap.fb_h - 20) break;
    }
    check(a2600vid_capture_fill(sess, &stopped_cap) == 1 && stopped_cap.now.valid && stopped_cap.fb_h > 100, "a live fill has the registers right now");
    printf("  beam: row %d of %d, scanline %d, clock %d, frame %llu, %d writes\n", stopped_cap.beam_row, stopped_cap.fb_h,
           stopped_cap.vpos, stopped_cap.hpos, (unsigned long long)stopped_cap.frame, stopped_cap.npokes);
    check(stopped_cap.beam_row > 80 && stopped_cap.beam_row < stopped_cap.fb_h, "the beam is inside the picture");
    {
        double share;
        int total = 0, top_ok = 1;
        a2600vid_tab_set_view(tab, A2600VID_V_BEAM);
        a2600vid_tab_refresh(tab, 1);
        pic = a2600vid_tab_picture(tab, &wide, &high);
        check(pic != NULL && wide == 320, "the Beam view draws at a stop");
        share = replay_match(&stopped_cap, stopped_cap.beam_row - 1, &total);
        printf("  rows above the beam (%d): the replay matches %.2f%% of %d pixels\n", stopped_cap.beam_row, share * 100.0, total);
        check(total > 5000 && share > 0.90, "rows above the beam replay like the TIA's own picture");
        for (int row = stopped_cap.beam_row + 1; pic && row < high; row += 17)
            top_ok = top_ok && pic[row * wide + 3] == pic[row * wide + 2];
        check(top_ok, "below the beam it is the last frame");
    }

    /* the inspector and write traps */
    a2600vid_tab_set_view(tab, A2600VID_V_DISPLAY);
    a2600vid_tab_refresh(tab, 1);
    {
        int found = 0;
        const a2600vid_pixel *pix = NULL;
        for (int row = 0; row < high && !found; row += 2)
            for (int col = 0; col < wide && !found; col += 4) {
                a2600vid_tab_hover(tab, col, row);
                pix = a2600vid_tab_pixel(tab);
                if (pix && pix->active && pix->object != A2600VID_O_BK) found = 1;
            }
        check(found, "an object other than the background drew some pixel");
        if (found) {
            printf("%s\n", pix->text);
            check(strstr(pix->text, "Drawn by") && strstr(pix->text, "Row registers"), "the inspector's text");
            check(a2600vid_tab_break_on_pixel(tab) == 0, "break on pixel: write traps on its object's registers");
        }
    }
    a2600debug_command(dbg, "clearTraps", out, sizeof out);
    check(a2600vid_tab_break_on_swatch(tab, A2600VID_O_BK) == 0, "break on swatch");
    a2600debug_command(dbg, "clearTraps", out, sizeof out);
    check(a2600vid_tab_break_on_register(tab, 2) == 0 && a2600vid_tab_break_on_register(tab, 2) == 0,
          "break on WSYNC (twice: still one trap)");
    {
        char why[256];
        int addr = -1;
        a2600debug_resume(dbg);
        wait_stopped(dbg, 0, 1000);
        check(wait_stopped(dbg, 1, 4000), "the machine stops on the WSYNC write trap");
        a2600debug_stop_reason(dbg, why, sizeof why, &addr);
        printf("  stop reason: %s (address %d)\n", why, addr);
        check(addr == 2 || strstr(why, "WSYNC") != NULL, "...at WSYNC, so the second call did not toggle it off");
        a2600debug_command(dbg, "clearTraps", out, sizeof out);
    }

    a2600vid_tab_free(tab);
    a2600debug_resume(dbg);
    a2600session_stop(sess);
    a2600session_free(sess);
    printf("%s\n", failures ? "FAILED" : "all ok");
    return failures ? 1 : 0;
}
