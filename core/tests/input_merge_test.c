/*
 * input_merge_test -- every way of holding a controller input, merged: the
 * keyboard, the Controllers window's pointer, a gamepad's buttons and its
 * stick are separate sources of a target, so letting go of one never releases
 * another. Opposed pairs cancel; a toggling switch flips on each source's
 * press; release_all lets go of the keyboard and the pointer but not a pad; a
 * pad that goes away recentres the analog position it moved. Then against a
 * running machine (the CONFIG client): what is held across a start is handed to
 * the new machine, the pointer's press and a wheel notch reach Stella and the
 * pulses let go by themselves.
 *
 * What is held is read through a2600session_buttons_held and _switch_held,
 * which report what the console is driven with; Stella-side accounting
 * (sent_acts) is read through session_internal.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "a2600session.h"
#include "input.h"
#include "session_internal.h"
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

#define BIT(act) (1u << (act))
#define FIRE0 A2600_TARGET_PORT(0, A2600_ACT_JOY_FIRE)
#define UP0 A2600_TARGET_PORT(0, A2600_ACT_JOY_UP)
#define DOWN0 A2600_TARGET_PORT(0, A2600_ACT_JOY_DOWN)
#define LEFT0 A2600_TARGET_PORT(0, A2600_ACT_JOY_LEFT)
#define RIGHT0 A2600_TARGET_PORT(0, A2600_ACT_JOY_RIGHT)

#define KB A2600_SRC_KEYBOARD
#define PTR A2600_SRC_POINTER
#define PAD(n) A2600_SRC_PAD(n)
#define STK(n) A2600_SRC_STICK(n)

static a2600session *g_s;

static a2600session *make(const char *tag)
{
    char cfg[512], data[512];
    a2600session_paths p;
    test_tmpdir(cfg, sizeof cfg, tag);
    snprintf(data, sizeof data, "%s-d", cfg);
    test_mkdir(data);
    memset(&p, 0, sizeof p);
    p.config_dir = cfg; p.data_dir = data; p.fujinet_lib = "";
    return a2600session_new(&p);
}

static unsigned held(int port) { return a2600session_buttons_held(g_s, port); }

/* ---- a stopped session: the merge itself ----------------------------------------- */

static void merge_rules(void)
{
    /* one target, several sources */
    input_press(g_s, KB, FIRE0, 1);
    input_press(g_s, PAD(0), FIRE0, 1);
    check(held(0) == BIT(A2600_ACT_JOY_FIRE), "fire held by the keyboard and a pad");
    input_press(g_s, KB, FIRE0, 0);
    check(held(0) == BIT(A2600_ACT_JOY_FIRE), "the key let go: the pad still holds it");
    input_press(g_s, PAD(0), FIRE0, 0);
    check(held(0) == 0, "and now nothing does");

    input_press(g_s, PAD(0), FIRE0, 1);
    input_press(g_s, PAD(1), FIRE0, 1);
    input_press(g_s, PAD(0), FIRE0, 0);
    check(held(0) == BIT(A2600_ACT_JOY_FIRE), "two pads: one lets go, the other still holds");
    input_press(g_s, PAD(1), FIRE0, 0);
    check(held(0) == 0, "both let go");

    input_press(g_s, PTR, FIRE0, 1);
    input_press(g_s, STK(0), FIRE0, 1);
    input_press(g_s, PTR, FIRE0, 0);
    check(held(0) == BIT(A2600_ACT_JOY_FIRE), "a pad's stick source and the pointer are separate too");
    input_press(g_s, STK(0), FIRE0, 0);
    check(held(0) == 0, "released");

    /* ports are independent */
    input_press(g_s, KB, A2600_TARGET_PORT(1, A2600_ACT_JOY_FIRE), 1);
    check(held(0) == 0 && held(1) == BIT(A2600_ACT_JOY_FIRE), "the right port's fire is not the left's");
    input_press(g_s, KB, A2600_TARGET_PORT(1, A2600_ACT_JOY_FIRE), 0);
}

