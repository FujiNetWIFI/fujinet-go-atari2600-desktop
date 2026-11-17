/*
 * paddle_hold_test -- an absolute paddle position set through the analog API
 * must STAY where it was put.
 *
 * Stella keeps an analog event "stored indefinitely" but acts on it only when
 * it differs from the previous frame's value by more than 10, then drops the
 * pot back to the digital position. So a knob set once, or a stick held still,
 * would show for ONE frame and then spring back to centre: the old gamepad
 * path hid this because it pushed the raw stick every poll (its noise kept the
 * value changing); the new one writes only on change. The host therefore owns a
 * nonzero position and re-sends it every frame (StellaHost::setAnalogHold).
 *
 * This runs a 6502 routine on the machine that reads paddle A the way a game
 * does (dump the capacitor, count scan-line pairs until INPT0 bit 7 sets) and
 * checks the count over time: a knob at -20000 and one at +20000 read clearly
 * different from centre, and read the same again 600 ms later with nothing
 * re-sent; setting 0 hands the paddle back to the digital position.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "a2600debug.h"
#include "a2600session.h"
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

static a2600session *g_s;
static a2600debug *g_d;

/* Loop on the machine: dump the pot, then count WSYNC pairs until INPT0 sets;
 * the last count is kept in $F0 (RAM $80-$FF is the 2600's only RAM). */
static void install_reader(void)
{
    static const uint8_t code[] = {
        0xA2, 0x00,             /* 80: LDX #0 */
        0xA9, 0x80,             /* 82: LDA #$80 */
        0x85, 0x01,             /* 84: STA VBLANK (dump) */
        0x85, 0x02,             /* 86: STA WSYNC */
        0x85, 0x02,             /* 88: STA WSYNC */
        0x85, 0x02,             /* 8A: STA WSYNC */
        0xA9, 0x00,             /* 8C: LDA #0 */
        0x85, 0x01,             /* 8E: STA VBLANK (release) */
        0x85, 0x02,             /* 90: STA WSYNC */
        0x85, 0x02,             /* 92: STA WSYNC */
        0xE8,                   /* 94: INX */
        0x24, 0x08,             /* 95: BIT INPT0 */
        0x10, 0xF7,             /* 97: BPL 90 */
        0x86, 0xF0,             /* 99: STX $F0 */
        0x4C, 0x80, 0x00,       /* 9B: JMP 80 */
    };
    size_t i;

    a2600debug_stop(g_d);
    sleep_ms(60);
    for (i = 0; i < sizeof code; i++)
        a2600debug_write(g_d, (uint16_t)(0x80 + i), code[i]);
    a2600debug_write(g_d, 0xF0, 0xFF);
    a2600debug_cpu_set(g_d, A2600_REG_PC, 0x0080);
    a2600debug_resume(g_d);
}

static int count_now(void)
{
    uint8_t ram[128];

    a2600debug_stop(g_d);
    sleep_ms(30);
    a2600debug_ram_get(g_d, ram);
    a2600debug_resume(g_d);
    return ram[0xF0 - 0x80];
}

/* the count after the position has had time to take effect */
static int settled(void)
{
    sleep_ms(250);
    return count_now();
}

int main(void)
{
    char cfg[512], data[512];
    a2600session_paths paths;
    a2600session_start_opts opts;
    int centre, left, right, left_later, right_later, back;

    test_tmpdir(cfg, sizeof cfg, "phcfg");
    test_tmpdir(data, sizeof data, "phdata");
    memset(&paths, 0, sizeof paths);
    paths.config_dir = cfg;
    paths.data_dir = data;
    paths.fujinet_lib = "";
    g_s = a2600session_new(&paths);
    if (!g_s) {
        fprintf(stderr, "paddle_hold_test: cannot create a session\n");
        return 77;
    }
    a2600session_default_opts(g_s, &opts);
    opts.enable_fujinet = 0;
    opts.enable_audio = 0;
    opts.enable_gamepad = 0;
    if (a2600session_start(g_s, &opts) != 0) {
        fprintf(stderr, "paddle_hold_test: %s\n", a2600session_last_error(g_s));
        a2600session_free(g_s);
        return 77;
    }
    g_d = a2600session_debugger(g_s);
    a2600session_set_port_type(g_s, 0, A2600_CTRL_PADDLES);
    sleep_ms(400);
    install_reader();
    sleep_ms(300);

    centre = settled();
    check(centre > 0 && centre < 255, "the digital (centre) position reads a plausible count");

    /* set once, never re-sent */
    a2600session_pointer_analog(g_s, 0, 0, -20000, 1);
    left = settled();
    a2600session_pointer_analog(g_s, 0, 0, -20000, 0);   /* the drag ends: the position stays */
    sleep_ms(600);
    left_later = count_now();
    check(left > centre + 10, "knob at -20000 reads clearly away from centre");
    check(left_later == left, "the position is still there 600 ms later with nothing re-sent");

    a2600session_pointer_analog(g_s, 0, 0, 20000, 1);
    right = settled();
    sleep_ms(600);
    right_later = count_now();
    check(right < centre - 10, "knob at +20000 reads clearly the other way");
    check(right_later == right, "and it holds too");
    check(left > right, "the two positions are in opposite order");

    /* a still pad stick: sent once through the input layer, then nothing */
    a2600session_pointer_analog(g_s, 0, 0, 0, 1);
    back = settled();
    check(back == centre, "0 hands the paddle back to the digital position");
    sleep_ms(600);
    check(count_now() == centre, "and it stays there");

    a2600session_free(g_s);
    printf("paddle_hold: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
