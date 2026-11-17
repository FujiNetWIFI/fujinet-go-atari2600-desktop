/*
 * gamepad_sdl -- SDL3 gamepads (and unmapped joysticks) as Atari 2600
 * controllers, on their own polling thread.
 *
 * A polling thread rather than SDL's event queue on the UI thread: this
 * app's video belongs to GTK/Qt/AppKit/Win32, not SDL, and only the gamepad
 * subsystem is initialised here -- never SDL video. Devices are added and
 * removed as they appear on the system: SDL posts the JOYSTICK_ and
 * GAMEPAD_ add/remove pairs and both are taken, with idempotent open/close,
 * so neither kind of device can be missed. A device SDL has no gamepad
 * mapping for (a plain HID adapter) is opened as a raw joystick instead:
 * its buttons and hats are bindable in the raw bands, just unnamed.
 *
 * What a pad drives follows the port's CURRENT controller type, read from
 * the session's effective_type (what Stella actually attached):
 *
 *   joystick  D-pad, plus the left stick when "analog sticks drive the
 *             joystick" is on (four-way with hysteresis).
 *   paddles   D-pad left/right turn paddle A, up/down paddle B; with
 *             "analog sticks drive paddles" on, the left stick's X is
 *             paddle A and the right stick's X is paddle B, as absolute
 *             positions.
 *   driving   D-pad left/right turn it; the left stick's X is the analog
 *             wheel when enabled.
 *   keypad    nothing implicit: twelve keys will not fit on a pad, and a
 *             guessed subset makes some games work and others fail for
 *             reasons the user cannot see. The keypad window covers it,
 *             and Map mode can bind any button to any key explicitly.
 *
 * Buttons go through the bindings table (a port's fire buttons, the
 * console switches, the system actions); a D-pad direction the user has
 * bound to something explicit stops also nudging the directional role.
 *
 * Pads are assigned to ports in connection order unless assigned explicitly.
 * An explicit assignment is remembered (settings key "pad_assign", see
 * pad_assign.h): a pad that is unplugged and plugged back in, or is there on
 * the next run, gets its port back -- matched by its SDL GUID, else by its
 * name. Every connect and disconnect is reported
 * (a2600session_gamepad_last_event) for the frontend's toast, and a pad that
 * goes away, moves to another port or changes what it plays lets go of
 * everything it held. Everything a pad holds is a source of its own in
 * input.c (its buttons and its stick separately), so letting go of one never
 * releases another, and a still stick never overrides the mouse.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "gamepad_sdl.h"
#include "input.h"
#include "pad_assign.h"
#include "session_internal.h"

#define MAX_PADS A2600_MAX_PADS
#define MAX_HATS 4

/* A third of full deflection engages, a sixth releases: low enough that a
 * worn stick still registers, high enough that resting drift does not, and
 * a gap wide enough that jitter across one threshold cannot flutter. */
#define STICK_ENTER 0.35f
#define STICK_EXIT  0.15f
#define TRIGGER_ON  0.5f

typedef struct {
    SDL_JoystickID id;
    int owner;                /* the input.c source number for this pad: stable
                               * while it is connected, whatever its slot */
    SDL_Gamepad *handle;      /* mapped gamepad, or NULL */
    SDL_Joystick *joy;        /* raw joystick when not a gamepad */
    int assign;               /* explicit port, -1 = automatic */
    int by_name;              /* assign was restored from a name-only match
                               * (the entry's GUID differs): an exact-GUID
                               * pad may take it over */
    int port;                 /* the port it drove on the last poll, or -1 */
    int type;                 /* the controller type it played on the last poll */
    int stick_dir;            /* left stick's four-way state (hysteresis) */
    int dpad_dir;             /* D-pad / hat 0 state */
    uint8_t sent[A2600_ACT_PER_PORT]; /* directional actions currently held */
    int sent_ax[3];           /* analog positions last handed to input.c */
    /* What each button's press asserted (target + 1, 0 = nothing), so its
     * release lets go of exactly that even if the binding changed meanwhile,
     * and a pad that goes away drops what it held. */
    int16_t btn_target[256];
    SDL_GUID guid;
    char guidstr[A2600_PAD_GUID_MAX];   /* guid as hex, for the pad_assign table */
    char name[A2600_PAD_NAME_MAX];
    int trigger_down[2];
    Uint8 last_hat[MAX_HATS];
} pad_slot;

typedef struct {
    struct a2600session *session;
    pthread_t thread;
    pthread_mutex_t lock;
    int running;
    volatile int stop_requested;
    pad_slot pads[MAX_PADS];
    int npads;
    unsigned generation;
    int quiet_scan;           /* the first scan of a restarted session re-finds
                               * pads without announcing them */
    /* Map-mode capture */
    int cap_active, cap_have, cap_result;
    /* the remembered explicit assignments (the "pad_assign" setting), for
     * pads that are away as well as those here; guarded by lock */
    a2600_pad_table table;
} gamepad_state;


/* ---- pure helpers ----------------------------------------------------------- */

