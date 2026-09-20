/*
 * bindings -- the remappable key/gamepad table, the keyboard translator,
 * and every name the keypad window's Map row and the settings store need.
 *
 * One flat target table (a2600session.h's A2600_TARGET_* indices): per port
 * the joystick, both paddles, the driving controller and the twelve keypad
 * keys; then the five console switches; then the session's own actions.
 * Each target holds at most one keysym and one gamepad button. Rebinding
 * STEALS -- a key drives exactly one target, because one keystroke doing
 * two things is worse than losing the old binding: the second effect is
 * invisible until it matters.
 *
 * Seeds its defaults LAZILY so the table is usable with no session behind
 * it: a unit test with no settings store gets the documented default map.
 * Persisted as one packed "bindings" key holding only the entries that
 * differ from the defaults ("<target>.k:<keysym>" / "<target>.b:<button>").
 *
 * Keysyms are X11's, folded to lower case before lookup so a binding made
 * with 'w' still fires while Shift is held for something else.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hid_keys.h"
#include "session_internal.h"

typedef struct {
    uint32_t keysym;
    int button;
} slot;

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static slot s_table[A2600_TARGET_COUNT];
static slot s_defaults[A2600_TARGET_COUNT];
static int s_seeded;
static struct a2600session *s_store;   /* whose settings hold "bindings" */

/* ---- defaults ------------------------------------------------------------- */

static void seed_key(int target, uint32_t keysym)
{
    s_defaults[target].keysym = keysym;
}

static void seed_button(int target, int button)
{
    s_defaults[target].button = button;
}

/* Stella's own default keyboard map, so its users feel at home: left
 * joystick on the arrows + Space, right on Y/G/H/J + F; left keypad on
 * 1 2 3 / Q W E / A S D / Z X C, right on 8 9 0 / I O P / K L ; / , . /.
 * Paddles ride the joystick directions (A: left/right, B: up/down) and
 * the driving controller left/right, so a game that wants one of those
 * works from the same keys without a second map to learn. */
