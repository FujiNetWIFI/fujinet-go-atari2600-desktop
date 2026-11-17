/*
 * controls_test -- the on-screen controllers' layout, pure: every combination
 * of controller types has a layout of the same extent (the window never
 * resizes when a game swaps a controller), every part is inside it and drives
 * a target of its own port, each controller type has the parts it should, the
 * hit test finds the topmost pressable part and treats round parts as ellipses,
 * and the joystick, knob and wheel drag maths.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "a2600session.h"
#include "test_tmpdir.h"

static int failures;
static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) failures++;
}

static int count_kind(const a2600_control *c, int n, int kind, int port)
{
    int i, num = 0;
    for (i = 0; i < n; i++)
        if (c[i].kind == kind && c[i].port == port) num++;
    return num;
}

static int has_target(const a2600_control *c, int n, int target)
{
    int i;
    for (i = 0; i < n; i++)
        if (c[i].target == target) return 1;
    return 0;
}

static const a2600_control *find(const a2600_control *c, int n, int kind, int port, int paddle)
{
    int i;
    for (i = 0; i < n; i++)
        if (c[i].kind == kind && c[i].port == port && c[i].paddle == paddle) return &c[i];
    return NULL;
}

static void every_combination(void)
{
    int left, right, ok_extent = 1, ok_inside = 1, ok_targets = 1, ok_unique = 1, ok_count = 1;
    float w0 = 0, h0 = 0;

    a2600session_controls_layout(A2600_CTRL_JOYSTICK, A2600_CTRL_JOYSTICK, NULL, &w0, &h0);
    for (left = 0; left < A2600_CTRL_COUNT; left++)
        for (right = 0; right < A2600_CTRL_COUNT; right++) {
            int n, i, j;
            float w, h;
            const a2600_control *c = a2600session_controls_layout(left, right, &n, &w, &h);
            if (!c || n <= 0 || n > 48) ok_count = 0;
            if (w != w0 || h != h0) ok_extent = 0;
            for (i = 0; i < n; i++) {
                if (c[i].x < 0 || c[i].y < 0 || c[i].x + c[i].w > w + 0.001f || c[i].y + c[i].h > h + 0.001f)
                    ok_inside = 0;
                if (c[i].target >= 0) {
                    if (c[i].target >= A2600_TARGET_COUNT) ok_targets = 0;
                    /* a controller part drives a target of its own port */
                    if (c[i].port >= 0 && c[i].target < 2 * A2600_ACT_PER_PORT
                        && c[i].target / A2600_ACT_PER_PORT != c[i].port)
                        ok_targets = 0;
                    /* a console part drives a switch or a session action */
                    if (c[i].port < 0 && c[i].target < 2 * A2600_ACT_PER_PORT) ok_targets = 0;
                    for (j = i + 1; j < n; j++)
                        if (c[j].target == c[i].target) ok_unique = 0;
                }
            }
        }
    check(ok_count, "every combination of types has a layout");
    check(ok_extent, "all of them have the same extent (the window never resizes)");
    check(ok_inside, "every part is inside it");
    check(ok_targets, "every part drives a target of its own port (or the console's)");
    check(ok_unique, "no target has two parts in one view");
    check(w0 > 10.0f && h0 > 10.0f, "and the extent is sane");
    {
        int n1, n2;
        const a2600_control *a = a2600session_controls_layout(A2600_CTRL_AUTO, 99, &n1, NULL, NULL);
        const a2600_control *b = a2600session_controls_layout(A2600_CTRL_JOYSTICK, A2600_CTRL_JOYSTICK, &n2, NULL, NULL);
        check(a == b && n1 == n2, "AUTO and an unknown type are drawn as a joystick");
    }
}