int a2600_dir_from_stick(float x, float y, int prev, float enter, float exit)
{
    int dir = 0;
    /* each axis independently, so diagonals are two bits and a stick that
     * is exactly horizontal never picks up a vertical bit from noise */
    if (x <= -enter || (x <= -exit && (prev & A2600_DIR_LEFT))) dir |= A2600_DIR_LEFT;
    if (x >=  enter || (x >=  exit && (prev & A2600_DIR_RIGHT))) dir |= A2600_DIR_RIGHT;
    if (y <= -enter || (y <= -exit && (prev & A2600_DIR_UP))) dir |= A2600_DIR_UP;
    if (y >=  enter || (y >=  exit && (prev & A2600_DIR_DOWN))) dir |= A2600_DIR_DOWN;
    return dir;
}

int a2600_pad_for_port(const int *assign, int npads, int port)
{
    int i, next = 0;
    for (i = 0; i < npads; i++)
        if (assign[i] == port) return i;
    for (i = 0; i < npads; i++) {
        if (assign[i] >= 0) continue;   /* explicitly elsewhere */
        if (next == port) return i;
        next++;
    }
    return -1;
}

float a2600_paddle_from_axis(float value, float deadzone)
{
    float mag = value < 0.f ? -value : value;
    if (mag <= deadzone) return 0.f;
    mag = (mag - deadzone) / (1.f - deadzone);
    if (mag > 1.f) mag = 1.f;
    return value < 0.f ? -mag : mag;
}

int a2600_pad_button_from_sdl(uint8_t button)
{
    switch (button) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return A2600_PAD_BTN_SOUTH;
    case SDL_GAMEPAD_BUTTON_EAST: return A2600_PAD_BTN_EAST;
    case SDL_GAMEPAD_BUTTON_WEST: return A2600_PAD_BTN_WEST;
    case SDL_GAMEPAD_BUTTON_NORTH: return A2600_PAD_BTN_NORTH;
    case SDL_GAMEPAD_BUTTON_BACK: return A2600_PAD_BTN_BACK;
    case SDL_GAMEPAD_BUTTON_GUIDE: return A2600_PAD_BTN_GUIDE;
    case SDL_GAMEPAD_BUTTON_START: return A2600_PAD_BTN_START;
    case SDL_GAMEPAD_BUTTON_LEFT_STICK: return A2600_PAD_BTN_LEFT_STICK;
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return A2600_PAD_BTN_RIGHT_STICK;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return A2600_PAD_BTN_LEFT_SHOULDER;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return A2600_PAD_BTN_RIGHT_SHOULDER;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return A2600_PAD_BTN_DPAD_UP;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return A2600_PAD_BTN_DPAD_DOWN;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return A2600_PAD_BTN_DPAD_LEFT;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return A2600_PAD_BTN_DPAD_RIGHT;
    default: return A2600_PAD_BTN_NONE;
    }
}

int a2600_sdl_button_from_pad(int button)
{
    switch (button) {
    case A2600_PAD_BTN_SOUTH: return SDL_GAMEPAD_BUTTON_SOUTH;
    case A2600_PAD_BTN_EAST: return SDL_GAMEPAD_BUTTON_EAST;
    case A2600_PAD_BTN_WEST: return SDL_GAMEPAD_BUTTON_WEST;
    case A2600_PAD_BTN_NORTH: return SDL_GAMEPAD_BUTTON_NORTH;
    case A2600_PAD_BTN_BACK: return SDL_GAMEPAD_BUTTON_BACK;
    case A2600_PAD_BTN_GUIDE: return SDL_GAMEPAD_BUTTON_GUIDE;
    case A2600_PAD_BTN_START: return SDL_GAMEPAD_BUTTON_START;
    case A2600_PAD_BTN_LEFT_STICK: return SDL_GAMEPAD_BUTTON_LEFT_STICK;
    case A2600_PAD_BTN_RIGHT_STICK: return SDL_GAMEPAD_BUTTON_RIGHT_STICK;
    case A2600_PAD_BTN_LEFT_SHOULDER: return SDL_GAMEPAD_BUTTON_LEFT_SHOULDER;
    case A2600_PAD_BTN_RIGHT_SHOULDER: return SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER;
    case A2600_PAD_BTN_DPAD_UP: return SDL_GAMEPAD_BUTTON_DPAD_UP;
    case A2600_PAD_BTN_DPAD_DOWN: return SDL_GAMEPAD_BUTTON_DPAD_DOWN;
    case A2600_PAD_BTN_DPAD_LEFT: return SDL_GAMEPAD_BUTTON_DPAD_LEFT;
    case A2600_PAD_BTN_DPAD_RIGHT: return SDL_GAMEPAD_BUTTON_DPAD_RIGHT;
    default: return -1;
    }
}

/* ---- slots ------------------------------------------------------------------ */

static int slot_for_id(gamepad_state *g, SDL_JoystickID id)
{
    int i;
    for (i = 0; i < g->npads; i++)
        if (g->pads[i].id == id) return i;
    return -1;
}