static void compute_defaults_locked(void)
{
    int t;
    for (t = 0; t < A2600_TARGET_COUNT; t++) {
        s_defaults[t].keysym = 0;
        s_defaults[t].button = A2600_PAD_BTN_NONE;
    }

    /* left port */
    seed_key(A2600_TARGET_PORT(0, A2600_ACT_JOY_UP), A2600_KEYSYM_UP);
    seed_key(A2600_TARGET_PORT(0, A2600_ACT_JOY_DOWN), A2600_KEYSYM_DOWN);
    seed_key(A2600_TARGET_PORT(0, A2600_ACT_JOY_LEFT), A2600_KEYSYM_LEFT);
    seed_key(A2600_TARGET_PORT(0, A2600_ACT_JOY_RIGHT), A2600_KEYSYM_RIGHT);
    seed_key(A2600_TARGET_PORT(0, A2600_ACT_JOY_FIRE), A2600_KEYSYM_SPACE);
    seed_key(A2600_TARGET_PORT(0, A2600_ACT_PADDLE_A_DEC), A2600_KEYSYM_LEFT);
    seed_key(A2600_TARGET_PORT(0, A2600_ACT_PADDLE_A_INC), A2600_KEYSYM_RIGHT);
    seed_key(A2600_TARGET_PORT(0, A2600_ACT_PADDLE_A_FIRE), A2600_KEYSYM_SPACE);
    seed_key(A2600_TARGET_PORT(0, A2600_ACT_PADDLE_B_DEC), A2600_KEYSYM_UP);
    seed_key(A2600_TARGET_PORT(0, A2600_ACT_PADDLE_B_INC), A2600_KEYSYM_DOWN);
    seed_key(A2600_TARGET_PORT(0, A2600_ACT_PADDLE_B_FIRE), A2600_KEYSYM_LCTRL);
    seed_key(A2600_TARGET_PORT(0, A2600_ACT_DRIVE_CCW), A2600_KEYSYM_LEFT);
    seed_key(A2600_TARGET_PORT(0, A2600_ACT_DRIVE_CW), A2600_KEYSYM_RIGHT);
    seed_key(A2600_TARGET_PORT(0, A2600_ACT_DRIVE_FIRE), A2600_KEYSYM_SPACE);
    {
        static const uint32_t keys[12] = { '1','2','3','q','w','e','a','s','d','z','x','c' };
        int i;
        for (i = 0; i < 12; i++)
            seed_key(A2600_TARGET_PORT(0, A2600_ACT_KEY_1 + i), keys[i]);
    }

    /* right port */
    seed_key(A2600_TARGET_PORT(1, A2600_ACT_JOY_UP), 'y');
    seed_key(A2600_TARGET_PORT(1, A2600_ACT_JOY_DOWN), 'h');
    seed_key(A2600_TARGET_PORT(1, A2600_ACT_JOY_LEFT), 'g');
    seed_key(A2600_TARGET_PORT(1, A2600_ACT_JOY_RIGHT), 'j');
    seed_key(A2600_TARGET_PORT(1, A2600_ACT_JOY_FIRE), 'f');
    seed_key(A2600_TARGET_PORT(1, A2600_ACT_PADDLE_A_DEC), 'g');
    seed_key(A2600_TARGET_PORT(1, A2600_ACT_PADDLE_A_INC), 'j');
    seed_key(A2600_TARGET_PORT(1, A2600_ACT_PADDLE_A_FIRE), 'f');
    seed_key(A2600_TARGET_PORT(1, A2600_ACT_PADDLE_B_DEC), 'y');
    seed_key(A2600_TARGET_PORT(1, A2600_ACT_PADDLE_B_INC), 'h');
    seed_key(A2600_TARGET_PORT(1, A2600_ACT_PADDLE_B_FIRE), 'v');
    seed_key(A2600_TARGET_PORT(1, A2600_ACT_DRIVE_CCW), 'g');
    seed_key(A2600_TARGET_PORT(1, A2600_ACT_DRIVE_CW), 'j');
    seed_key(A2600_TARGET_PORT(1, A2600_ACT_DRIVE_FIRE), 'f');
    {
        static const uint32_t keys[12] = { '8','9','0','i','o','p','k','l',';',',','.','/' };
        int i;
        for (i = 0; i < 12; i++)
            seed_key(A2600_TARGET_PORT(1, A2600_ACT_KEY_1 + i), keys[i]);
    }

    /* console switches: Stella's F1/F2/F3; the difficulty switches on
     * Alt+L / Alt+R are menu accelerators, not bindings (F5/F7/F8 are the
     * family's debugger keys and stay free of the machine). */
    seed_key(A2600_TARGET_SWITCH(A2600_SW_SELECT), A2600_KEYSYM_F1);
    seed_key(A2600_TARGET_SWITCH(A2600_SW_RESET), A2600_KEYSYM_F2);
    seed_key(A2600_TARGET_SWITCH(A2600_SW_COLOR_BW), A2600_KEYSYM_F3);

    /* system actions */
    seed_key(A2600_TARGET_SYSACT(A2600_SYSACT_REBOOT_CONFIG), A2600_KEYSYM_ESCAPE);

    /* gamepad defaults, uniform on both ports: face buttons fire, the
     * shoulders turn paddles and the driving controller, Start/Back are
     * the console's Reset/Select. Directions come from the D-pad and the
     * sticks in gamepad_sdl.c, not from bindings. */
    {
        int port;
        for (port = 0; port < 2; port++) {
            seed_button(A2600_TARGET_PORT(port, A2600_ACT_JOY_FIRE), A2600_PAD_BTN_SOUTH);
            seed_button(A2600_TARGET_PORT(port, A2600_ACT_PADDLE_A_FIRE), A2600_PAD_BTN_SOUTH);
            seed_button(A2600_TARGET_PORT(port, A2600_ACT_PADDLE_B_FIRE), A2600_PAD_BTN_EAST);
            seed_button(A2600_TARGET_PORT(port, A2600_ACT_DRIVE_FIRE), A2600_PAD_BTN_SOUTH);
            seed_button(A2600_TARGET_PORT(port, A2600_ACT_PADDLE_A_DEC), A2600_PAD_BTN_LEFT_SHOULDER);
            seed_button(A2600_TARGET_PORT(port, A2600_ACT_PADDLE_A_INC), A2600_PAD_BTN_RIGHT_SHOULDER);
            seed_button(A2600_TARGET_PORT(port, A2600_ACT_DRIVE_CCW), A2600_PAD_BTN_LEFT_SHOULDER);
            seed_button(A2600_TARGET_PORT(port, A2600_ACT_DRIVE_CW), A2600_PAD_BTN_RIGHT_SHOULDER);
        }
        seed_button(A2600_TARGET_SWITCH(A2600_SW_RESET), A2600_PAD_BTN_START);
        seed_button(A2600_TARGET_SWITCH(A2600_SW_SELECT), A2600_PAD_BTN_BACK);
    }
}

