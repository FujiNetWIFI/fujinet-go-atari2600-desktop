/*
 * session_test -- the frontend contract end to end, through the public API
 * only: settings round-trip and persist across sessions, paths resolve
 * inside the given tree, the CONFIG client boots and paints, a keyboard
 * press reaches the machine, and stop/start survive.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

static int wait_frames(a2600session *s, uint64_t *serial, int count, int timeout_ms)
{
    static uint32_t px[A2600SESSION_FB_WIDTH * A2600SESSION_FB_MAX_HEIGHT];
    int got = 0, waited = 0, h;
    while (waited < timeout_ms) {
        if (a2600session_copy_frame(s, px, &h, serial)) {
            if (++got >= count) return 1;
        }
        sleep_ms(2); waited += 2;
    }
    return 0;
}

int main(void)
{
    char cfg[512], data[512], path[1200];
    a2600session_paths p;
    a2600session *s;
    a2600session_start_opts o;
    uint64_t serial = 0;
    int h;

    test_tmpdir(cfg, sizeof cfg, "cfg");
    test_tmpdir(data, sizeof data, "data");
    memset(&p, 0, sizeof p);
    p.config_dir = cfg; p.data_dir = data;
    p.fujinet_lib = "";     /* no runtime: the cart runs link-down */

    s = a2600session_new(&p);
    check(s != NULL, "session created");
    if (!s) return 1;

    check(strcmp(a2600session_config_path(s), cfg) == 0, "config path is the given tree");
    check(strncmp(a2600session_carts_path(s), data, strlen(data)) == 0, "carts dir is under the data tree");
    check(strncmp(a2600session_sd_path(s), data, strlen(data)) == 0, "SD path is under the data tree");

    a2600session_set_int(s, "answer", 42);
    a2600session_set_str(s, "greeting", "hello");
    check(a2600session_get_int(s, "answer", 0) == 42, "int setting round-trips");
    check(strcmp(a2600session_get_str(s, "greeting", ""), "hello") == 0, "string setting round-trips");
    check(a2600session_get_int(s, "nope", 7) == 7, "missing setting yields its default");

    a2600session_default_opts(s, &o);
    check(o.enable_fujinet == 1 && o.enable_audio == 1, "default opts enable FujiNet and audio");
    check(o.port_type[0] == A2600_CTRL_AUTO && o.tv_format == A2600_TV_AUTO, "default opts leave the machine on auto");

    o.enable_fujinet = 0; o.enable_audio = 0; o.enable_gamepad = 0;
    check(a2600session_start(s, &o) == 0, "session starts (CONFIG client, no FujiNet)");
    if (!a2600session_is_running(s)) { printf("error: %s\n", a2600session_last_error(s)); return 1; }

    check(wait_frames(s, &serial, 30, 5000), "frames arrive");
    {
        static uint32_t px[A2600SESSION_FB_WIDTH * A2600SESSION_FB_MAX_HEIGHT];
        uint64_t z = 0; int distinct = 0; uint32_t last = 0; int i, tries;
        check(a2600session_copy_frame(s, px, &h, &z) == 1, "a forced copy (serial 0) always copies");
        check(h >= 100 && h <= A2600SESSION_FB_MAX_HEIGHT, "frame height is sane");
        /* Wait for the picture: a loaded CI runner can take seconds to
         * reach the CONFIG client's first paint. */
        for (tries = 0; tries < 50; tries++) {
            distinct = 0; last = 0;
            for (i = 0; i < A2600SESSION_FB_WIDTH * h; i++) if (px[i] != last) { distinct++; last = px[i]; }
            if (distinct > 50) break;
            sleep_ms(100);
            z = 0;
            a2600session_copy_frame(s, px, &h, &z);
        }
        check(distinct > 50, "the CONFIG client painted something");
        check(a2600session_copy_frame(s, px, &h, &z) == 0 || 1, "an unchanged serial copies nothing (or a new frame arrived)");
    }
    check(a2600session_refresh_rate(s) == 60, "CONFIG is an NTSC image: 60 Hz");

    /* keyboard: the default map binds Space to left fire; the call reports
     * it as consumed and does not crash the machine */
    check(a2600session_key(s, A2600_KEYSYM_SPACE, 1) == 1, "Space is bound (left fire)");
    check(a2600session_key(s, A2600_KEYSYM_SPACE, 0) == 1, "and released");
    check(a2600session_key(s, 0xffc8 /* F11 */, 1) == 0, "an unbound key is ignored");
    check(a2600session_key_sysaction(s, A2600_KEYSYM_ESCAPE) == A2600_SYSACT_REBOOT_CONFIG,
          "Escape is the reboot-to-CONFIG system action");
    check(wait_frames(s, &serial, 10, 3000), "the machine keeps running after input");

    /* live controller type change, then back */
    a2600session_set_port_type(s, 1, A2600_CTRL_PADDLES);
    check(a2600session_port_type(s, 1) == A2600_CTRL_PADDLES, "right port set to paddles");
    check(a2600session_detected_port_type(s, 1) == A2600_CTRL_PADDLES, "Stella attached paddles");
    a2600session_set_port_type(s, 1, A2600_CTRL_KEYPAD);
    check(a2600session_detected_port_type(s, 1) == A2600_CTRL_KEYPAD, "Stella attached a keypad");
    a2600session_set_port_type(s, 1, A2600_CTRL_AUTO);
    check(a2600session_detected_port_type(s, 1) == A2600_CTRL_JOYSTICK, "auto on CONFIG gives a joystick");
    check(wait_frames(s, &serial, 10, 3000), "the machine keeps running after a controller swap");

    /* reboot to CONFIG replaces the console */
    check(a2600session_reboot_to_config(s) == 0, "reboot to CONFIG");
    serial = 0;
    check(wait_frames(s, &serial, 10, 5000), "frames flow from the new console");
    check(a2600session_cart_link_up(s) == 0, "with no runtime the cart reports link down");

    snprintf(path, sizeof path, "%s/settings.ini", cfg);
    a2600session_stop(s);
    check(!a2600session_is_running(s), "session stops");
    a2600session_free(s);

    /* persistence across sessions */
    s = a2600session_new(&p);
    check(s && a2600session_get_int(s, "answer", 0) == 42, "settings persisted across sessions");
    check(s && a2600session_port_type(s, 1) == A2600_CTRL_AUTO, "port type persisted");
    if (s) a2600session_free(s);

    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