static const char *slot_name(const pad_slot *slot)
{
    const char *n = slot->handle ? SDL_GetGamepadName(slot->handle)
                                 : SDL_GetJoystickName(slot->joy);
    return n ? n : "Gamepad";
}

/* Lets go of everything the pad holds, whichever way it was held. The caller
 * may hold g->lock: input.c's lock is a leaf below it. */
static void slot_release_all(gamepad_state *g, pad_slot *slot)
{
    memset(slot->sent, 0, sizeof slot->sent);
    memset(slot->sent_ax, 0, sizeof slot->sent_ax);
    memset(slot->btn_target, 0, sizeof slot->btn_target);
    input_pad_clear(g->session, slot->owner);
}

static void slot_close(gamepad_state *g, pad_slot *slot)
{
    slot_release_all(g, slot);
    if (slot->handle) SDL_CloseGamepad(slot->handle);
    if (slot->joy) SDL_CloseJoystick(slot->joy);
    slot->handle = NULL;
    slot->joy = NULL;
}

static int effective_port_locked(gamepad_state *g, int idx);

/* Gives the pad in slot idx its remembered port. An entry with the pad's own
 * GUID is the pad's: it takes it from a pad that got it only through the name
 * fallback. A pad known only by name takes the entry if no other pad carries
 * it (two identical pads, or one pad known under two GUIDs): then this one
 * stays automatic. Locked by the caller; only reads g->table, never writes. */
static void restore_assignment(gamepad_state *g, int idx)
{
    pad_slot *slot = &g->pads[idx];
    int ent = a2600_pad_table_find(&g->table, slot->guidstr, slot->name), k;
    int exact;

    if (ent < 0 || g->table.entry[ent].port >= 2) return;
    exact = !strcmp(g->table.entry[ent].guid, slot->guidstr);
    for (k = 0; k < g->npads; k++) {
        pad_slot *other = &g->pads[k];
        if (k == idx || other->assign < 0 ||
            a2600_pad_table_find(&g->table, other->guidstr, other->name) != ent)
            continue;
        if (!exact || !other->by_name) return;
        other->assign = -1;          /* the entry's owner is here: it wins */
        other->by_name = 0;
    }
    slot->assign = g->table.entry[ent].port;
    slot->by_name = !exact;
}

/* Idempotent, and the single place the gamepad/joystick decision is made:
 * both SDL_EVENT_JOYSTICK_ADDED and SDL_EVENT_GAMEPAD_ADDED land here (SDL
 * posts both for a mapped device) and the slot_for_id guard makes the second
 * one a no-op. */
static void open_pad(gamepad_state *g, SDL_JoystickID id)
{
    char msg[128];
    int announce = 0;

    pthread_mutex_lock(&g->lock);
    if (g->npads < MAX_PADS && slot_for_id(g, id) < 0) {
        /* Not an else-if on SDL_IsGamepad: a device SDL calls a gamepad can
         * still fail to open as one. Falling through to the raw joystick
         * keeps it usable -- every button is still bindable, just unnamed. */
        SDL_Gamepad *gp = SDL_IsGamepad(id) ? SDL_OpenGamepad(id) : NULL;
        SDL_Joystick *j = gp ? NULL : SDL_OpenJoystick(id);
        if (gp || j) {
            pad_slot *slot = &g->pads[g->npads++];
            int h, k, used = 0, port;
            for (k = 0; k < g->npads - 1; k++)
                used |= 1 << g->pads[k].owner;
            memset(slot, 0, sizeof *slot);
            for (k = 0; k < MAX_PADS && (used & (1 << k)); k++) {}
            slot->owner = k;
            slot->id = id;
            slot->handle = gp;
            slot->joy = j;
            slot->assign = -1;
            slot->port = -1;
            slot->type = -1;
            for (h = 0; h < MAX_HATS; h++) slot->last_hat[h] = SDL_HAT_CENTERED;
            slot->guid = SDL_GetJoystickGUIDForID(id);
            snprintf(slot->name, sizeof slot->name, "%s", slot_name(slot));
            SDL_GUIDToString(slot->guid, slot->guidstr, sizeof slot->guidstr);
            restore_assignment(g, g->npads - 1);
            fprintf(stderr, "a2600 gamepad: %s connected (%s)\n", slot->name,
                    gp ? "gamepad" : "raw joystick");
            if (!g->quiet_scan) {
                port = effective_port_locked(g, g->npads - 1);
                if (port >= 0)
                    snprintf(msg, sizeof msg, "Connected: %s (%s port)", slot->name,
                             port ? "right" : "left");
                else
                    snprintf(msg, sizeof msg, "Connected: %s", slot->name);
                g->generation++;
                announce = 1;
            }
        }
    }
    pthread_mutex_unlock(&g->lock);
    if (announce) session_gamepad_event(g->session, msg);
}