static uint32_t fold(uint32_t keysym)
{
    if (keysym >= 'A' && keysym <= 'Z') return keysym + 32;
    /* the numeric keypad's digits mean the digits: one binding drives both */
    if (keysym >= A2600_KEYSYM_KP_0 && keysym <= A2600_KEYSYM_KP_9)
        return '0' + (keysym - A2600_KEYSYM_KP_0);
    if (keysym == A2600_KEYSYM_KP_ENTER) return A2600_KEYSYM_RETURN;
    if (keysym == A2600_KEYSYM_KP_MULTIPLY) return '*';
    if (keysym == A2600_KEYSYM_KP_DIVIDE) return '/';
    if (keysym == A2600_KEYSYM_KP_PERIOD) return '.';
    return keysym;
}

/* ---- persistence ---------------------------------------------------------- */

static void apply_persisted_locked(const char *packed)
{
    const char *p = packed;
    while (p && *p) {
        int target = -1; char kind = 0; long value = 0;
        const char *end = strchr(p, ' ');
        if (sscanf(p, "%d.%c:%ld", &target, &kind, &value) == 3
            && target >= 0 && target < A2600_TARGET_COUNT) {
            if (kind == 'k') s_table[target].keysym = (uint32_t)value;
            else if (kind == 'b') s_table[target].button = (int)value;
        }
        if (!end) break;
        p = end + 1;
    }
}

static void pack_locked(char *out, size_t outsz)
{
    size_t len = 0;
    int t;
    out[0] = '\0';
    for (t = 0; t < A2600_TARGET_COUNT; t++) {
        if (s_table[t].keysym != s_defaults[t].keysym)
            len += (size_t)snprintf(out + len, outsz - len, "%s%d.k:%u",
                                    len ? " " : "", t, s_table[t].keysym);
        if (len >= outsz) break;
        if (s_table[t].button != s_defaults[t].button)
            len += (size_t)snprintf(out + len, outsz - len, "%s%d.b:%d",
                                    len ? " " : "", t, s_table[t].button);
        if (len >= outsz) break;
    }
}

static void persist_locked(void)
{
    char packed[8192];
    if (!s_store) return;
    pack_locked(packed, sizeof packed);
    a2600session_set_str(s_store, "bindings", packed);
}

static void ensure_seeded_locked(void)
{
    if (s_seeded) return;
    compute_defaults_locked();
    memcpy(s_table, s_defaults, sizeof s_table);
    s_seeded = 1;
}

void bindings_init(struct a2600session *s)
{
    pthread_mutex_lock(&s_lock);
    compute_defaults_locked();
    memcpy(s_table, s_defaults, sizeof s_table);
    s_seeded = 1;
    s_store = s;
    if (s)
        apply_persisted_locked(a2600session_get_str(s, "bindings", ""));
    pthread_mutex_unlock(&s_lock);
}

/* ---- lookups -------------------------------------------------------------- */

static int target_for_key_locked(uint32_t keysym)
{
    int t;
    if (!keysym) return -1;
    for (t = 0; t < A2600_TARGET_COUNT; t++)
        if (s_table[t].keysym == keysym) return t;
    return -1;
}

int a2600session_target_for_key(a2600session *s, uint32_t keysym)
{
    int t;
    (void)s;
    pthread_mutex_lock(&s_lock);
    ensure_seeded_locked();
    t = target_for_key_locked(fold(keysym));
    pthread_mutex_unlock(&s_lock);
    return t;
}

/* Gamepad buttons are scoped to the pad's port: the same button index is
 * free to mean different things on different ports, so only that port's
 * targets and the machine-wide switches/actions are searched. */
