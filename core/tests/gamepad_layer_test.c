/*
 * gamepad_layer_test -- the gamepad layer end to end with SDL3 virtual
 * joysticks: no hardware, no human and no machine. Pads take the two ports in
 * connection order; an explicit assignment moves them; two pads on one port do
 * not release each other; a button, the D-pad and the stick are each a
 * source of their own; a trigger axis is a bindable button and cannot hang the
 * pad lock; hot-unplug releases exactly what that pad held and recentres the
 * analog position it moved; a still stick never overrides the mouse; Map's
 * capture swallows the button it takes; a raw HID adapter's buttons and hat
 * work; and the assignment is remembered ("pad_assign"), restored when the pad
 * comes back (on the same session and a new one), found through a stale GUID
 * by its name, and announced with a toast and a generation that carry across a
 * restart of the thread.
 *
 * What a pad holds is read back through a2600session_buttons_held and
 * a2600session_analog_get, which report what the console is driven with. The
 * controller types are set through the session's effective_type (what Stella
 * attached), since no machine runs. Every wait polls with a ten second
 * ceiling, so a slow runner is merely slow. SKIPs (77) where SDL has no
 * virtual joysticks or real pads are attached.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "a2600session.h"
#include "gamepad_sdl.h"
#include "input.h"
#include "session_internal.h"
#include "test_tmpdir.h"

#define NAME_FMT "a2600test pad %c"
#define WAIT_TRIES 500           /* x 20 ms: ten seconds */
#define BIT(act) (1u << (act))
#define PAD_BTN(name) SDL_GAMEPAD_BUTTON_##name

static int failures;

static void check(int pass, const char *what)
{
    printf("%s: %s\n", pass ? "ok" : "FAIL", what);
    if (!pass) failures++;
}

static void sleep_ms(int msec)
{
    struct timespec stamp = { msec / 1000, (long)(msec % 1000) * 1000000L };
    nanosleep(&stamp, NULL);
}

static a2600session *g_s;
static char g_cfg[512], g_data[512];

static void expect_held(int port, unsigned want, const char *what)
{
    unsigned held = 0;
    int tries;
    for (tries = 0; tries < WAIT_TRIES; tries++) {
        held = a2600session_buttons_held(g_s, port);
        if (held == want) {
            printf("ok: %s\n", what);
            return;
        }
        sleep_ms(20);
    }
    printf("FAIL: %s (port %d held %07X want %07X)\n", what, port, held, want);
    failures++;
}

static void expect_analog(int port, int paddle, int lo, int hi, const char *what)
{
    int value = 0, tries;
    for (tries = 0; tries < WAIT_TRIES; tries++) {
        value = a2600session_analog_get(g_s, port, paddle);
        if (value >= lo && value <= hi) {
            printf("ok: %s\n", what);
            return;
        }
        sleep_ms(20);
    }
    printf("FAIL: %s (position %d, want %d..%d)\n", what, value, lo, hi);
    failures++;
}

/* A distinct vendor/product per pad: virtual joysticks share one GUID
 * otherwise, and the assignment table would not tell them apart. */
static SDL_JoystickID attach_pad(const char *name, int raw, int product)
{
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    if (raw) {
        desc.type = SDL_JOYSTICK_TYPE_UNKNOWN;
        desc.naxes = 4;
        desc.nbuttons = 8;
        desc.nhats = 1;
    } else {
        desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
        desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    }
    desc.vendor_id = 0x1209;
    desc.product_id = (Uint16)product;
    desc.name = name;
    return SDL_AttachVirtualJoystick(&desc);
}

static int pad_slot_now(const char *name)
{
    int idx;
    for (idx = 0; idx < a2600session_gamepad_count(g_s); idx++) {
        char got[64];
        a2600session_gamepad_name(g_s, idx, got, sizeof got);
        if (strcmp(got, name) == 0)
            return idx;
    }
    return -1;
}

static int wait_for_pad(const char *name)
{
    int idx = pad_slot_now(name), tries;
    for (tries = 0; tries < WAIT_TRIES && idx < 0; tries++) {
        sleep_ms(20);
        idx = pad_slot_now(name);
    }
    return idx;
}

static void wait_gone(const char *name)
{
    int tries;
    for (tries = 0; tries < WAIT_TRIES && pad_slot_now(name) >= 0; tries++)
        sleep_ms(20);
}