static void close_pad(gamepad_state *g, SDL_JoystickID id)
{
    char msg[128];
    int idx, i, announce = 0;
    pthread_mutex_lock(&g->lock);
    idx = slot_for_id(g, id);
    if (idx >= 0) {
        fprintf(stderr, "a2600 gamepad: %s disconnected\n", g->pads[idx].name);
        snprintf(msg, sizeof msg, "Disconnected: %s", g->pads[idx].name);
        slot_close(g, &g->pads[idx]);
        for (i = idx; i < g->npads - 1; i++) g->pads[i] = g->pads[i + 1];
        g->npads--;
        g->generation++;
        announce = 1;
    }
    pthread_mutex_unlock(&g->lock);
    if (announce) session_gamepad_event(g->session, msg);
}

/* ---- buttons ---------------------------------------------------------------- */

/* The port a slot drives right now. Locked by the caller. */
static int effective_port_locked(gamepad_state *g, int idx)
{
    int assign[MAX_PADS], i, port;
    for (i = 0; i < MAX_PADS; i++) assign[i] = -1;
    for (i = 0; i < g->npads; i++) assign[i] = g->pads[i].assign;
    for (port = 0; port < 2; port++)
        if (a2600_pad_for_port(assign, g->npads, port) == idx) return port;
    return -1;
}

static int is_sysact(int target)
{
    return target >= A2600_TARGET_SYSACT(0);
}

static void dispatch_button(gamepad_state *g, SDL_JoystickID which, int b, int down)
{
    int idx, port, owner = 0, target = -1;
    if (b == A2600_PAD_BTN_NONE) return;

    pthread_mutex_lock(&g->lock);
    if (g->cap_active) {
        /* Map mode: every button event is swallowed while armed, not just
         * the captured one, so the press that maps a button does not also
         * fire whatever it used to do. */
        if (down && !g->cap_have) { g->cap_have = 1; g->cap_result = b; }
        pthread_mutex_unlock(&g->lock);
        return;
    }
    idx = slot_for_id(g, which);
    port = idx >= 0 ? effective_port_locked(g, idx) : -1;
    if (idx >= 0 && port >= 0 && b >= 0 && b < 256) {
        pad_slot *slot = &g->pads[idx];
        owner = slot->owner;
        if (down) {
            target = a2600session_target_for_button(g->session, port, b);
            if (target >= 0 && !is_sysact(target))
                slot->btn_target[b] = (int16_t)(target + 1);
        } else if (slot->btn_target[b]) {
            /* what the press asserted, whatever the binding says now */
            target = slot->btn_target[b] - 1;
            slot->btn_target[b] = 0;
        }
    }
    pthread_mutex_unlock(&g->lock);
    if (target < 0) return;

    /* a system action is posted, on the press only, for the UI thread */
    if (is_sysact(target)) {
        if (down) a2600session_sysaction_post(g->session, target - A2600_TARGET_SYSACT(0));
        return;
    }
    input_press(g->session, A2600_SRC_PAD(owner), target, down);
}

static int hat_dir_from_mask(Uint8 mask)
{
    switch (mask) {
    case SDL_HAT_UP: return 0;
    case SDL_HAT_RIGHTUP: return 1;
    case SDL_HAT_RIGHT: return 2;
    case SDL_HAT_RIGHTDOWN: return 3;
    case SDL_HAT_DOWN: return 4;
    case SDL_HAT_LEFTDOWN: return 5;
    case SDL_HAT_LEFT: return 6;
    case SDL_HAT_LEFTUP: return 7;
    default: return -1;
    }
}

static void handle_hat_motion(gamepad_state *g, SDL_JoystickID which, int hat, Uint8 mask)
{
    int idx, prev_dir, dir;
    if (hat < 0 || hat >= MAX_HATS) return;
    pthread_mutex_lock(&g->lock);
    idx = slot_for_id(g, which);
    if (idx < 0 || g->pads[idx].handle) { pthread_mutex_unlock(&g->lock); return; }
    prev_dir = hat_dir_from_mask(g->pads[idx].last_hat[hat]);
    g->pads[idx].last_hat[hat] = mask;
    pthread_mutex_unlock(&g->lock);
    dir = hat_dir_from_mask(mask);
    /* hat positions are bindable buttons in their own band; hat 0's four
     * cardinal points also feed the directional role in poll_one */
    if (prev_dir >= 0)
        dispatch_button(g, which, A2600_PAD_HAT_BASE + hat * A2600_PAD_HAT_DIRS + prev_dir, 0);
    if (dir >= 0)
        dispatch_button(g, which, A2600_PAD_HAT_BASE + hat * A2600_PAD_HAT_DIRS + dir, 1);
}

/* After a session restart the new machine holds nothing, but a pad button may
 * still be physically held: assert it again, as its press would have. */