int a2600session_target_for_button(a2600session *s, int port, int button)
{
    int t, found = -1;
    (void)s;
    if (button == A2600_PAD_BTN_NONE) return -1;
    pthread_mutex_lock(&s_lock);
    ensure_seeded_locked();
    for (t = 0; t < A2600_ACT_PER_PORT; t++)
        if (s_table[A2600_TARGET_PORT(port, t)].button == button) {
            found = A2600_TARGET_PORT(port, t);
            break;
        }
    if (found < 0)
        for (t = 2 * A2600_ACT_PER_PORT; t < A2600_TARGET_COUNT; t++)
            if (s_table[t].button == button) { found = t; break; }
    pthread_mutex_unlock(&s_lock);
    return found;
}

a2600_binding a2600session_binding_get(a2600session *s, int target)
{
    a2600_binding b = { 0, A2600_PAD_BTN_NONE };
    (void)s;
    if (target < 0 || target >= A2600_TARGET_COUNT) return b;
    pthread_mutex_lock(&s_lock);
    ensure_seeded_locked();
    b.keysym = s_table[target].keysym;
    b.button = s_table[target].button;
    pthread_mutex_unlock(&s_lock);
    return b;
}

/* A key may legitimately drive several targets: by default one key fires
 * the joystick, paddle and driving controller of the same port, since only
 * one of them is ever plugged in. An explicit rebinding is the user saying
 * "this key does exactly this", so it displaces every holder -- and names
 * them all, comma-separated, so nothing is lost silently. */
static void describe_stolen(const int *victims, int nvictims, char *stolen,
                            int stolensz)
{
    int i, len = 0;
    if (!stolen || stolensz <= 0) return;
    stolen[0] = '\0';
    for (i = 0; i < nvictims && len < stolensz; i++)
        len += snprintf(stolen + len, (size_t)(stolensz - len), "%s%s",
                        i ? ", " : "", a2600_target_name(victims[i]));
}

void a2600session_binding_set_key(a2600session *s, int target, uint32_t keysym,
                                  char *stolen, int stolensz)
{
    int victims[A2600_TARGET_COUNT], nvictims = 0, t;
    (void)s;
    if (target < 0 || target >= A2600_TARGET_COUNT) return;
    keysym = fold(keysym);
    pthread_mutex_lock(&s_lock);
    ensure_seeded_locked();
    if (keysym) {
        for (t = 0; t < A2600_TARGET_COUNT; t++)
            if (t != target && s_table[t].keysym == keysym) {
                s_table[t].keysym = 0;
                victims[nvictims++] = t;
            }
    }
    s_table[target].keysym = keysym;
    persist_locked();
    pthread_mutex_unlock(&s_lock);
    describe_stolen(victims, nvictims, stolen, stolensz);
}

void a2600session_binding_set_button(a2600session *s, int target, int button,
                                     char *stolen, int stolensz)
{
    int victims[A2600_TARGET_COUNT], nvictims = 0, t;
    int port = target / A2600_ACT_PER_PORT;   /* 2 = switches/sysactions */
    (void)s;
    if (target < 0 || target >= A2600_TARGET_COUNT) return;
    pthread_mutex_lock(&s_lock);
    ensure_seeded_locked();
    if (button != A2600_PAD_BTN_NONE) {
        for (t = 0; t < A2600_TARGET_COUNT; t++) {
            int tport = t / A2600_ACT_PER_PORT;
            /* steal only within the same scope: this port, or the
             * machine-wide targets, which every pad reaches */
            if (t != target && s_table[t].button == button
                && (tport == port || tport >= 2 || port >= 2)) {
                s_table[t].button = A2600_PAD_BTN_NONE;
                victims[nvictims++] = t;
            }
        }
    }
    s_table[target].button = button;
    persist_locked();
    pthread_mutex_unlock(&s_lock);
    describe_stolen(victims, nvictims, stolen, stolensz);
}

void a2600session_bindings_reset(a2600session *s)
{
    (void)s;
    pthread_mutex_lock(&s_lock);
    compute_defaults_locked();
    memcpy(s_table, s_defaults, sizeof s_table);
    s_seeded = 1;
    if (s_store) a2600session_set_str(s_store, "bindings", "");
    pthread_mutex_unlock(&s_lock);
}

/* ---- names ---------------------------------------------------------------- */