static void pad_name(char *dst, size_t size, int num)
{
    snprintf(dst, size, NAME_FMT, 'A' + num);
}

static int port_of(const char *name)
{
    const int idx = wait_for_pad(name);
    return idx < 0 ? -2 : a2600session_gamepad_effective_port(g_s, idx);
}

/* a port that has settled (the pad thread sees an assignment a poll later and
 * lets go of what the pad held) */
static int wait_port(const char *name, int want)
{
    int tries;
    for (tries = 0; tries < WAIT_TRIES; tries++) {
        if (port_of(name) == want) break;
        sleep_ms(20);
    }
    sleep_ms(120);
    return port_of(name) == want;
}

static int wait_sysaction(int want)
{
    int tries, got = -1;
    for (tries = 0; tries < WAIT_TRIES; tries++) {
        if (a2600session_sysaction_take(g_s, &got))
            return got == want;
        sleep_ms(20);
    }
    return 0;
}

static void new_session(void)
{
    a2600session_paths p;
    memset(&p, 0, sizeof p);
    p.config_dir = g_cfg; p.data_dir = g_data; p.fujinet_lib = "";
    g_s = a2600session_new(&p);
}

static int session_up(void)
{
    new_session();
    if (!g_s)
        return 0;
    /* no machine runs, so the start options never reach the session: the
     * sticks drive what they would by default */
    a2600session_set_analog(g_s, 1, 1, 1);
    if (gamepad_start(g_s) != 0) {
        printf("SKIP: the gamepad layer would not start: %s\n", a2600session_last_error(g_s));
        a2600session_free(g_s);
        g_s = NULL;
        return 0;
    }
    return 1;
}

static void session_down(void)
{
    gamepad_stop(g_s);
    a2600session_free(g_s);
    g_s = NULL;
}

static int has_entry(const char *guid, const char *name, int port)
{
    char want[200];
    snprintf(want, sizeof want, "%s|%s=%d", guid, name, port);
    return strstr(a2600session_get_str(g_s, "pad_assign", ""), want) != NULL;
}

/* ---- one pad: what it plays and what it holds --------------------------------------- */