static void reassert_held(gamepad_state *g)
{
    int idx;
    for (idx = 0; idx < MAX_PADS; idx++) {
        int held[64 + 64 + MAX_HATS * A2600_PAD_HAT_DIRS];
        int nheld = 0, b, h, count;
        SDL_JoystickID id;
        pad_slot *slot;

        pthread_mutex_lock(&g->lock);
        if (idx >= g->npads) { pthread_mutex_unlock(&g->lock); break; }
        slot = &g->pads[idx];
        id = slot->id;
        /* the port it plays from now on, so the first poll sees no move
         * (a move releases whatever the pad holds) */
        slot->port = effective_port_locked(g, idx);
        slot->type = slot->port >= 0 ? g->session->effective_type[slot->port] : -1;
        if (slot->handle) {
            for (b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; b++)
                if (SDL_GetGamepadButton(slot->handle, (SDL_GamepadButton)b))
                    held[nheld++] = a2600_pad_button_from_sdl((uint8_t)b);
        } else if (slot->joy) {
            count = SDL_GetNumJoystickButtons(slot->joy);
            for (b = 0; b < count && b < 64; b++)
                if (SDL_GetJoystickButton(slot->joy, b))
                    held[nheld++] = A2600_PAD_BTN_RAW_BASE + b;
            count = SDL_GetNumJoystickHats(slot->joy);
            for (h = 0; h < count && h < MAX_HATS; h++) {
                Uint8 mask = SDL_GetJoystickHat(slot->joy, h);
                int dir = hat_dir_from_mask(mask);
                slot->last_hat[h] = mask;
                if (dir >= 0)
                    held[nheld++] = A2600_PAD_HAT_BASE + h * A2600_PAD_HAT_DIRS + dir;
            }
        }
        pthread_mutex_unlock(&g->lock);
        for (b = 0; b < nheld; b++)
            dispatch_button(g, id, held[b], 1);
    }
}

/* ---- the directional role and the analog controllers ------------------------ */

static float axis_norm(Sint16 v)
{
    return v < 0 ? (float)v / 32768.0f : (float)v / 32767.0f;
}

static void slot_stick(const pad_slot *slot, int right, float *x, float *y)
{
    if (slot->handle) {
        *x = axis_norm(SDL_GetGamepadAxis(slot->handle, right ? SDL_GAMEPAD_AXIS_RIGHTX : SDL_GAMEPAD_AXIS_LEFTX));
        *y = axis_norm(SDL_GetGamepadAxis(slot->handle, right ? SDL_GAMEPAD_AXIS_RIGHTY : SDL_GAMEPAD_AXIS_LEFTY));
    } else {
        int base = right ? 2 : 0;
        *x = SDL_GetNumJoystickAxes(slot->joy) > base ? axis_norm(SDL_GetJoystickAxis(slot->joy, base)) : 0.f;
        *y = SDL_GetNumJoystickAxes(slot->joy) > base + 1 ? axis_norm(SDL_GetJoystickAxis(slot->joy, base + 1)) : 0.f;
    }
}