static void opposed_pairs(void)
{
    static const struct { int a, b; const char *name; } k_pairs[] = {
        { A2600_ACT_JOY_UP, A2600_ACT_JOY_DOWN, "up and down" },
        { A2600_ACT_JOY_LEFT, A2600_ACT_JOY_RIGHT, "left and right" },
        { A2600_ACT_PADDLE_A_DEC, A2600_ACT_PADDLE_A_INC, "paddle A's two ways" },
        { A2600_ACT_PADDLE_B_DEC, A2600_ACT_PADDLE_B_INC, "paddle B's two ways" },
        { A2600_ACT_DRIVE_CCW, A2600_ACT_DRIVE_CW, "the wheel's two ways" },
    };
    char what[96];
    size_t i;

    for (i = 0; i < sizeof k_pairs / sizeof k_pairs[0]; i++) {
        const int ta = A2600_TARGET_PORT(0, k_pairs[i].a), tb = A2600_TARGET_PORT(0, k_pairs[i].b);
        input_press(g_s, KB, ta, 1);
        check(held(0) == BIT(k_pairs[i].a), "one way held");
        input_press(g_s, PAD(0), tb, 1);
        snprintf(what, sizeof what, "%s together: the console sees neither", k_pairs[i].name);
        check(held(0) == 0, what);
        input_press(g_s, PAD(0), tb, 0);
        snprintf(what, sizeof what, "%s: letting one go leaves the other", k_pairs[i].name);
        check(held(0) == BIT(k_pairs[i].a), what);
        input_press(g_s, KB, ta, 0);
    }
    input_press(g_s, KB, UP0, 1);
    input_press(g_s, KB, LEFT0, 1);
    check(held(0) == (BIT(A2600_ACT_JOY_UP) | BIT(A2600_ACT_JOY_LEFT)), "a diagonal is two bits, not an opposed pair");
    input_press(g_s, KB, UP0, 0);
    input_press(g_s, KB, LEFT0, 0);
    input_press(g_s, KB, A2600_TARGET_PORT(0, A2600_ACT_KEY_1), 1);
    input_press(g_s, KB, A2600_TARGET_PORT(0, A2600_ACT_KEY_2), 1);
    check(held(0) == (BIT(A2600_ACT_KEY_1) | BIT(A2600_ACT_KEY_2)), "keypad keys do not cancel each other");
    input_release_sources(g_s, KB);
    check(held(0) == 0, "released");
}

static void pointer_rules(void)
{
    a2600session_pointer_set(g_s, 0, BIT(A2600_ACT_JOY_UP) | BIT(A2600_ACT_JOY_LEFT));
    check(held(0) == (BIT(A2600_ACT_JOY_UP) | BIT(A2600_ACT_JOY_LEFT)), "the pointer holds a diagonal");
    a2600session_pointer_set(g_s, 0, BIT(A2600_ACT_JOY_RIGHT));
    check(held(0) == BIT(A2600_ACT_JOY_RIGHT), "a drag that changes direction replaces the whole hold");
    input_press(g_s, KB, UP0, 1);
    a2600session_pointer_set(g_s, 0, BIT(A2600_ACT_JOY_DOWN));
    check(held(0) == 0, "the keyboard's up and the pointer's down cancel");
    a2600session_pointer_set(g_s, 0, 0);
    check(held(0) == BIT(A2600_ACT_JOY_UP), "the drag ends: the keyboard's hold is what is left");
    input_press(g_s, KB, UP0, 0);
    check(held(0) == 0, "released");

    a2600session_press(g_s, FIRE0, 1);
    input_press(g_s, KB, FIRE0, 1);
    a2600session_press(g_s, FIRE0, 0);
    check(held(0) == BIT(A2600_ACT_JOY_FIRE), "a window button's release leaves a key's hold");
    input_press(g_s, KB, FIRE0, 0);
    check(held(0) == 0, "released");
}