static void basics(void)
{
    char name[64], text[200];
    SDL_JoystickID vid;
    SDL_Joystick *joy;
    int idx;
    unsigned gen0;

    pad_name(name, sizeof name, 0);
    vid = attach_pad(name, 0, 0x1000);
    if (!vid) { check(0, "basics: the virtual pad attaches"); return; }
    joy = SDL_OpenJoystick(vid);

    if (!session_up()) { failures++; SDL_CloseJoystick(joy); SDL_DetachVirtualJoystick(vid); return; }
    gen0 = a2600session_gamepad_generation(g_s);
    idx = wait_for_pad(name);
    check(idx >= 0, "the pad is picked up");
    if (idx < 0) goto out;
    check(a2600session_gamepad_assignment(g_s, idx) == -1, "a pad nobody assigned is automatic");
    check(a2600session_gamepad_effective_port(g_s, idx) == 0, "the first pad plays the left port");
    check(a2600session_gamepad_generation(g_s) != gen0, "its connecting changed the generation");
    a2600session_gamepad_last_event(g_s, text, sizeof text);
    check(strstr(text, "Connected: ") == text && strstr(text, name) != NULL && strstr(text, "(left port)") != NULL,
          "the toast says it connected and which port it plays");

    /* default mapping: South fires, the D-pad steers the joystick */
    SDL_SetJoystickVirtualButton(joy, PAD_BTN(SOUTH), true);
    expect_held(0, BIT(A2600_ACT_JOY_FIRE), "South is the joystick's fire");
    SDL_SetJoystickVirtualButton(joy, PAD_BTN(DPAD_UP), true);
    expect_held(0, BIT(A2600_ACT_JOY_FIRE) | BIT(A2600_ACT_JOY_UP), "the D-pad's up steers up");
    SDL_SetJoystickVirtualButton(joy, PAD_BTN(DPAD_LEFT), true);
    expect_held(0, BIT(A2600_ACT_JOY_FIRE) | BIT(A2600_ACT_JOY_UP) | BIT(A2600_ACT_JOY_LEFT), "and left: a diagonal");
    SDL_SetJoystickVirtualButton(joy, PAD_BTN(SOUTH), false);
    SDL_SetJoystickVirtualButton(joy, PAD_BTN(DPAD_UP), false);
    SDL_SetJoystickVirtualButton(joy, PAD_BTN(DPAD_LEFT), false);
    expect_held(0, 0, "everything let go");

    /* the left stick steers too, with hysteresis */
    SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_LEFTX, 32767);
    expect_held(0, BIT(A2600_ACT_JOY_RIGHT), "the left stick, pushed right, steers right");
    SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_LEFTX, 0);
    expect_held(0, 0, "and centred lets go");

    /* the keyboard on the same fire: the pad's hold survives the key's release */
    SDL_SetJoystickVirtualButton(joy, PAD_BTN(SOUTH), true);
    expect_held(0, BIT(A2600_ACT_JOY_FIRE), "the pad holds fire again");
    a2600session_key(g_s, A2600_KEYSYM_SPACE, 1);
    a2600session_key(g_s, A2600_KEYSYM_SPACE, 0);
    expect_held(0, BIT(A2600_ACT_JOY_FIRE), "a key pressed and released on the same fire leaves the pad's hold");
    SDL_SetJoystickVirtualButton(joy, PAD_BTN(SOUTH), false);
    expect_held(0, 0, "released");

    /* Back is Select, Start is Reset (a momentary console switch) */
    SDL_SetJoystickVirtualButton(joy, PAD_BTN(START), true);
    sleep_ms(150);
    check(a2600session_switch_held(g_s, A2600_SW_RESET), "Start holds the console's Reset");
    SDL_SetJoystickVirtualButton(joy, PAD_BTN(START), false);
    sleep_ms(150);
    check(!a2600session_switch_held(g_s, A2600_SW_RESET), "and lets go");

    /* a button on a session action posts it, once, on the press */
    a2600session_binding_set_button(g_s, A2600_TARGET_SYSACT(A2600_SYSACT_PAUSE), A2600_PAD_BTN_NORTH, NULL, 0);
    {
        int stale;
        while (a2600session_sysaction_take(g_s, &stale)) {}
    }
    SDL_SetJoystickVirtualButton(joy, PAD_BTN(NORTH), true);
    check(wait_sysaction(A2600_SYSACT_PAUSE), "a pad button bound to Pause posts it");
    sleep_ms(150);
    {
        int more;
        check(!a2600session_sysaction_take(g_s, &more), "once: holding it posts nothing more");
    }
    SDL_SetJoystickVirtualButton(joy, PAD_BTN(NORTH), false);
    a2600session_bindings_reset(g_s);

    /* a trigger axis is a bindable button, and a held trigger cannot hang the pad lock */
    a2600session_binding_set_button(g_s, A2600_TARGET_PORT(0, A2600_ACT_JOY_FIRE), A2600_PAD_BTN_LEFT_TRIGGER, NULL, 0);
    /* (a virtual trigger is released at the axis minimum: 0 reads as half pressed) */
    SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, -32768);
    sleep_ms(250);
    SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 32767);
    expect_held(0, BIT(A2600_ACT_JOY_FIRE), "the left trigger, bound to fire, fires");
    check(a2600session_gamepad_effective_port(g_s, 0) == 0, "the pad lock is still free with a trigger held");
    SDL_SetJoystickVirtualButton(joy, PAD_BTN(DPAD_DOWN), true);
    expect_held(0, BIT(A2600_ACT_JOY_FIRE) | BIT(A2600_ACT_JOY_DOWN), "and other inputs still work while it is held");
    SDL_SetJoystickVirtualButton(joy, PAD_BTN(DPAD_DOWN), false);
    SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, -32768);
    expect_held(0, 0, "the trigger released");
    a2600session_bindings_reset(g_s);

    /* Map: a button is captured and swallowed */
    {
        int btn = -1, got = 0, tries;
        a2600session_gamepad_capture_begin(g_s);
        SDL_SetJoystickVirtualButton(joy, PAD_BTN(EAST), true);
        for (tries = 0; tries < WAIT_TRIES && !got; tries++) {
            got = a2600session_gamepad_capture_poll(g_s, &btn);
            if (!got) sleep_ms(20);
        }
        check(got && btn == A2600_PAD_BTN_EAST, "Map captures the next button pressed (East)");
        sleep_ms(100);
        check(a2600session_buttons_held(g_s, 0) == 0, "and the press that mapped it fired nothing");
        SDL_SetJoystickVirtualButton(joy, PAD_BTN(EAST), false);
        a2600session_gamepad_capture_begin(g_s);
        a2600session_gamepad_capture_cancel(g_s);
        SDL_SetJoystickVirtualButton(joy, PAD_BTN(SOUTH), true);
        expect_held(0, BIT(A2600_ACT_JOY_FIRE), "a cancelled capture lets buttons fire again");
        SDL_SetJoystickVirtualButton(joy, PAD_BTN(SOUTH), false);
    }

    /* hot-unplug releases exactly what the pad held, buttons and stick */
    SDL_SetJoystickVirtualButton(joy, PAD_BTN(SOUTH), true);
    SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_LEFTX, -32768);
    expect_held(0, BIT(A2600_ACT_JOY_FIRE) | BIT(A2600_ACT_JOY_LEFT), "fire and left held");
    a2600session_key(g_s, '1', 1);
    expect_held(0, BIT(A2600_ACT_JOY_FIRE) | BIT(A2600_ACT_JOY_LEFT) | BIT(A2600_ACT_KEY_1), "and the keyboard holds keypad 1");
    SDL_CloseJoystick(joy); joy = NULL;
    SDL_DetachVirtualJoystick(vid);
    wait_gone(name);
    check(pad_slot_now(name) < 0, "the pad went away");
    expect_held(0, BIT(A2600_ACT_KEY_1), "unplugged: its buttons and its stick are let go, the keyboard's hold stays");
    a2600session_key(g_s, '1', 0);
    a2600session_gamepad_last_event(g_s, text, sizeof text);
    check(strstr(text, "Disconnected: ") == text && strstr(text, name) != NULL, "the toast says it disconnected");
    expect_held(0, 0, "all clear");
    vid = 0;