static void views(void)
{
    int n, port, key;
    const a2600_control *c;

    c = a2600session_controls_layout(A2600_CTRL_JOYSTICK, A2600_CTRL_PADDLES, &n, NULL, NULL);
    check(find(c, n, A2600_CTL_STICK, 0, 0) != NULL, "a joystick view has the stick");
    check(has_target(c, n, A2600_TARGET_PORT(0, A2600_ACT_JOY_FIRE)), "and the fire button");
    check(!has_target(c, n, A2600_TARGET_PORT(0, A2600_ACT_KEY_1)), "and no keypad");
    check(count_kind(c, n, A2600_CTL_KNOB, 1) == 2, "the paddle view has two knobs");
    check(find(c, n, A2600_CTL_KNOB, 1, 0) && find(c, n, A2600_CTL_KNOB, 1, 1), "for paddles 0 and 1");
    check(has_target(c, n, A2600_TARGET_PORT(1, A2600_ACT_PADDLE_A_FIRE))
          && has_target(c, n, A2600_TARGET_PORT(1, A2600_ACT_PADDLE_B_FIRE)), "each with its fire button");
    check(has_target(c, n, A2600_TARGET_PORT(1, A2600_ACT_PADDLE_A_DEC))
          && has_target(c, n, A2600_TARGET_PORT(1, A2600_ACT_PADDLE_A_INC))
          && has_target(c, n, A2600_TARGET_PORT(1, A2600_ACT_PADDLE_B_DEC))
          && has_target(c, n, A2600_TARGET_PORT(1, A2600_ACT_PADDLE_B_INC)), "and its two turn keys");

    c = a2600session_controls_layout(A2600_CTRL_DRIVING, A2600_CTRL_KEYPAD, &n, NULL, NULL);
    check(find(c, n, A2600_CTL_WHEEL, 0, -1) != NULL, "a driving view has the wheel (paddle -1)");
    check(has_target(c, n, A2600_TARGET_PORT(0, A2600_ACT_DRIVE_CCW))
          && has_target(c, n, A2600_TARGET_PORT(0, A2600_ACT_DRIVE_CW))
          && has_target(c, n, A2600_TARGET_PORT(0, A2600_ACT_DRIVE_FIRE)), "its two ways and the fire button");
    for (key = 0; key < 12; key++)
        if (!has_target(c, n, A2600_TARGET_PORT(1, A2600_ACT_KEY_1 + key))) break;
    check(key == 12, "the keypad view has all twelve keys");
    {
        /* 1 2 3 / 4 5 6 / 7 8 9 / * 0 # in reading order */
        const a2600_control *k1 = NULL, *k2 = NULL, *k4 = NULL, *kstar = NULL;
        int i;
        for (i = 0; i < n; i++) {
            if (c[i].target == A2600_TARGET_PORT(1, A2600_ACT_KEY_1)) k1 = &c[i];
            if (c[i].target == A2600_TARGET_PORT(1, A2600_ACT_KEY_2)) k2 = &c[i];
            if (c[i].target == A2600_TARGET_PORT(1, A2600_ACT_KEY_4)) k4 = &c[i];
            if (c[i].target == A2600_TARGET_PORT(1, A2600_ACT_KEY_STAR)) kstar = &c[i];
        }
        check(k1 && k2 && k4 && kstar && k2->x > k1->x && k4->y > k1->y && kstar->y > k4->y && kstar->x == k1->x,
              "in the keypad's reading order");
        check(k1 && !strcmp(k1->label, "1") && kstar && !strcmp(kstar->label, "*"), "labelled");
    }

    c = a2600session_controls_layout(A2600_CTRL_JOYSTICK, A2600_CTRL_JOYSTICK, &n, NULL, NULL);
    check(has_target(c, n, A2600_TARGET_SWITCH(A2600_SW_SELECT)) && has_target(c, n, A2600_TARGET_SWITCH(A2600_SW_RESET)),
          "the console has Select and Reset");
    check(has_target(c, n, A2600_TARGET_SWITCH(A2600_SW_COLOR_BW)) && has_target(c, n, A2600_TARGET_SWITCH(A2600_SW_LEFT_DIFF))
          && has_target(c, n, A2600_TARGET_SWITCH(A2600_SW_RIGHT_DIFF)), "and the three position switches");
    check(has_target(c, n, A2600_TARGET_SYSACT(A2600_SYSACT_REBOOT_CONFIG)) && has_target(c, n, A2600_TARGET_SYSACT(A2600_SYSACT_PAUSE)),
          "and the two session actions");
    {
        const a2600_control *sw = NULL;
        int i;
        for (i = 0; i < n; i++)
            if (c[i].target == A2600_TARGET_SWITCH(A2600_SW_COLOR_BW)) sw = &c[i];
        check(sw && sw->kind == A2600_CTL_SWITCH && sw->legend && *sw->legend, "a latching switch is a SWITCH and carries its printed legend");
    }
    for (port = 0; port < 2; port++)
        check(count_kind(c, n, A2600_CTL_CAPTION, port) == 1, "each port has a caption");
}