static void switch_rules(void)
{
    const int select = A2600_TARGET_SWITCH(A2600_SW_SELECT);
    const int color = A2600_TARGET_SWITCH(A2600_SW_COLOR_BW);
    const int start = a2600session_switch_get(g_s, A2600_SW_COLOR_BW);

    check(!a2600session_switch_held(g_s, A2600_SW_SELECT), "Select is not held");
    input_press(g_s, PAD(0), select, 1);
    check(a2600session_switch_held(g_s, A2600_SW_SELECT), "a pad holds Select");
    input_press(g_s, KB, select, 1);
    input_press(g_s, PAD(0), select, 0);
    check(a2600session_switch_held(g_s, A2600_SW_SELECT), "the key still holds it");
    input_press(g_s, KB, select, 0);
    check(!a2600session_switch_held(g_s, A2600_SW_SELECT), "and now it is let go");

    input_press(g_s, KB, color, 1);
    check(a2600session_switch_get(g_s, A2600_SW_COLOR_BW) == !start, "a press flips Colour / B&W");
    input_press(g_s, KB, color, 1);
    check(a2600session_switch_get(g_s, A2600_SW_COLOR_BW) == !start, "a repeat of the same press does not flip it again");
    input_press(g_s, PAD(0), color, 1);
    check(a2600session_switch_get(g_s, A2600_SW_COLOR_BW) == start, "another source's press flips it back");
    input_press(g_s, KB, color, 0);
    input_press(g_s, PAD(0), color, 0);
    input_press(g_s, KB, color, 1);
    check(a2600session_switch_get(g_s, A2600_SW_COLOR_BW) == !start, "after the release the next press flips it");
    input_press(g_s, KB, color, 0);
    check(a2600session_switch_held(g_s, A2600_SW_COLOR_BW) == a2600session_switch_get(g_s, A2600_SW_COLOR_BW),
          "a toggling switch reports where it is");
    a2600session_switch_set(g_s, A2600_SW_COLOR_BW, start);
}

static void release_rules(void)
{
    input_press(g_s, KB, FIRE0, 1);
    input_press(g_s, PTR, UP0, 1);
    input_press(g_s, PAD(2), RIGHT0, 1);
    a2600session_release_all(g_s);
    check(held(0) == BIT(A2600_ACT_JOY_RIGHT), "release_all lets go of the keyboard and the pointer, not a pad");
    input_press(g_s, STK(1), A2600_TARGET_PORT(0, A2600_ACT_JOY_LEFT), 1);
    input_press(g_s, PAD(1), FIRE0, 1);
    input_pad_clear(g_s, 2);
    check(held(0) == (BIT(A2600_ACT_JOY_LEFT) | BIT(A2600_ACT_JOY_FIRE)), "pad_clear lets go of one pad only");
    input_pad_clear(g_s, 1);
    check(held(0) == 0, "and the other pad's, buttons and stick both");
}

static void analog_rules(void)
{
    a2600session_pointer_analog(g_s, 0, 0, 20000, 1);
    check(a2600session_analog_get(g_s, 0, 0) == 20000, "a drag on paddle A sets its position");
    input_analog(g_s, STK(0), 0, 0, -5000);
    check(a2600session_analog_get(g_s, 0, 0) == -5000, "a pad that moves its stick takes the position (last writer wins)");
    a2600session_pointer_analog(g_s, 0, 0, 12000, 1);
    check(a2600session_analog_get(g_s, 0, 0) == 12000, "and the pointer takes it back");
    a2600session_pointer_analog(g_s, 0, 0, 0, 0);
    check(a2600session_analog_get(g_s, 0, 0) == 12000, "a drag that ends leaves the paddle where it was");
    check(a2600session_analog_get(g_s, 1, 0) == 0 && a2600session_analog_get(g_s, 0, 1) == 0,
          "the other paddles are untouched");

    input_analog(g_s, STK(0), 0, 1, 8000);
    a2600session_pointer_analog(g_s, 0, -1, -7000, 1);
    check(a2600session_analog_get(g_s, 0, 1) == 8000 && a2600session_analog_get(g_s, 0, -1) == -7000,
          "paddle B and the driving wheel are separate positions");
    input_analog(g_s, STK(0), 0, 0, 40000);
    check(a2600session_analog_get(g_s, 0, 0) == 32767, "a position is clamped to 32767");
    input_analog(g_s, STK(0), 0, 0, -40000);
    check(a2600session_analog_get(g_s, 0, 0) == -32767, "and -32767");

    input_pad_clear(g_s, 0);
    check(a2600session_analog_get(g_s, 0, 0) == 0 && a2600session_analog_get(g_s, 0, 1) == 0,
          "a pad that goes away recentres what it moved last");
    check(a2600session_analog_get(g_s, 0, -1) == -7000, "but not what the pointer owns");
    a2600session_pointer_analog(g_s, 0, -1, 0, 1);
    a2600session_pointer_analog(g_s, 0, -1, 0, 0);
}