out:
    if (joy) SDL_CloseJoystick(joy);
    if (vid) SDL_DetachVirtualJoystick(vid);
    session_down();
}

/* ---- two pads: assignment, and two on one port ---------------------------------------- */

static void two_pads(void)
{
    char name[2][64];
    SDL_JoystickID vid[2];
    SDL_Joystick *joy[2];
    int i;

    for (i = 0; i < 2; i++) {
        pad_name(name[i], sizeof name[i], i);
        vid[i] = attach_pad(name[i], 0, 0x1100 + i);
        if (!vid[i]) { check(0, "two pads: both virtual pads attach"); return; }
        joy[i] = SDL_OpenJoystick(vid[i]);
        sleep_ms(60);
    }
    if (!session_up()) { failures++; goto out; }
    check(wait_port(name[0], 0) && wait_port(name[1], 1), "two pads: the first plays the left port, the second the right");
    SDL_SetJoystickVirtualButton(joy[1], PAD_BTN(SOUTH), true);
    expect_held(1, BIT(A2600_ACT_JOY_FIRE), "the second pad fires the right port");
    check(a2600session_buttons_held(g_s, 0) == 0, "and not the left");
    SDL_SetJoystickVirtualButton(joy[1], PAD_BTN(SOUTH), false);

    /* pad A on the right port: pad B (automatic) takes the left */
    a2600session_gamepad_assign(g_s, wait_for_pad(name[0]), 1);
    check(wait_port(name[0], 1) && wait_port(name[1], 0), "pad A assigned to the right port: pad B plays the left");
    SDL_SetJoystickVirtualButton(joy[0], PAD_BTN(SOUTH), true);
    expect_held(1, BIT(A2600_ACT_JOY_FIRE), "pad A now fires the right port");
    check(a2600session_buttons_held(g_s, 0) == 0, "and not the left");
    /* moving a pad to another port lets go of what it held on the first */
    a2600session_gamepad_assign(g_s, wait_for_pad(name[0]), 0);
    check(wait_port(name[0], 0), "pad A assigned back to the left port");
    expect_held(1, 0, "a pad that moves lets go of what it held on its old port");
    sleep_ms(150);
    check(a2600session_buttons_held(g_s, 0) == 0, "(its button, still down, is not asserted on the new port until pressed again)");
    SDL_SetJoystickVirtualButton(joy[0], PAD_BTN(SOUTH), false);
    sleep_ms(100);
    SDL_SetJoystickVirtualButton(joy[0], PAD_BTN(SOUTH), true);
    expect_held(0, BIT(A2600_ACT_JOY_FIRE), "pressed again, it fires the left port");
    SDL_SetJoystickVirtualButton(joy[0], PAD_BTN(SOUTH), false);
    expect_held(0, 0, "released");
    a2600session_gamepad_assign(g_s, wait_for_pad(name[0]), -1);

    /* two pads' fire on one port: one lets go, the other still holds it */
    a2600session_gamepad_assign(g_s, wait_for_pad(name[0]), 0);
    a2600session_gamepad_assign(g_s, wait_for_pad(name[1]), 1);
    check(wait_port(name[0], 0) && wait_port(name[1], 1), "each pad on its port");
    a2600session_gamepad_assign(g_s, wait_for_pad(name[1]), 0);
    check(wait_port(name[0], 0) && wait_port(name[1], -1), "a second pad assigned to the same port plays nothing (the first holds it)");
    a2600session_gamepad_assign(g_s, wait_for_pad(name[0]), -1);
    a2600session_gamepad_assign(g_s, wait_for_pad(name[1]), -1);

    /* unplugging a pad does not move the other's port */
    check(wait_port(name[0], 0) && wait_port(name[1], 1), "automatic again: A left, B right");
    SDL_CloseJoystick(joy[0]); joy[0] = NULL;
    SDL_DetachVirtualJoystick(vid[0]); vid[0] = 0;
    wait_gone(name[0]);
    check(wait_port(name[1], 0), "pad A unplugged: pad B (automatic) moves to the first free port");
out:
    for (i = 0; i < 2; i++) {
        if (joy[i]) SDL_CloseJoystick(joy[i]);
        if (vid[i]) SDL_DetachVirtualJoystick(vid[i]);
    }
    if (g_s) session_down();
}

