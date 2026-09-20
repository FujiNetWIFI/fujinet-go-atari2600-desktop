/*
 * bindings_test -- the remappable table and the keyboard translator, pure:
 * defaults, steal-and-describe, persistence through the settings store, and
 * names. No machine is started.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

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

int main(void)
{
    char cfg[512], data[512], stolen[64], name[64];
    a2600session_paths p;
    a2600session *s;
    a2600_binding b;

    test_tmpdir(cfg, sizeof cfg, "bcfg");
    test_tmpdir(data, sizeof data, "bdata");
    memset(&p, 0, sizeof p);
    p.config_dir = cfg; p.data_dir = data; p.fujinet_lib = "";
    s = a2600session_new(&p);
    if (!s) return 1;

    /* defaults */
    b = a2600session_binding_get(s, A2600_TARGET_PORT(0, A2600_ACT_JOY_FIRE));
    check(b.keysym == A2600_KEYSYM_SPACE && b.button == A2600_PAD_BTN_SOUTH, "left fire defaults to Space / A");
    b = a2600session_binding_get(s, A2600_TARGET_PORT(1, A2600_ACT_KEY_1));
    check(b.keysym == '8', "right keypad 1 defaults to 8");
    b = a2600session_binding_get(s, A2600_TARGET_SWITCH(A2600_SW_RESET));
    check(b.keysym == A2600_KEYSYM_F2 && b.button == A2600_PAD_BTN_START, "Reset defaults to F2 / Start");
    check(a2600session_target_for_key(s, 'Q') == A2600_TARGET_PORT(0, A2600_ACT_KEY_4), "lookup folds case");
    check(a2600session_target_for_key(s, A2600_KEYSYM_KP_2) == A2600_TARGET_PORT(0, A2600_ACT_KEY_2), "keypad digits fold onto the digits");
    check(a2600session_target_for_button(s, 1, A2600_PAD_BTN_SOUTH) == A2600_TARGET_PORT(1, A2600_ACT_JOY_FIRE), "button lookup is scoped to the port");
    check(a2600session_target_for_button(s, 1, A2600_PAD_BTN_START) == A2600_TARGET_SWITCH(A2600_SW_RESET), "and falls through to the machine-wide switches");

    /* steal */
    a2600session_binding_set_key(s, A2600_TARGET_PORT(1, A2600_ACT_JOY_FIRE), A2600_KEYSYM_SPACE, stolen, sizeof stolen);
    check(strstr(stolen, "Left Joystick Fire") != NULL && strstr(stolen, "Left Paddle A Fire") != NULL
          && strstr(stolen, "Left Driving Fire") != NULL, "rebinding Space reports every holder it displaced");
    b = a2600session_binding_get(s, A2600_TARGET_PORT(0, A2600_ACT_JOY_FIRE));
    check(b.keysym == 0, "the old holder lost the key");
    check(a2600session_target_for_key(s, A2600_KEYSYM_SPACE) == A2600_TARGET_PORT(1, A2600_ACT_JOY_FIRE), "the new holder has it");

    /* names */
    a2600session_keysym_name(A2600_KEYSYM_F12, name, sizeof name);
    check(strcmp(name, "F12") == 0, "F12 names itself");
    a2600session_keysym_name('a', name, sizeof name);
    check(strcmp(name, "A") == 0, "letters name upper-case");
    check(strcmp(a2600_pad_button_name(A2600_PAD_BTN_DPAD_LEFT), "D-pad Left") == 0, "pad button names");
    check(strcmp(a2600_target_name(A2600_TARGET_SYSACT(A2600_SYSACT_REBOOT_CONFIG)), "Reboot to CONFIG") == 0, "system action names");
    check(strcmp(a2600_target_short_name(A2600_TARGET_PORT(0, A2600_ACT_KEY_POUND)), "#") == 0, "short keypad names");

    /* native key codes: evdev KEY_1 is 2, Windows scancode 0x02 is '1', macOS 0x12 is '1' */
    check(a2600session_keysym_from_evdev(2) == '1', "evdev KEY_1 -> '1'");
    check(a2600session_keysym_from_win_scancode(0x02, 0) == '1', "Windows scancode 02 -> '1'");
    check(a2600session_keysym_from_macos_keycode(0x12) == '1', "macOS keycode 0x12 -> '1'");
    check(a2600session_keysym_from_evdev(103) == A2600_KEYSYM_UP, "evdev KEY_UP -> Up");
    check(a2600session_keysym_from_win_scancode(0x48, 1) == A2600_KEYSYM_UP, "Windows E0 48 -> Up");
    check(a2600session_keysym_from_macos_keycode(0x7E) == A2600_KEYSYM_UP, "macOS 0x7E -> Up");

    /* persistence */
    a2600session_free(s);
    s = a2600session_new(&p);
    check(s && a2600session_target_for_key(s, A2600_KEYSYM_SPACE) == A2600_TARGET_PORT(1, A2600_ACT_JOY_FIRE), "the rebinding persisted");
    a2600session_bindings_reset(s);
    check(a2600session_target_for_key(s, A2600_KEYSYM_SPACE) == A2600_TARGET_PORT(0, A2600_ACT_JOY_FIRE), "reset restores the defaults");
    a2600session_free(s);

    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