static const char *const act_names[A2600_ACT_PER_PORT] = {
    "Joystick Up", "Joystick Down", "Joystick Left", "Joystick Right",
    "Joystick Fire",
    "Paddle A Left", "Paddle A Right", "Paddle A Fire",
    "Paddle B Left", "Paddle B Right", "Paddle B Fire",
    "Driving Left", "Driving Right", "Driving Fire",
    "Keypad 1", "Keypad 2", "Keypad 3", "Keypad 4", "Keypad 5", "Keypad 6",
    "Keypad 7", "Keypad 8", "Keypad 9", "Keypad *", "Keypad 0", "Keypad #",
};
static const char *const act_short[A2600_ACT_PER_PORT] = {
    "Up", "Down", "Left", "Right", "Fire",
    "A-", "A+", "A Fire", "B-", "B+", "B Fire",
    "CCW", "CW", "Fire",
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "*", "0", "#",
};
static const char *const switch_names[A2600_SW_COUNT] = {
    "Select", "Reset", "Color / B&W", "Left Difficulty", "Right Difficulty",
};
static const char *const sysact_names[A2600_SYSACT_COUNT] = {
    "Reboot to CONFIG", "Pause",
};

const char *a2600_target_name(int target)
{
    static char buf[64];
    if (target < 0 || target >= A2600_TARGET_COUNT) return "";
    if (target < 2 * A2600_ACT_PER_PORT) {
        snprintf(buf, sizeof buf, "%s %s", target < A2600_ACT_PER_PORT ? "Left" : "Right",
                 act_names[target % A2600_ACT_PER_PORT]);
        return buf;
    }
    target -= 2 * A2600_ACT_PER_PORT;
    if (target < A2600_SW_COUNT) return switch_names[target];
    return sysact_names[target - A2600_SW_COUNT];
}

const char *a2600_target_short_name(int target)
{
    if (target < 0 || target >= A2600_TARGET_COUNT) return "";
    if (target < 2 * A2600_ACT_PER_PORT) return act_short[target % A2600_ACT_PER_PORT];
    target -= 2 * A2600_ACT_PER_PORT;
    if (target < A2600_SW_COUNT) return switch_names[target];
    return sysact_names[target - A2600_SW_COUNT];
}

static const char *const pad_button_names[A2600_PAD_BTN_COUNT] = {
    "A", "B", "X", "Y", "Back", "Guide", "Start", "Left Stick", "Right Stick",
    "Left Shoulder", "Right Shoulder", "D-pad Up", "D-pad Down", "D-pad Left",
    "D-pad Right", "Left Trigger", "Right Trigger",
};

const char *a2600_pad_button_name(int button)
{
    static char buf[32];
    if (A2600_PAD_BTN_IS_NAMED(button)) return pad_button_names[button];
    if (A2600_PAD_BTN_IS_RAW(button)) {
        snprintf(buf, sizeof buf, "Button %d", button - A2600_PAD_BTN_RAW_BASE);
        return buf;
    }
    if (A2600_PAD_BTN_IS_HAT(button)) {
        static const char *const dirs[A2600_PAD_HAT_DIRS] =
            { "Up", "Up-Right", "Right", "Down-Right", "Down", "Down-Left",
              "Left", "Up-Left" };
        int h = (button - A2600_PAD_HAT_BASE) / A2600_PAD_HAT_DIRS;
        int d = (button - A2600_PAD_HAT_BASE) % A2600_PAD_HAT_DIRS;
        snprintf(buf, sizeof buf, "Hat %d %s", h, dirs[d]);
        return buf;
    }
    return "";
}