/* ---- the analog controllers ---------------------------------------------------------- */

static void analog(void)
{
    char name[64];
    SDL_JoystickID vid;
    SDL_Joystick *joy;
    int idx;

    pad_name(name, sizeof name, 3);
    vid = attach_pad(name, 0, 0x1300);
    if (!vid) { check(0, "analog: the virtual pad attaches"); return; }
    joy = SDL_OpenJoystick(vid);
    if (!session_up()) { failures++; goto out; }
    idx = wait_for_pad(name);
    check(idx >= 0, "analog: the pad is picked up");
    if (idx < 0) goto out;

    /* paddles: the left stick's X is paddle A, absolutely; the right's is B */
    g_s->effective_type[0] = A2600_CTRL_PADDLES;
    SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_LEFTX, 16384);
    expect_analog(0, 0, 12000, 16500, "paddle A follows the left stick (dead zone taken out)");
    SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_RIGHTX, -32768);
    expect_analog(0, 1, -32767, -32000, "paddle B follows the right stick");
    SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_LEFTX, 0);
    SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_RIGHTX, 0);
    expect_analog(0, 0, 0, 0, "centred: paddle A is back at the middle");
    expect_analog(0, 1, 0, 0, "and B");
    /* worn-stick drift inside the dead zone does nothing */
    SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_LEFTX, 2500);
    sleep_ms(200);
    check(a2600session_analog_get(g_s, 0, 0) == 0, "drift inside the dead zone writes nothing");
    SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_LEFTX, 0);

    /* a still stick never overrides the mouse */
    a2600session_pointer_analog(g_s, 0, 0, 20000, 1);
    sleep_ms(300);
    check(a2600session_analog_get(g_s, 0, 0) == 20000, "a centred pad leaves the mouse's drag alone");
    a2600session_pointer_analog(g_s, 0, 0, 0, 0);
    a2600session_pointer_analog(g_s, 0, 0, 0, 1);
    a2600session_pointer_analog(g_s, 0, 0, 0, 0);

    /* unplugging a deflected stick recentres what it moved */
    SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_LEFTX, 32767);
    expect_analog(0, 0, 32000, 32767, "deflected to the end");
    SDL_CloseJoystick(joy); joy = NULL;
    SDL_DetachVirtualJoystick(vid); vid = 0;
    wait_gone(name);
    expect_analog(0, 0, 0, 0, "unplugged: the paddle goes back to the middle");

    /* the driving controller: the left stick's X is the wheel */
    g_s->effective_type[0] = A2600_CTRL_DRIVING;
    vid = attach_pad(name, 0, 0x1300);
    joy = vid ? SDL_OpenJoystick(vid) : NULL;
    idx = wait_for_pad(name);
    check(idx >= 0, "the pad is back");
    SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_LEFTX, -32768);
    expect_analog(0, -1, -32767, -32000, "driving: the left stick's X is the wheel");
    SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_LEFTX, 0);
    SDL_SetJoystickVirtualButton(joy, PAD_BTN(DPAD_RIGHT), true);
    expect_held(0, BIT(A2600_ACT_DRIVE_CW), "and the D-pad's right turns it clockwise");
    SDL_SetJoystickVirtualButton(joy, PAD_BTN(DPAD_RIGHT), false);
    expect_held(0, 0, "released");
    /* turning the type under a held pad lets go of what it held as the old type */
    SDL_SetJoystickVirtualButton(joy, PAD_BTN(DPAD_LEFT), true);
    expect_held(0, BIT(A2600_ACT_DRIVE_CCW), "the D-pad's left is the wheel's other way");
    g_s->effective_type[0] = A2600_CTRL_JOYSTICK;
    expect_held(0, BIT(A2600_ACT_JOY_LEFT), "and when a game swaps in a joystick, it is joystick-left, not the wheel");
    SDL_SetJoystickVirtualButton(joy, PAD_BTN(DPAD_LEFT), false);
    expect_held(0, 0, "released");
    /* the keypad: nothing implicit, though a bound button works */
    g_s->effective_type[0] = A2600_CTRL_KEYPAD;
    SDL_SetJoystickVirtualButton(joy, PAD_BTN(DPAD_LEFT), true);
    sleep_ms(200);
    check(a2600session_buttons_held(g_s, 0) == 0, "a keypad port gets nothing implicit from the D-pad");
    SDL_SetJoystickVirtualButton(joy, PAD_BTN(DPAD_LEFT), false);