static int slot_dpad(gamepad_state *g, const pad_slot *slot, int port)
{
    int dir = 0;
    if (slot->handle) {
        /* a D-pad direction the user bound explicitly no longer plays the
         * directional role -- otherwise a button remapped to a keypad key
         * would also nudge the joystick every poll */
        if (a2600session_target_for_button(g->session, port, A2600_PAD_BTN_DPAD_UP) < 0
            && SDL_GetGamepadButton(slot->handle, SDL_GAMEPAD_BUTTON_DPAD_UP)) dir |= A2600_DIR_UP;
        if (a2600session_target_for_button(g->session, port, A2600_PAD_BTN_DPAD_DOWN) < 0
            && SDL_GetGamepadButton(slot->handle, SDL_GAMEPAD_BUTTON_DPAD_DOWN)) dir |= A2600_DIR_DOWN;
        if (a2600session_target_for_button(g->session, port, A2600_PAD_BTN_DPAD_LEFT) < 0
            && SDL_GetGamepadButton(slot->handle, SDL_GAMEPAD_BUTTON_DPAD_LEFT)) dir |= A2600_DIR_LEFT;
        if (a2600session_target_for_button(g->session, port, A2600_PAD_BTN_DPAD_RIGHT) < 0
            && SDL_GetGamepadButton(slot->handle, SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) dir |= A2600_DIR_RIGHT;
    } else if (SDL_GetNumJoystickHats(slot->joy) > 0) {
        Uint8 m = SDL_GetJoystickHat(slot->joy, 0);
        if (m & SDL_HAT_UP) dir |= A2600_DIR_UP;
        if (m & SDL_HAT_DOWN) dir |= A2600_DIR_DOWN;
        if (m & SDL_HAT_LEFT) dir |= A2600_DIR_LEFT;
        if (m & SDL_HAT_RIGHT) dir |= A2600_DIR_RIGHT;
    }
    return dir;
}

static void want_action(uint8_t *want, int act, int on)
{
    if (on) want[act] = 1;
}

/* A stick axis as an analog position, -32767..32767: the dead zone taken out
 * so a worn stick still reaches the ends and a resting one never jitters, in
 * steps of 1/256 so a drifting value does not rewrite the position on every
 * poll. */
static int analog_value(float axis)
{
    const float f = a2600_paddle_from_axis(axis, A2600_STICK_DEADZONE);
    return (int)(roundf(f * 256.0f) / 256.0f * 32767.0f);
}

/* A trigger crossing its threshold. Dispatched by poll_sticks once it has
 * let go of the lock: dispatch_button takes it too, and it is not recursive. */
typedef struct {
    SDL_JoystickID id;
    int button;
    int down;
} trigger_edge;

#define MAX_EDGES (MAX_PADS * 2)

static void poll_one(gamepad_state *g, pad_slot *slot, int port,
                     trigger_edge *edges, int *nedges)
{
    struct a2600session *s = g->session;
    uint8_t want[A2600_ACT_PER_PORT];
    int want_ax[3] = { 0, 0, 0 };
    int type, dir, a;
    float lx, ly, rx, ry;

    memset(want, 0, sizeof want);
    type = port >= 0 ? s->effective_type[port] : -1;
    if (port != slot->port || type != slot->type) {
        /* moved to another port (or none), or what is plugged in changed
         * under it: release what it held and start again */
        slot_release_all(g, slot);
        slot->port = port;
        slot->type = type;
        slot->stick_dir = 0;
        if (port < 0) return;
    }

    slot->dpad_dir = slot_dpad(g, slot, port);
    slot_stick(slot, 0, &lx, &ly);
    slot_stick(slot, 1, &rx, &ry);

    switch (type) {
    case A2600_CTRL_JOYSTICK:
    case A2600_CTRL_AUTO:
        dir = slot->dpad_dir;
        if (s->opts.analog_joystick) {
            slot->stick_dir = a2600_dir_from_stick(lx, ly, slot->stick_dir, STICK_ENTER, STICK_EXIT);
            dir |= slot->stick_dir;
        }
        want_action(want, A2600_ACT_JOY_UP, dir & A2600_DIR_UP);
        want_action(want, A2600_ACT_JOY_DOWN, dir & A2600_DIR_DOWN);
        want_action(want, A2600_ACT_JOY_LEFT, dir & A2600_DIR_LEFT);
        want_action(want, A2600_ACT_JOY_RIGHT, dir & A2600_DIR_RIGHT);
        break;
    case A2600_CTRL_PADDLES:
        dir = slot->dpad_dir;
        want_action(want, A2600_ACT_PADDLE_A_DEC, dir & A2600_DIR_LEFT);
        want_action(want, A2600_ACT_PADDLE_A_INC, dir & A2600_DIR_RIGHT);
        want_action(want, A2600_ACT_PADDLE_B_DEC, dir & A2600_DIR_UP);
        want_action(want, A2600_ACT_PADDLE_B_INC, dir & A2600_DIR_DOWN);
        if (s->opts.analog_paddle) {
            /* absolute positions: A on the left stick's X, B on the right's */
            want_ax[0] = analog_value(lx);
            want_ax[1] = analog_value(rx);
        }
        break;
    case A2600_CTRL_DRIVING:
        dir = slot->dpad_dir;
        want_action(want, A2600_ACT_DRIVE_CCW, dir & A2600_DIR_LEFT);
        want_action(want, A2600_ACT_DRIVE_CW, dir & A2600_DIR_RIGHT);
        if (s->opts.analog_driving)
            want_ax[2] = analog_value(lx);
        break;
    default:
        break;   /* keypad: nothing implicit */
    }
    (void)ly; (void)ry;

    for (a = 0; a < A2600_ACT_PER_PORT; a++) {
        if (want[a] != slot->sent[a]) {
            slot->sent[a] = want[a];
            input_press(s, A2600_SRC_STICK(slot->owner), A2600_TARGET_PORT(port, a), want[a]);
        }
    }
    /* an analog position is written only when it changes, so a still stick
     * never overrides the Controllers window's knob */
    for (a = 0; a < 3; a++) {
        if (want_ax[a] != slot->sent_ax[a]) {
            slot->sent_ax[a] = want_ax[a];
            input_analog(s, A2600_SRC_STICK(slot->owner), port, a, want_ax[a]);
        }
    }

    /* the triggers, as buttons */
    if (slot->handle) {
        int t;
        for (t = 0; t < 2; t++) {
            float v = axis_norm(SDL_GetGamepadAxis(slot->handle,
                t ? SDL_GAMEPAD_AXIS_RIGHT_TRIGGER : SDL_GAMEPAD_AXIS_LEFT_TRIGGER));
            int down = v >= TRIGGER_ON;
            if (down != slot->trigger_down[t]) {
                slot->trigger_down[t] = down;
                if (*nedges < MAX_EDGES) {
                    edges[*nedges].id = slot->id;
                    edges[*nedges].button = t ? A2600_PAD_BTN_RIGHT_TRIGGER : A2600_PAD_BTN_LEFT_TRIGGER;
                    edges[*nedges].down = down;
                    (*nedges)++;
                }
            }
        }
    }
}

static void poll_sticks(gamepad_state *g)
{
    int i, nedges = 0;
    trigger_edge edges[MAX_EDGES];
    pthread_mutex_lock(&g->lock);
    for (i = 0; i < g->npads; i++)
        poll_one(g, &g->pads[i], effective_port_locked(g, i), edges, &nedges);
    pthread_mutex_unlock(&g->lock);
    for (i = 0; i < nedges; i++)
        dispatch_button(g, edges[i].id, edges[i].button, edges[i].down);
}

/* ---- the thread ------------------------------------------------------------- */

static void *thread_main(void *arg)
{
    gamepad_state *g = arg;
    int count = 0, i;
    /* Every joystick, not just the ones SDL has a gamepad mapping for:
     * SDL_GetGamepads would silently omit a plain HID adapter entirely. */
    SDL_JoystickID *ids = SDL_GetJoysticks(&count);
    if (ids) {
        for (i = 0; i < count; i++) open_pad(g, ids[i]);
        SDL_free(ids);
    }
    if (g->quiet_scan) {
        g->quiet_scan = 0;
        reassert_held(g);
    }

    while (!g->stop_requested) {
        SDL_Event ev;
        int npads_now;

        pthread_mutex_lock(&g->lock);
        npads_now = g->npads;
        pthread_mutex_unlock(&g->lock);
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_EVENT_JOYSTICK_ADDED: open_pad(g, ev.jdevice.which); break;
            case SDL_EVENT_JOYSTICK_REMOVED: close_pad(g, ev.jdevice.which); break;
            case SDL_EVENT_GAMEPAD_ADDED: open_pad(g, ev.gdevice.which); break;
            case SDL_EVENT_GAMEPAD_REMOVED: close_pad(g, ev.gdevice.which); break;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            case SDL_EVENT_GAMEPAD_BUTTON_UP:
                dispatch_button(g, ev.gbutton.which,
                                a2600_pad_button_from_sdl(ev.gbutton.button),
                                ev.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
                break;
            case SDL_EVENT_JOYSTICK_BUTTON_DOWN:
            case SDL_EVENT_JOYSTICK_BUTTON_UP: {
                /* a mapped gamepad also emits these for the same press it
                 * reported as a GAMEPAD_BUTTON event; the slot's kind decides */
                int gamepad, idx;
                pthread_mutex_lock(&g->lock);
                idx = slot_for_id(g, ev.jbutton.which);
                gamepad = idx >= 0 && g->pads[idx].handle != NULL;
                pthread_mutex_unlock(&g->lock);
                if (gamepad || ev.jbutton.button >= 64) break;
                dispatch_button(g, ev.jbutton.which,
                                A2600_PAD_BTN_RAW_BASE + ev.jbutton.button,
                                ev.type == SDL_EVENT_JOYSTICK_BUTTON_DOWN);
                break;
            }
            case SDL_EVENT_JOYSTICK_HAT_MOTION:
                handle_hat_motion(g, ev.jhat.which, ev.jhat.hat, ev.jhat.value);
                break;
            default:
                break;
            }
        }
        poll_sticks(g);
        /* ~60 Hz while a pad is attached; with none there is only hotplug to
         * notice, which can wait a quarter of a second. (SDL_WaitEventTimeout
         * polls at ~1 kHz inside, so it is no help.) */
        SDL_Delay(npads_now > 0 ? 16 : 250);
    }

    pthread_mutex_lock(&g->lock);
    for (i = 0; i < g->npads; i++) slot_close(g, &g->pads[i]);
    g->npads = 0;
    pthread_mutex_unlock(&g->lock);
    return NULL;
}