static const a2600_control *body(const a2600_control *c, int n, const char *name, int port)
{
    int i;
    for (i = 0; i < n; i++)
        if (c[i].kind == A2600_CTL_BODY && c[i].port == port && !strcmp(c[i].label, name)) return &c[i];
    return NULL;
}

static int near(float a, float b)
{
    return fabsf(a - b) < 0.011f;
}

/* the geometry is DESIGN-atari.md's, in its own units */
static void design(void)
{
    int n, i;
    const a2600_control *c, *bd, *part;

    c = a2600session_controls_layout(A2600_CTRL_JOYSTICK, A2600_CTRL_PADDLES, &n, NULL, NULL);
    bd = body(c, n, "Joystick", 0);
    check(bd && near(bd->w, 8.0f) && near(bd->h, 8.0f), "the CX40's case is 8.0 x 8.0");
    part = find(c, n, A2600_CTL_STICK, 0, 0);
    check(bd && part && near(part->x + part->w * 0.5f - bd->x, 4.5f) && near(part->y + part->h * 0.5f - bd->y, 4.5f)
          && near(part->w, 4.2f), "its stick region is centred at (4.5, 4.5) with r 2.1");
    for (i = 0; i < n; i++)
        if (c[i].target == A2600_TARGET_PORT(0, A2600_ACT_JOY_FIRE)) part = &c[i];
    check(bd && part->kind == A2600_CTL_ROUND_BUTTON && near(part->x + part->w * 0.5f - bd->x, 1.7f)
          && near(part->y + part->h * 0.5f - bd->y, 1.7f) && near(part->w, 1.9f),
          "its fire button is a round button at (1.7, 1.7), r 0.95");

    bd = body(c, n, "Paddle A", 1);
    part = NULL;
    for (i = 0; i < n; i++)
        if (c[i].kind == A2600_CTL_KNOB && c[i].port == 1 && c[i].paddle == 0) part = &c[i];
    check(bd && part && near(bd->w, 7.0f) && near(bd->h, 6.0f) && near(part->x + part->w * 0.5f - bd->x, 3.2f)
          && near(part->y + part->h * 0.5f - bd->y, 2.9f) && near(part->w, 4.4f),
          "a CX30 paddle is 7.0 x 6.0 with its knob at (3.2, 2.9), r 2.2");
    {
        const a2600_control *bdb = body(c, n, "Paddle B", 1);
        check(bd && bdb && near(bdb->x - bd->x, 7.8f) && near(bdb->y, bd->y), "the pair has a 0.8 gap");
        for (i = 0; i < n; i++)
            if (c[i].target == A2600_TARGET_PORT(1, A2600_ACT_PADDLE_A_FIRE)) part = &c[i];
        check(bd && part->kind == A2600_CTL_BUTTON && near(part->x, bd->x) && near(part->w, 0.9f) && near(part->y - bd->y, 2.2f),
              "the red button is on the left edge, x 0.0 to 0.9, y 2.2 to 3.8");
    }

    c = a2600session_controls_layout(A2600_CTRL_KEYPAD, A2600_CTRL_DRIVING, &n, NULL, NULL);
    bd = body(c, n, "Keypad", 0);
    check(bd && near(bd->w, 5.6f) && near(bd->h, 8.0f), "the CX50 is 5.6 x 8.0");
    {
        const a2600_control *k1 = NULL, *k6 = NULL, *kp = NULL;
        for (i = 0; i < n; i++) {
            if (c[i].target == A2600_TARGET_PORT(0, A2600_ACT_KEY_1)) k1 = &c[i];
            if (c[i].target == A2600_TARGET_PORT(0, A2600_ACT_KEY_6)) k6 = &c[i];
            if (c[i].target == A2600_TARGET_PORT(0, A2600_ACT_KEY_POUND)) kp = &c[i];
        }
        check(bd && k1 && near(k1->x + k1->w * 0.5f - bd->x, 1.45f) && near(k1->y + k1->h * 0.5f - bd->y, 3.1f)
              && k1->kind == A2600_CTL_ROUND_BUTTON && near(k1->w, 1.0f), "its first key is a round key centred at (1.45, 3.1)");
        check(k6 && k1 && near(k6->x - k1->x, 2.7f) && near(k6->y - k1->y, 1.2f), "with pitch 1.35 across and 1.2 down");
        check(kp && k1 && near(kp->x - k1->x, 2.7f) && near(kp->y - k1->y, 3.6f), "ending at the # key");
    }
    check(find(c, n, A2600_CTL_WHEEL, 1, -1) != NULL && body(c, n, "Driving", 1) != NULL, "the driving controller has the paddle's body");

    bd = body(c, n, "Console", -1);
    check(bd && near(bd->w, 24.0f) && near(bd->h, 5.2f), "the console panel is 24.0 x 5.2");
    {
        const a2600_control *wood = body(c, n, "Wood", -1), *slot = body(c, n, "Cartridge slot", -1);
        check(wood && near(wood->h, 1.6f) && near(wood->y, bd->y + bd->h), "over a 1.6 wood strip");
        check(slot && near(slot->x - bd->x, 8.6f) && near(slot->w, 6.8f), "with the cartridge slot at x 8.6 to 15.4");
    }
    {
        static const struct { int target; float cx; } k_sw[] = {
            { A2600_TARGET_SYSACT(A2600_SYSACT_REBOOT_CONFIG), 2.0f }, { A2600_TARGET_SWITCH(A2600_SW_COLOR_BW), 4.4f },
            { A2600_TARGET_SWITCH(A2600_SW_LEFT_DIFF), 6.6f }, { A2600_TARGET_SWITCH(A2600_SW_RIGHT_DIFF), 17.2f },
            { A2600_TARGET_SWITCH(A2600_SW_SELECT), 19.9f }, { A2600_TARGET_SWITCH(A2600_SW_RESET), 22.4f },
        };
        int ok = 1;
        size_t k;
        for (k = 0; k < sizeof k_sw / sizeof k_sw[0]; k++) {
            part = NULL;
            for (i = 0; i < n; i++)
                if (c[i].target == k_sw[k].target) part = &c[i];
            if (!part || !near(part->x + part->w * 0.5f - bd->x, k_sw[k].cx)) ok = 0;
        }
        check(ok, "the six switches sit at x = 2.0, 4.4, 6.6, 17.2, 19.9 and 22.4");
    }
}