out:
    if (joy) SDL_CloseJoystick(joy);
    if (vid) SDL_DetachVirtualJoystick(vid);
    if (g_s) session_down();
}

/* ---- a raw HID adapter --------------------------------------------------------------- */

static void raw_adapter(void)
{
    SDL_JoystickID vid;
    SDL_Joystick *joy;
    const char *name = "a2600test raw adapter";

    vid = attach_pad(name, 1, 0x1400);
    if (!vid) { check(0, "raw: the virtual adapter attaches"); return; }
    joy = SDL_OpenJoystick(vid);
    if (!session_up()) { failures++; goto out; }
    check(wait_for_pad(name) >= 0, "raw: an unmapped adapter is picked up as a pad");

    /* its buttons are in the raw band; bind one to fire */
    a2600session_binding_set_button(g_s, A2600_TARGET_PORT(0, A2600_ACT_JOY_FIRE), A2600_PAD_BTN_RAW_BASE + 2, NULL, 0);
    SDL_SetJoystickVirtualButton(joy, 2, true);
    expect_held(0, BIT(A2600_ACT_JOY_FIRE), "raw button 2, bound to fire, fires");
    SDL_SetJoystickVirtualButton(joy, 2, false);
    expect_held(0, 0, "and lets go");
    a2600session_bindings_reset(g_s);

    /* hat 0 steers, and its directions are bindable too */
    SDL_SetJoystickVirtualHat(joy, 0, SDL_HAT_LEFT);
    expect_held(0, BIT(A2600_ACT_JOY_LEFT), "the hat's left steers left");
    SDL_SetJoystickVirtualHat(joy, 0, SDL_HAT_RIGHTUP);
    expect_held(0, BIT(A2600_ACT_JOY_RIGHT) | BIT(A2600_ACT_JOY_UP), "and a diagonal is two directions");
    SDL_SetJoystickVirtualHat(joy, 0, SDL_HAT_CENTERED);
    expect_held(0, 0, "centred lets go");
    a2600session_binding_set_button(g_s, A2600_TARGET_PORT(0, A2600_ACT_JOY_FIRE), A2600_PAD_HAT_BASE + 4, NULL, 0); /* hat 0 down */
    SDL_SetJoystickVirtualHat(joy, 0, SDL_HAT_DOWN);
    expect_held(0, BIT(A2600_ACT_JOY_FIRE) | BIT(A2600_ACT_JOY_DOWN), "a hat direction bound to fire fires (and still steers)");
    SDL_SetJoystickVirtualHat(joy, 0, SDL_HAT_CENTERED);
    expect_held(0, 0, "released");
    a2600session_bindings_reset(g_s);

    /* axes 0/1 are the left stick */
    SDL_SetJoystickVirtualAxis(joy, 0, 32767);
    expect_held(0, BIT(A2600_ACT_JOY_RIGHT), "axis 0, pushed, steers right");
    SDL_SetJoystickVirtualAxis(joy, 0, 0);
    expect_held(0, 0, "centred");
out:
    if (joy) SDL_CloseJoystick(joy);
    if (vid) SDL_DetachVirtualJoystick(vid);
    if (g_s) session_down();
}