int gamepad_start(struct a2600session *s)
{
    gamepad_state *g;
    if (s->gamepad) return 0;
    /* The app owns its signals; SDL must not intercept SIGINT/SIGTERM. */
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    /* the poll thread is not the focused window: keep reading pads when the
     * app is in the background (the debugger or another window has focus) */
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    /* Bluetooth pads over hidapi and the joystick subsystem together. */
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "1");
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD | SDL_INIT_JOYSTICK)) {
        session_set_error(s, "SDL gamepad init failed: %s", SDL_GetError());
        return -1;
    }
    g = calloc(1, sizeof *g);
    if (!g) { SDL_QuitSubSystem(SDL_INIT_GAMEPAD | SDL_INIT_JOYSTICK); return -1; }
    g->session = s;
    pthread_mutex_init(&g->lock, NULL);
    /* the generation carries across a restart, so a frontend's baseline never
     * goes stale; a restarted thread's first scan re-finds the same pads, which
     * is no hot-plug: no toast and no change of generation */
    g->generation = s->pad_gen_base;
    g->quiet_scan = s->pad_restarted;
    /* the remembered assignments, once: garbage or no key is an empty table */
    a2600_pad_table_decode(&g->table, a2600session_get_str(s, "pad_assign", ""), 2);
    /* An explicit stack: the platform default is 512 KB on macOS, and SDL's
     * HID enumeration runs on this thread. */
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 4u << 20);
    if (pthread_create(&g->thread, &attr, thread_main, g) != 0) {
        pthread_attr_destroy(&attr);
        session_set_error(s, "gamepad thread could not start");
        pthread_mutex_destroy(&g->lock);
        free(g);
        SDL_QuitSubSystem(SDL_INIT_GAMEPAD | SDL_INIT_JOYSTICK);
        return -1;
    }
    pthread_attr_destroy(&attr);
    g->running = 1;
    s->gamepad = g;
    return 0;
}