static void centre(const a2600_control *c, float *x, float *y)
{
    *x = c->x + c->w * 0.5f;
    *y = c->y + c->h * 0.5f;
}

static void hits(void)
{
    int n, i, ok_centre = 1, ok_none = 1;
    float x, y, w, h;
    const a2600_control *c;
    int lt, rt;

    for (lt = 1; lt < A2600_CTRL_COUNT; lt++)
        for (rt = 1; rt < A2600_CTRL_COUNT; rt++) {
            c = a2600session_controls_layout(lt, rt, &n, &w, &h);
            for (i = 0; i < n; i++) {
                int hit;
                if (c[i].kind == A2600_CTL_BODY || c[i].kind == A2600_CTL_CAPTION) continue;
                centre(&c[i], &x, &y);
                hit = a2600session_controls_hit(lt, rt, x, y);
                /* the centre of a part hits it, or a part drawn over it */
                if (hit < 0 || hit < i) ok_centre = 0;
            }
        }
    check(ok_centre, "the centre of every pressable part hits that part (or one drawn over it)");

    c = a2600session_controls_layout(A2600_CTRL_JOYSTICK, A2600_CTRL_JOYSTICK, &n, &w, &h);
    check(a2600session_controls_hit(A2600_CTRL_JOYSTICK, A2600_CTRL_JOYSTICK, -1.0f, -1.0f) == -1, "outside the layout hits nothing");
    check(a2600session_controls_hit(A2600_CTRL_JOYSTICK, A2600_CTRL_JOYSTICK, w + 1.0f, h + 1.0f) == -1, "and past it");
    for (i = 0; i < n; i++)
        if (c[i].kind == A2600_CTL_CAPTION) {
            centre(&c[i], &x, &y);
            if (a2600session_controls_hit(A2600_CTRL_JOYSTICK, A2600_CTRL_JOYSTICK, x, y) >= 0
                && c[a2600session_controls_hit(A2600_CTRL_JOYSTICK, A2600_CTRL_JOYSTICK, x, y)].kind == A2600_CTL_CAPTION)
                ok_none = 0;
        }
    check(ok_none, "a caption is never hit");
    {
        const a2600_control *st = find(c, n, A2600_CTL_STICK, 0, 0);
        int hit;
        check(st != NULL, "the stick is there");
        if (st) {
            centre(st, &x, &y);
            hit = a2600session_controls_hit(A2600_CTRL_JOYSTICK, A2600_CTRL_JOYSTICK, x, y);
            check(hit >= 0 && c[hit].kind == A2600_CTL_STICK, "the stick's centre hits the stick");
            hit = a2600session_controls_hit(A2600_CTRL_JOYSTICK, A2600_CTRL_JOYSTICK, st->x + 0.05f, st->y + 0.05f);
            check(hit < 0 || c[hit].kind != A2600_CTL_STICK, "but its box's corner does not (the stick is round)");
            hit = a2600session_controls_hit(A2600_CTRL_JOYSTICK, A2600_CTRL_JOYSTICK, st->x + st->w - 0.05f, y);
            check(hit >= 0 && c[hit].kind == A2600_CTL_STICK, "while the edge of the circle on its axis does");
        }
    }
    c = a2600session_controls_layout(A2600_CTRL_PADDLES, A2600_CTRL_DRIVING, &n, &w, &h);
    {
        const a2600_control *kn = find(c, n, A2600_CTL_KNOB, 0, 1), *wh = find(c, n, A2600_CTL_WHEEL, 1, -1);
        int hit;
        if (kn) { centre(kn, &x, &y); hit = a2600session_controls_hit(A2600_CTRL_PADDLES, A2600_CTRL_DRIVING, x, y);
                  check(hit >= 0 && c[hit].kind == A2600_CTL_KNOB && c[hit].paddle == 1, "paddle B's knob is hit as paddle 1"); }
        if (wh) { centre(wh, &x, &y); hit = a2600session_controls_hit(A2600_CTRL_PADDLES, A2600_CTRL_DRIVING, x, y);
                  check(hit >= 0 && c[hit].kind == A2600_CTL_WHEEL, "the wheel is hit"); }
    }
}