/* ---- the remembered assignment ------------------------------------------------------- */

static void persistence(void)
{
    char name[64], guid[64], text[200];
    SDL_JoystickID vid;
    SDL_Joystick *joy;
    int idx;
    unsigned gen_before;

    {
        /* the earlier tests assigned pads: start from no settings */
        char path[600];
        snprintf(path, sizeof path, "%s/settings.ini", g_cfg);
        remove(path);
    }
    pad_name(name, sizeof name, 5);
    vid = attach_pad(name, 0, 0x1500);
    if (!vid) { check(0, "persistence: the virtual pad attaches"); return; }
    joy = SDL_OpenJoystick(vid);
    SDL_GUIDToString(SDL_GetJoystickGUIDForID(vid), guid, sizeof guid);

    if (!session_up()) { failures++; goto out; }
    idx = wait_for_pad(name);
    check(idx >= 0, "persistence: the pad is picked up");
    if (idx < 0) goto out;
    check(strstr(a2600session_get_str(g_s, "pad_assign", ""), guid) == NULL, "a pad nobody assigned has no entry");

    a2600session_gamepad_assign(g_s, idx, 1);
    check(a2600session_gamepad_assignment(g_s, idx) == 1 && a2600session_gamepad_effective_port(g_s, idx) == 1
          && has_entry(guid, name, 1), "assigning the right port is remembered: <guid>|<name>=1 in pad_assign");
    {
        char path[600], line[16384];
        int found = 0;
        FILE *file;
        a2600session_settings_flush(g_s);
        snprintf(path, sizeof path, "%s/settings.ini", g_cfg);
        file = fopen(path, "r");
        if (file) {
            while (fgets(line, sizeof line, file))
                if (!strncmp(line, "pad_assign=", 11) && strstr(line, guid))
                    found = 1;
            fclose(file);
        }
        check(found, "the settings file on disk holds it");
    }

    /* unplugged and plugged back in on the same session: it gets the port back */
    SDL_CloseJoystick(joy); joy = NULL;
    SDL_DetachVirtualJoystick(vid); vid = 0;
    wait_gone(name);
    gen_before = a2600session_gamepad_generation(g_s);
    check(pad_slot_now(name) < 0 && has_entry(guid, name, 1), "unplugged: the entry stays");
    vid = attach_pad(name, 0, 0x1500);
    joy = vid ? SDL_OpenJoystick(vid) : NULL;
    idx = wait_for_pad(name);
    check(idx >= 0 && a2600session_gamepad_assignment(g_s, idx) == 1 && a2600session_gamepad_effective_port(g_s, idx) == 1,
          "plugged back in: the assignment is restored");
    check(a2600session_gamepad_generation(g_s) != gen_before, "and the reconnection changed the generation");
    a2600session_gamepad_last_event(g_s, text, sizeof text);
    check(strstr(text, "Connected: ") == text && strstr(text, "(right port)") != NULL, "the toast says it plays the right port");

    /* a new session on the same settings */
    gen_before = a2600session_gamepad_generation(g_s);
    session_down();
    if (!session_up()) { failures++; goto out; }
    idx = wait_for_pad(name);
    check(idx >= 0 && a2600session_gamepad_assignment(g_s, idx) == 1 && a2600session_gamepad_effective_port(g_s, idx) == 1,
          "a new session: the assignment is restored on device add");

    /* a stale GUID (another USB port): the name finds the entry */
    snprintf(text, sizeof text, "ffffffffffffffffffffffffffffffff|%s=1", name);
    a2600session_set_str(g_s, "pad_assign", text);
    session_down();
    if (!session_up()) { failures++; goto out; }
    idx = wait_for_pad(name);
    check(idx >= 0 && a2600session_gamepad_assignment(g_s, idx) == 1,
          "a different GUID with the same name gets the assignment too");

    /* Automatic removes the entry */
    a2600session_gamepad_assign(g_s, idx, 1);
    check(has_entry(guid, name, 1), "assigned under the pad's own GUID again");
    a2600session_gamepad_assign(g_s, idx, -1);
    check(a2600session_gamepad_assignment(g_s, idx) == -1 && strstr(a2600session_get_str(g_s, "pad_assign", ""), name) == NULL,
          "Automatic: no explicit port, and the entry is gone");

    /* garbage, and a port outside 0..1, are ignored */
    a2600session_set_str(g_s, "pad_assign", "garbage;;|=;zz|x=1;");
    session_down();
    if (!session_up()) { failures++; goto out; }
    idx = wait_for_pad(name);
    check(idx >= 0 && a2600session_gamepad_assignment(g_s, idx) == -1, "garbage in pad_assign is skipped, not fatal");
    snprintf(text, sizeof text, "%s|%s=7", guid, name);
    a2600session_set_str(g_s, "pad_assign", text);
    session_down();
    if (!session_up()) { failures++; goto out; }
    idx = wait_for_pad(name);
    check(idx >= 0 && a2600session_gamepad_assignment(g_s, idx) == -1, "a port past the two is ignored");

    /* the generation and the toast carry across a restart of the thread */
    gen_before = a2600session_gamepad_generation(g_s);
    a2600session_gamepad_last_event(g_s, text, sizeof text);
    {
        char again[200];
        gamepad_stop(g_s);
        check(a2600session_gamepad_generation(g_s) == gen_before, "stopping the thread keeps the generation");
        if (gamepad_start(g_s) != 0) { check(0, "the thread restarts"); goto out; }
        idx = wait_for_pad(name);
        check(idx >= 0, "the restarted thread finds the pad again");
        sleep_ms(200);
        a2600session_gamepad_last_event(g_s, again, sizeof again);
        check(a2600session_gamepad_generation(g_s) == gen_before && strcmp(text, again) == 0,
              "and it is no hot-plug: the generation and the toast are unchanged");
    }
out:
    if (joy) SDL_CloseJoystick(joy);
    if (vid) SDL_DetachVirtualJoystick(vid);
    if (g_s) session_down();
}