int a2600session_keysym_name(uint32_t keysym, char *dst, int dstsz)
{
    const char *name = NULL;
    if (!dst || dstsz <= 0) return 0;
    switch (keysym) {
    case 0: name = ""; break;
    case A2600_KEYSYM_UP: name = "Up"; break;
    case A2600_KEYSYM_DOWN: name = "Down"; break;
    case A2600_KEYSYM_LEFT: name = "Left"; break;
    case A2600_KEYSYM_RIGHT: name = "Right"; break;
    case A2600_KEYSYM_ESCAPE: name = "Escape"; break;
    case A2600_KEYSYM_RETURN: name = "Return"; break;
    case A2600_KEYSYM_BACKSPACE: name = "Backspace"; break;
    case A2600_KEYSYM_TAB: name = "Tab"; break;
    case A2600_KEYSYM_SPACE: name = "Space"; break;
    case A2600_KEYSYM_LSHIFT: name = "Left Shift"; break;
    case A2600_KEYSYM_RSHIFT: name = "Right Shift"; break;
    case A2600_KEYSYM_LCTRL: name = "Left Ctrl"; break;
    case A2600_KEYSYM_RCTRL: name = "Right Ctrl"; break;
    case A2600_KEYSYM_LALT: name = "Left Alt"; break;
    case A2600_KEYSYM_RALT: name = "Right Alt"; break;
    case A2600_KEYSYM_KP_ENTER: name = "Keypad Enter"; break;
    case A2600_KEYSYM_KP_MULTIPLY: name = "Keypad *"; break;
    case A2600_KEYSYM_KP_DIVIDE: name = "Keypad /"; break;
    case A2600_KEYSYM_KP_PERIOD: name = "Keypad ."; break;
    case 0xffff: name = "Delete"; break;
    case 0xff63: name = "Insert"; break;
    case 0xff50: name = "Home"; break;
    case 0xff57: name = "End"; break;
    case 0xff55: name = "Page Up"; break;
    case 0xff56: name = "Page Down"; break;
    case 0xffe5: name = "Caps Lock"; break;
    default: break;
    }
    if (name) return snprintf(dst, (size_t)dstsz, "%s", name);
    if (keysym >= A2600_KEYSYM_F1 && keysym <= A2600_KEYSYM_F12)
        return snprintf(dst, (size_t)dstsz, "F%u", keysym - A2600_KEYSYM_F1 + 1);
    if (keysym >= A2600_KEYSYM_KP_0 && keysym <= A2600_KEYSYM_KP_9)
        return snprintf(dst, (size_t)dstsz, "Keypad %u", keysym - A2600_KEYSYM_KP_0);
    if (keysym >= 0x21 && keysym <= 0x7e) {
        char c = (char)keysym;
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        return snprintf(dst, (size_t)dstsz, "%c", c);
    }
    if (keysym >= A2600SESSION_KEYSYM_HID_BASE
        && keysym <= A2600SESSION_KEYSYM_HID_BASE + A2600SESSION_HID_USAGE_MAX) {
        const char *hid = a2600_hid_usage_name(keysym - A2600SESSION_KEYSYM_HID_BASE);
        if (hid) return snprintf(dst, (size_t)dstsz, "%s", hid);
        return snprintf(dst, (size_t)dstsz, "HID 0x%02X",
                        (unsigned)(keysym - A2600SESSION_KEYSYM_HID_BASE));
    }
    return snprintf(dst, (size_t)dstsz, "Key 0x%X", keysym);
}

/* ---- the keyboard translator --------------------------------------------- */

int a2600session_key_sysaction(a2600session *s, uint32_t keysym)
{
    int t = a2600session_target_for_key(s, keysym);
    if (t < A2600_TARGET_SYSACT(0)) return -1;
    return t - A2600_TARGET_SYSACT(0);
}

int a2600session_key(a2600session *s, uint32_t keysym, int down)
{
    uint32_t folded = fold(keysym);
    int t;

    if (!s) return 0;
    if (down) {
        int hit = 0;
        /* every target the key drives (a port's joystick, paddle and
         * driving fire share one key by default); a system action is the
         * frontend's to fire and is not a machine input */
        pthread_mutex_lock(&s_lock);
        ensure_seeded_locked();
        for (t = 0; t < A2600_TARGET_SYSACT(0); t++) {
            if (s_table[t].keysym != folded) continue;
            hit = 1;
            if (s->held_keysym[t] == folded) continue;  /* auto-repeat */
            s->held_keysym[t] = folded;
            pthread_mutex_unlock(&s_lock);
            a2600session_press(s, t, 1);
            pthread_mutex_lock(&s_lock);
        }
        pthread_mutex_unlock(&s_lock);
        return hit;
    }
    {
        int hit = 0;
        /* remembered by the key, so a release clears exactly what its press
         * asserted even if the binding changed in between */
        for (t = 0; t < A2600_TARGET_COUNT; t++) {
            if (s->held_keysym[t] == folded) {
                s->held_keysym[t] = 0;
                a2600session_press(s, t, 0);
                hit = 1;
            }
        }
        return hit;
    }
}

void a2600session_release_all(a2600session *s)
{
    int t;
    if (!s) return;
    for (t = 0; t < A2600_TARGET_COUNT; t++) {
        if (s->held_keysym[t]) {
            s->held_keysym[t] = 0;
            a2600session_press(s, t, 0);
        }
    }
}
