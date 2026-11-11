/*
 * a26_test -- a cartridge named .a26 (most 2600 dumps are) loads and runs:
 * the same image as data/bbasic/main.bas.bin, copied to game.a26.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>
#include <time.h>

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

static int copy(const char *from, const char *to)
{
    char buf[8192];
    size_t n;
    FILE *in = fopen(from, "rb"), *out = fopen(to, "wb");
    if (!in || !out) { if (in) fclose(in); if (out) fclose(out); return -1; }
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) fwrite(buf, 1, n, out);
    fclose(in);
    fclose(out);
    return 0;
}

static void try_ext(a2600session *s, a2600debug *d, const char *dir, const char *ext)
{
    char src[1024], dst[1024], info[256], what[160];
    a2600debug_cpu cpu;

    snprintf(src, sizeof src, "%s/main.bas.bin", BBASIC_FIXTURE_DIR);
    snprintf(dst, sizeof dst, "%s/game.%s", dir, ext);
    check(copy(src, dst) == 0, "copied the image");
    snprintf(what, sizeof what, "game.%s loads", ext);
    check(a2600session_load_cart(s, dst) == 0, what);
    if (failures) printf("  error: %s\n", a2600session_last_error(s));
    sleep_ms(300);
    check(strstr(a2600session_cart_path(s), ext) != NULL, "the session remembers the path");
    a2600debug_stop(d);
    a2600debug_cart_info(d, info, sizeof info);
    printf("  cart: %s\n", info);
    a2600debug_cpu_get(d, &cpu);
    snprintf(what, sizeof what, "game.%s runs the cartridge (pc $%04X is the image's, not CONFIG's)", ext, cpu.pc);
    check(cpu.pc >= 0xF000 && cpu.pc < 0xF480 && a2600debug_bank_count(d) <= 1, what);
    a2600debug_resume(d);
}

int main(int argc, char **argv)
{
    char cfg[512], data[512], dir[512];
    a2600session_paths p;
    a2600session *s;
    a2600session_start_opts o;
    a2600debug *d;

    test_tmpdir(cfg, sizeof cfg, "acfg");
    test_tmpdir(data, sizeof data, "adata");
    test_tmpdir(dir, sizeof dir, "acart");
    memset(&p, 0, sizeof p);
    p.config_dir = cfg; p.data_dir = data; p.fujinet_lib = "";
    s = a2600session_new(&p);
    a2600session_default_opts(s, &o);
    o.enable_fujinet = 0; o.enable_audio = 0; o.enable_gamepad = 0;
    if (a2600session_start(s, &o) != 0) { printf("start: %s\n", a2600session_last_error(s)); return 1; }
    sleep_ms(300);
    d = a2600session_debugger(s);

    try_ext(s, d, dir, "bin");
    try_ext(s, d, dir, "a26");

    /* any further arguments: real ROMs to try (a development aid) */
    for (int k = 1; k < argc; k++) {
        char info[256];
        a2600debug_cpu cpu;
        int rc = a2600session_load_cart(s, argv[k]);
        sleep_ms(500);
        a2600debug_stop(d);
        a2600debug_cart_info(d, info, sizeof info);
        a2600debug_cpu_get(d, &cpu);
        printf("%s: rc=%d (%s) cart=%s pc=$%04X\n", argv[k], rc, rc ? a2600session_last_error(s) : "", info, cpu.pc);
        a2600debug_resume(d);
    }

    a2600session_stop(s);
    a2600session_free(s);
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