/* the keyboard's path, through the bindings table */
static void keyboard_path(void)
{
    char stolen[128];
    check(a2600session_key(g_s, A2600_KEYSYM_SPACE, 1) == 1, "Space is bound");
    check((held(0) & BIT(A2600_ACT_JOY_FIRE)) != 0, "and holds left fire");
    check(a2600session_key(g_s, A2600_KEYSYM_SPACE, 1) == 1 && (held(0) & BIT(A2600_ACT_JOY_FIRE)) != 0,
          "an auto-repeat changes nothing");
    /* rebound while held: the release lets go of what the press asserted */
    a2600session_binding_set_key(g_s, A2600_TARGET_PORT(1, A2600_ACT_KEY_1), A2600_KEYSYM_SPACE, stolen, sizeof stolen);
    check(a2600session_key(g_s, A2600_KEYSYM_SPACE, 0) == 1, "the release is seen");
    check(held(0) == 0 && held(1) == 0, "and it lets go of exactly what the press had asserted");
    a2600session_bindings_reset(g_s);

    /* a pad and the keyboard on the same fire: the key's release leaves the pad's hold */
    input_press(g_s, PAD(0), FIRE0, 1);
    a2600session_key(g_s, A2600_KEYSYM_SPACE, 1);
    a2600session_key(g_s, A2600_KEYSYM_SPACE, 0);
    check((held(0) & BIT(A2600_ACT_JOY_FIRE)) != 0, "the key lets go: the pad's fire is still held");
    input_press(g_s, PAD(0), FIRE0, 0);

    /* focus loss */
    a2600session_key(g_s, A2600_KEYSYM_SPACE, 1);
    a2600session_release_all(g_s);
    check(held(0) == 0, "focus loss lets go of a held key");
    check(a2600session_key(g_s, A2600_KEYSYM_SPACE, 1) == 1 && (held(0) & BIT(A2600_ACT_JOY_FIRE)) != 0,
          "and the same key presses again afterwards");
    a2600session_key(g_s, A2600_KEYSYM_SPACE, 0);
}

/* ---- a running machine -------------------------------------------------------------- */

static int wait_until(int (*pred)(void), int timeout_ms)
{
    int waited = 0;
    while (waited < timeout_ms) {
        if (pred()) return 1;
        sleep_ms(5);
        waited += 5;
    }
    return pred();
}

static int fire_sent(void) { return (g_s->sent_acts[0] & BIT(A2600_ACT_JOY_FIRE)) != 0; }
static int nothing_sent(void) { return g_s->sent_acts[0] == 0 && g_s->sent_acts[1] == 0; }
static int cw_not_held(void) { return (held(0) & BIT(A2600_ACT_DRIVE_CW)) == 0; }
static int cw_sent(void) { return (g_s->sent_acts[0] & BIT(A2600_ACT_DRIVE_CW)) != 0; }
static int select_released(void) { return !a2600session_switch_held(g_s, A2600_SW_SELECT); }