static void drags(void)
{
    const unsigned U = 1u << A2600_ACT_JOY_UP, D = 1u << A2600_ACT_JOY_DOWN;
    const unsigned L = 1u << A2600_ACT_JOY_LEFT, R = 1u << A2600_ACT_JOY_RIGHT;

    check(a2600session_stick_from_vector(0, 0, 10) == 0, "the middle of the stick is nothing");
    check(a2600session_stick_from_vector(2, 0, 10) == 0, "and so is the quarter of its reach around it");
    check(a2600session_stick_from_vector(5, 0, 10) == R, "right");
    check(a2600session_stick_from_vector(-5, 0, 10) == L, "left");
    check(a2600session_stick_from_vector(0, -5, 10) == U, "up (+y is down)");
    check(a2600session_stick_from_vector(0, 5, 10) == D, "down");
    check(a2600session_stick_from_vector(5, -5, 10) == (U | R), "up-right is two bits");
    check(a2600session_stick_from_vector(-5, -5, 10) == (U | L), "up-left");
    check(a2600session_stick_from_vector(5, 5, 10) == (D | R), "down-right");
    check(a2600session_stick_from_vector(-5, 5, 10) == (D | L), "down-left");
    check(a2600session_stick_from_vector(10, 3, 10) == R, "a shallow angle is still right (22.5 degrees is the edge)");
    check(a2600session_stick_from_vector(10, 5, 10) == (D | R), "and a steeper one is the diagonal");
    check(a2600session_stick_from_vector(5, 5, 0) == 0, "a stick of no reach gives nothing");

    check(fabsf(a2600session_vector_angle(0, -1)) < 0.001f, "straight up is 0 degrees");
    check(fabsf(a2600session_vector_angle(1, 0) - 90.f) < 0.001f, "right is 90, clockwise");
    check(fabsf(a2600session_vector_angle(-1, 0) + 90.f) < 0.001f, "left is -90");
    check(fabsf(fabsf(a2600session_vector_angle(0, 1)) - 180.f) < 0.001f, "down is 180");

    check(a2600session_knob_from_vector(0, 0) == 0, "a pointer on the knob's centre is the centre position");
    check(a2600session_knob_from_vector(0, -1) == 0, "straight up is the middle of the pot");
    check(abs(a2600session_knob_from_vector(1, 0) - 21845) <= 1, "90 degrees clockwise is two thirds of the way");
    check(abs(a2600session_knob_from_vector(-1, 0) + 21845) <= 1, "and counter-clockwise the other way");
    check(a2600session_knob_from_vector(1, -1) > 10000 && a2600session_knob_from_vector(1, -1) < 11000, "45 degrees is a third");
    check(a2600session_knob_from_vector(1, 1) == 32767, "135 degrees is the clockwise stop");
    check(a2600session_knob_from_vector(-1, 1) == -32767, "and -135 the other");
    check(a2600session_knob_from_vector(0.1f, 1) == 32767 && a2600session_knob_from_vector(-0.1f, 1) == -32767,
          "past the stops it holds its end, on either side of straight down");
    {
        int prev = -32768, mono = 1, deg;
        for (deg = -135; deg <= 135; deg += 5) {
            const float rad = (float)deg * 3.14159265f / 180.0f;
            const int v = a2600session_knob_from_vector(sinf(rad), -cosf(rad));
            if (v < prev) mono = 0;
            prev = v;
        }
        check(mono, "the pot's position rises steadily across its travel");
    }
}

static void view_types(void)
{
    char cfg[512], data[512];
    a2600session_paths p;
    a2600session *s;

    test_tmpdir(cfg, sizeof cfg, "ctlcfg");
    snprintf(data, sizeof data, "%s-d", cfg);
    test_mkdir(data);
    memset(&p, 0, sizeof p);
    p.config_dir = cfg; p.data_dir = data; p.fujinet_lib = "";
    s = a2600session_new(&p);
    if (!s) { check(0, "session"); return; }
    check(a2600session_view_type(s, 0) == A2600_CTRL_JOYSTICK, "with nothing detected an AUTO port is drawn as a joystick");
    a2600session_set_port_type(s, 0, A2600_CTRL_PADDLES);
    a2600session_set_port_type(s, 1, A2600_CTRL_KEYPAD);
    check(a2600session_view_type(s, 0) == A2600_CTRL_PADDLES && a2600session_view_type(s, 1) == A2600_CTRL_KEYPAD,
          "an explicit type is what is drawn");
    check(a2600session_view_type(s, 2) == A2600_CTRL_JOYSTICK, "a bad port is a joystick");
    a2600session_free(s);
}

int main(void)
{
    every_combination();
    views();
    design();
    hits();
    drags();
    view_types();
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
