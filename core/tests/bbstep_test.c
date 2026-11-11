/*
 * bbstep_test -- BASIC line stepping on real batari Basic programs: the
 * cartridges in data/bbasic booted in the machine, stepped by line.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "a2600debug.h"
#include "a2600session.h"
#include "a2600srcmap.h"
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

enum { RUN, INTO, OVER, OUT };

/* Do `how` and wait for the new stop (the stop serial rises); then the BASIC
 * line the machine is on, or -1 if it is not on a mapped line (or never stopped). */
static int go(a2600debug *d, int how)
{
    uint64_t serial = a2600debug_stop_serial(d);
    int file = 0, line = 0, i;

    switch (how) {
    case RUN:  a2600debug_resume(d); break;
    case INTO: a2600debug_step_line(d); break;
    case OVER: a2600debug_step_line_over(d); break;
    default:   a2600debug_step_line_out(d); break;
    }
    for (i = 0; i < 400 && !(a2600debug_stop_serial(d) > serial && a2600debug_is_stopped(d)); i++) sleep_ms(10);
    if (!(a2600debug_stop_serial(d) > serial && a2600debug_is_stopped(d))) return -1;
    return a2600debug_line_at_pc(d, &file, &line) ? line : -1;
}

static int find_file(const char *name)
{
    a2600srcmap *m = a2600srcmap_shared();
    int i, n = a2600srcmap_file_count(m);
    for (i = 0; i < n; i++) {
        const char *f = a2600srcmap_file_name(m, i);
        size_t l = strlen(f), k = strlen(name);
        if (l >= k && strcmp(f + l - k, name) == 0) return i;
    }
    return -1;
}

static void boot(a2600session *s, a2600debug *d, const char *prog)
{
    char path[1024];
    a2600srcmap_import_result r;

    snprintf(path, sizeof path, "%s/%s.bas", BBASIC_FIXTURE_DIR, prog);
    check(a2600srcmap_import(a2600srcmap_shared(), path, &r) == 0 && r.mapped > 5, "the source and its listing import");
    snprintf(path, sizeof path, "%s/%s.bas.bin", BBASIC_FIXTURE_DIR, prog);
    check(a2600session_load_cart(s, path) == 0, "the cartridge loads");
    sleep_ms(300);
    a2600debug_stop(d);
    check(a2600debug_is_stopped(d), "stopped");
}

/* Run to a breakpoint on `line` of the file. */
static int run_to_line(a2600debug *d, int file, int line)
{
    int got;
    check(a2600debug_line_break_toggle(d, file, line) == 1, "a breakpoint on the line");
    check(a2600debug_line_break_state(d, file, line) == 1, "and the line shows it");
    got = go(d, RUN);
    a2600debug_line_break_toggle(d, file, line);
    check(a2600debug_line_break_state(d, file, line) == 0, "removed again");
    return got;
}

static void main_bas(a2600session *s, a2600debug *d)
{
    char reason[256];
    int file, address;

    boot(s, d, "main");
    file = find_file("main.bas");
    check(file >= 0, "main.bas is loaded");
    check(a2600debug_line_break_toggle(d, file, 12) == -1, "a label line has no code to break on");

    check(run_to_line(d, file, 6) == 6, "runs to line 6 (COLUBK)");
    check(go(d, INTO) == 7, "step: line 7");
    a2600debug_stop_reason(d, reason, sizeof reason, &address);
    check(strstr(reason, "BASIC line main.bas:7") != NULL, "the stop says which line");
    check(go(d, INTO) == 8, "step: line 8 (gosub)");
    check(go(d, INTO) == 13, "step into the gosub: line 13");
    check(go(d, INTO) == 14, "step: line 14 (return)");
    check(go(d, INTO) == 9, "step: back in the caller, line 9 (drawscreen)");
    check(go(d, OVER) == 10, "over drawscreen (a whole frame): line 10");
    check(go(d, OVER) == 6, "over the goto: line 6");
    check(go(d, OVER) == 7, "over: line 7");
    check(go(d, OVER) == 8, "over: line 8");
    check(go(d, OVER) == 9, "over the gosub: line 9, not 13");
    check(go(d, INTO) == 10, "into: line 10");
    check(go(d, INTO) == 6, "into: line 6");
    check(go(d, INTO) == 7, "into: line 7");
    check(go(d, INTO) == 8, "into: line 8");
    check(go(d, INTO) == 13, "into the gosub again: line 13");
    check(go(d, OUT) == 9, "out of the routine: line 9");

    /* a breakpoint in the middle of an over wins */
    check(go(d, INTO) == 10, "line 10");
    check(go(d, INTO) == 6, "line 6");
    check(go(d, INTO) == 7, "line 7");
    check(go(d, INTO) == 8, "line 8 again");
    check(a2600debug_line_break_toggle(d, file, 13) == 1, "a breakpoint inside bump");
    check(go(d, OVER) == 13, "over the gosub stops at the breakpoint inside it");
    a2600debug_stop_reason(d, reason, sizeof reason, &address);
    check(strstr(reason, "BASIC line") == NULL, "and the reason is the breakpoint, not the step");
    a2600debug_line_break_toggle(d, file, 13);
}

static void banked_bas(a2600session *s, a2600debug *d)
{
    int file;

    boot(s, d, "banked");
    file = find_file("banked.bas");
    check(file >= 0, "banked.bas is loaded");

    check(run_to_line(d, file, 7) == 7, "runs to line 7 (gosub bump bank2)");
    check(go(d, INTO) == 13, "step across the bank switch: line 13 in bank 2");
    check(go(d, INTO) == 14, "step: line 14");
    check(go(d, INTO) == 15, "step: line 15 (return thisbank)");
    check(go(d, INTO) == 8, "step: back in bank 1, line 8");
    check(go(d, INTO) == 9, "step: line 9");
    check(go(d, INTO) == 6, "step: line 6");
    check(go(d, INTO) == 7, "step: line 7");
    check(go(d, OVER) == 8, "over the bank-switched gosub: line 8");
    check(go(d, INTO) == 9, "line 9");
    check(go(d, INTO) == 6, "line 6");
    check(go(d, INTO) == 7, "line 7");
    check(go(d, INTO) == 13, "into again: line 13");
    check(go(d, OUT) == 8, "out of the bank-2 routine: line 8");
}

int main(void)
{
    char cfg[512], data[512];
    a2600session_paths p;
    a2600session *s;
    a2600session_start_opts o;
    a2600debug *d;

    test_tmpdir(cfg, sizeof cfg, "bcfg");
    test_tmpdir(data, sizeof data, "bdata");
    memset(&p, 0, sizeof p);
    p.config_dir = cfg; p.data_dir = data; p.fujinet_lib = "";
    s = a2600session_new(&p);
    a2600session_default_opts(s, &o);
    o.enable_fujinet = 0; o.enable_audio = 0; o.enable_gamepad = 0;
    if (a2600session_start(s, &o) != 0) { printf("start: %s\n", a2600session_last_error(s)); return 1; }
    sleep_ms(300);
    d = a2600session_debugger(s);
    check(d != NULL, "debugger handle");

    main_bas(s, d);
    banked_bas(s, d);

    a2600session_stop(s);
    a2600session_free(s);
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