void gamepad_stop(struct a2600session *s)
{
    gamepad_state *g = s->gamepad;
    if (!g) return;
    g->stop_requested = 1;
    pthread_join(g->thread, NULL);
    s->pad_gen_base = g->generation;
    s->pad_restarted = 1;
    pthread_mutex_destroy(&g->lock);
    s->gamepad = NULL;
    free(g);
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD | SDL_INIT_JOYSTICK);
}

/* ---- public API ------------------------------------------------------------- */

int a2600session_gamepad_count(a2600session *s)
{
    gamepad_state *g = s->gamepad;
    int n;
    if (!g) return 0;
    pthread_mutex_lock(&g->lock);
    n = g->npads;
    pthread_mutex_unlock(&g->lock);
    return n;
}

int a2600session_gamepad_name(a2600session *s, int idx, char *dst, int dstsz)
{
    gamepad_state *g = s->gamepad;
    int n = 0;
    if (!dst || dstsz <= 0) return 0;
    dst[0] = '\0';
    if (!g) return 0;
    pthread_mutex_lock(&g->lock);
    if (idx >= 0 && idx < g->npads)
        n = snprintf(dst, (size_t)dstsz, "%s", g->pads[idx].name);
    pthread_mutex_unlock(&g->lock);
    return n;
}

void a2600session_gamepad_assign(a2600session *s, int idx, int port)
{
    gamepad_state *g = s->gamepad;
    char text[A2600_PAD_ASSIGN_TEXT_MAX];
    int changed = 0;
    if (!g) return;
    pthread_mutex_lock(&g->lock);
    if (idx >= 0 && idx < g->npads) {
        pad_slot *slot = &g->pads[idx];
        if (port >= 0 && port < 2) {
            slot->assign = port;
            slot->by_name = 0;
            changed = a2600_pad_table_set(&g->table, slot->guidstr, slot->name, port) > 0;
        } else {
            const char *kguid[MAX_PADS], *kname[MAX_PADS];
            int k, nkeep = 0;
            slot->assign = -1;
            slot->by_name = 0;
            for (k = 0; k < g->npads; k++)
                if (k != idx && g->pads[k].assign >= 0) {
                    kguid[nkeep] = g->pads[k].guidstr;
                    kname[nkeep++] = g->pads[k].name;
                }
            changed = a2600_pad_table_remove(&g->table, slot->guidstr, slot->name,
                                             nkeep, kguid, kname);
        }
        if (changed && a2600_pad_table_encode(&g->table, text, sizeof text) < 0)
            changed = 0;
    }
    pthread_mutex_unlock(&g->lock);
    /* the settings are written here, on the caller's thread, with the pad lock
     * let go: the pad thread must never wait on the settings store */
    if (changed)
        a2600session_set_str(s, "pad_assign", text);
}

int a2600session_gamepad_assignment(a2600session *s, int idx)
{
    gamepad_state *g = s->gamepad;
    int r = -1;
    if (!g) return -1;
    pthread_mutex_lock(&g->lock);
    if (idx >= 0 && idx < g->npads) r = g->pads[idx].assign;
    pthread_mutex_unlock(&g->lock);
    return r;
}

int a2600session_gamepad_effective_port(a2600session *s, int idx)
{
    gamepad_state *g = s->gamepad;
    int r = -1;
    if (!g) return -1;
    pthread_mutex_lock(&g->lock);
    if (idx >= 0 && idx < g->npads) r = effective_port_locked(g, idx);
    pthread_mutex_unlock(&g->lock);
    return r;
}

unsigned a2600session_gamepad_generation(a2600session *s)
{
    gamepad_state *g = s->gamepad;
    unsigned r;
    if (!g) return s->pad_gen_base;
    pthread_mutex_lock(&g->lock);
    r = g->generation;
    pthread_mutex_unlock(&g->lock);
    return r;
}

void a2600session_gamepad_capture_begin(a2600session *s)
{
    gamepad_state *g = s->gamepad;
    if (!g) return;
    pthread_mutex_lock(&g->lock);
    g->cap_active = 1; g->cap_have = 0; g->cap_result = A2600_PAD_BTN_NONE;
    pthread_mutex_unlock(&g->lock);
}

void a2600session_gamepad_capture_cancel(a2600session *s)
{
    gamepad_state *g = s->gamepad;
    if (!g) return;
    pthread_mutex_lock(&g->lock);
    g->cap_active = 0; g->cap_have = 0;
    pthread_mutex_unlock(&g->lock);
}

int a2600session_gamepad_capture_poll(a2600session *s, int *button)
{
    gamepad_state *g = s->gamepad;
    int got = 0;
    if (!g) return 0;
    pthread_mutex_lock(&g->lock);
    if (g->cap_active && g->cap_have) {
        if (button) *button = g->cap_result;
        g->cap_active = 0; g->cap_have = 0;
        got = 1;
    }
    pthread_mutex_unlock(&g->lock);
    return got;
}