static void running_machine(void)
{
    a2600session_start_opts o;

    /* a key held BEFORE the machine exists */
    input_press(g_s, KB, FIRE0, 1);
    check(g_s->sent_acts[0] == 0, "nothing is sent while the machine is stopped");
    a2600session_default_opts(g_s, &o);
    o.enable_fujinet = 0; o.enable_audio = 0; o.enable_gamepad = 0;
    if (a2600session_start(g_s, &o) != 0) {
        printf("SKIP: the machine would not start: %s\n", a2600session_last_error(g_s));
        return;
    }
    check(fire_sent(), "a key held across the start is handed to the new machine");
    input_press(g_s, KB, FIRE0, 0);
    check(nothing_sent(), "and its release is sent");

    a2600session_press(g_s, FIRE0, 1);
    check(fire_sent(), "the window's press reaches the machine");
    a2600session_press(g_s, FIRE0, 0);
    check(nothing_sent(), "and its release");

    /* the opposed pair: neither is sent, and the one left is sent when the other lets go */
    input_press(g_s, KB, UP0, 1);
    input_press(g_s, PAD(0), DOWN0, 1);
    check((g_s->sent_acts[0] & (BIT(A2600_ACT_JOY_UP) | BIT(A2600_ACT_JOY_DOWN))) == 0, "up and down together: the machine is told neither");
    input_press(g_s, PAD(0), DOWN0, 0);
    check((g_s->sent_acts[0] & BIT(A2600_ACT_JOY_UP)) != 0, "the pad lets go: up is now sent");
    input_press(g_s, KB, UP0, 0);
    check(nothing_sent(), "released");

    /* the wheel's notch: held ~6 frames, then let go by the pulse thread */
    a2600session_twist_pulse(g_s, 0, 1);
    check((held(0) & BIT(A2600_ACT_DRIVE_CW)) != 0 && cw_sent(), "a wheel notch is held and sent");
    check(wait_until(cw_not_held, 5000), "and lets go by itself");
    check(wait_until(nothing_sent, 5000), "the machine is told");
    a2600session_twist_pulse(g_s, 0, 1);
    a2600session_twist_pulse(g_s, 0, -1);
    check((held(0) & BIT(A2600_ACT_DRIVE_CW)) == 0 && (held(0) & BIT(A2600_ACT_DRIVE_CCW)) != 0,
          "a notch the other way ends the one before it");
    check(wait_until(nothing_sent, 5000), "and that lets go too");

    a2600session_switch_pulse(g_s, A2600_SW_SELECT);
    check(a2600session_switch_held(g_s, A2600_SW_SELECT), "the menu's Select pulse is held");
    check(wait_until(select_released, 5000), "and lets go by itself");

    /* a restart: what is held at the stop is handed to the new machine */
    input_press(g_s, STK(0), FIRE0, 1);
    a2600session_stop(g_s);
    check(!a2600session_is_running(g_s), "stopped");
    input_press(g_s, KB, UP0, 1);
    a2600session_default_opts(g_s, &o);
    o.enable_fujinet = 0; o.enable_audio = 0; o.enable_gamepad = 0;
    check(a2600session_start(g_s, &o) == 0, "restarted");
    check(fire_sent() && (g_s->sent_acts[0] & BIT(A2600_ACT_JOY_UP)) != 0,
          "after a restart the new machine is handed everything still held");
    input_press(g_s, STK(0), FIRE0, 0);
    input_press(g_s, KB, UP0, 0);
    check(nothing_sent(), "released");

    /* an analog position survives a restart too */
    a2600session_pointer_analog(g_s, 1, 0, 9000, 1);
    a2600session_stop(g_s);
    a2600session_default_opts(g_s, &o);
    o.enable_fujinet = 0; o.enable_audio = 0; o.enable_gamepad = 0;
    check(a2600session_start(g_s, &o) == 0 && a2600session_analog_get(g_s, 1, 0) == 9000,
          "a paddle keeps its position across a restart");
    a2600session_pointer_analog(g_s, 1, 0, 0, 1);
    a2600session_pointer_analog(g_s, 1, 0, 0, 0);
    a2600session_stop(g_s);
}

int main(void)
{
    g_s = make("imerge");
    if (!g_s) return 1;

    merge_rules();
    opposed_pairs();
    pointer_rules();
    switch_rules();
    release_rules();
    analog_rules();
    keyboard_path();
    running_machine();

    a2600session_free(g_s);
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