int main(void)
{
    int count = 0;
    SDL_JoystickID *real;

    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "0");
    if (!SDL_Init(SDL_INIT_JOYSTICK | SDL_INIT_GAMEPAD)) {
        printf("SKIP: SDL joystick init failed: %s\n", SDL_GetError());
        return 77;
    }
    real = SDL_GetJoysticks(&count);
    SDL_free(real);
    if (count > 0) {
        printf("SKIP: %d real joystick(s) attached; this test needs a clean set\n", count);
        SDL_Quit();
        return 77;
    }
    {
        SDL_VirtualJoystickDesc desc;
        SDL_JoystickID probe;
        SDL_INIT_INTERFACE(&desc);
        desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
        desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
        desc.name = "a2600test probe";
        probe = SDL_AttachVirtualJoystick(&desc);
        if (!probe) {
            printf("SKIP: SDL has no virtual joysticks here: %s\n", SDL_GetError());
            SDL_Quit();
            return 77;
        }
        SDL_DetachVirtualJoystick(probe);
    }
    /* This test keeps its own hold on the SDL subsystem for the whole run: a
     * session's stop would otherwise drop the last reference, and SDL destroys
     * the virtual pads with it (a real pad survives that, a virtual one does
     * not). The session starts and stops on top of it. */

    test_tmpdir(g_cfg, sizeof g_cfg, "padcfg");
    snprintf(g_data, sizeof g_data, "%s-d", g_cfg);
    test_mkdir(g_data);

    basics();
    two_pads();
    analog();
    raw_adapter();
    persistence();

    SDL_Quit();
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
