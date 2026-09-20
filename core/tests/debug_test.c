/*
 * debug_test -- the debugger contract against the running CONFIG client.
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

int main(void)
{
    char cfg[512], data[512];
    static char out[65536];   /* 'help' alone is 8 KB */
    a2600session_paths p;
    a2600session *s;
    a2600session_start_opts o;
    a2600debug *d;
    a2600debug_cpu cpu, cpu2;
    a2600debug_riot riot;
    a2600debug_tia tia;
    static a2600debug_line lines[64];
    static uint32_t px[A2600SESSION_FB_WIDTH * A2600SESSION_FB_MAX_HEIGHT];
    uint8_t ram[128];
    uint32_t bps[8];
    int n, pc_line, h, total, first;

    test_tmpdir(cfg, sizeof cfg, "dcfg");
    test_tmpdir(data, sizeof data, "ddata");
    memset(&p, 0, sizeof p);
    p.config_dir = cfg; p.data_dir = data; p.fujinet_lib = "";
    s = a2600session_new(&p);
    a2600session_default_opts(s, &o);
    o.enable_fujinet = 0; o.enable_audio = 0; o.enable_gamepad = 0;
    if (a2600session_start(s, &o) != 0) { printf("start: %s\n", a2600session_last_error(s)); return 1; }
    sleep_ms(300);

    d = a2600session_debugger(s);
    check(d != NULL, "debugger handle");
    check(!a2600debug_is_stopped(d), "running at first");

    a2600debug_stop(d);
    check(a2600debug_is_stopped(d), "stop() stops the machine");
    a2600debug_cpu_get(d, &cpu);
    printf("pc=%04x sp=%02x a=%02x x=%02x y=%02x cycles=%d\n", cpu.pc, cpu.sp, cpu.a, cpu.x, cpu.y, cpu.cycles);
    check(cpu.pc >= 0x1000 && cpu.pc <= 0x1fff, "PC is in cartridge space");

    a2600debug_step(d);
    a2600debug_cpu_get(d, &cpu2);
    check(cpu2.pc != cpu.pc, "step advances PC");

    n = a2600debug_disassemble(d, -1, 0, lines, 64, &total, &pc_line);
    printf("disassembly: %d of %d lines, pc at line %d\n", n, total, pc_line);
    check(n == 64 && total > 64, "disassembly lists a whole bank, windowed");
    check(pc_line >= 0 && pc_line < total, "the PC's absolute line is reported");
    first = pc_line > 8 ? pc_line - 8 : 0;
    n = a2600debug_disassemble(d, -1, first, lines, 64, &total, &pc_line);
    check(pc_line - first >= 0 && pc_line - first < n
          && lines[pc_line - first].is_pc && lines[pc_line - first].address == cpu2.pc,
          "a window around the PC has the PC line marked and matching");
    check(lines[pc_line - first].disasm[0] != '\0', "the PC line has a mnemonic");
    /* Stella counts the FujiNet cart's SELECTABLE banks: the 16 KB image is
     * eight 2 KB halves, of which the fixed upper half is not one */
    check(a2600debug_bank_count(d) == 7, "the CONFIG ROM has 7 selectable banks");
    /* Stella reports the fixed upper half as bank 7, past the selectable ones */
    check(a2600debug_current_bank(d) >= 0 && a2600debug_current_bank(d) <= 7, "the current bank is in range");

    /* breakpoints */
    check(a2600debug_breakpoint_toggle(d, (uint16_t)cpu2.pc, a2600debug_current_bank(d)) == 1, "toggling sets a breakpoint");
    check(a2600debug_breakpoint_check(d, (uint16_t)cpu2.pc, a2600debug_current_bank(d)) == 1, "and it reads back");
    n = a2600debug_breakpoint_list(d, bps, 8);
    check(n == 1 && (bps[0] & 0xffff) == (uint32_t)cpu2.pc, "the list has it");
    n = a2600debug_disassemble(d, -1, first, lines, 64, &total, &pc_line);
    check(pc_line - first >= 0 && lines[pc_line - first].has_breakpoint, "the disassembly marks it");
    a2600debug_breakpoint_clear(d);
    check(a2600debug_breakpoint_list(d, bps, 8) == 0, "clear empties the list");

    /* the prompt */
    n = a2600debug_command(d, "listBreaks", out, sizeof out);
    check(n > 0 && strstr(out, "no breakpoints") != NULL, "a prompt command returns its text");
    n = a2600debug_command(d, "help", out, sizeof out);
    check(strstr(out, "breakIf") != NULL && strstr(out, "trapWrite") != NULL, "'help' lists the parser's commands");
    n = a2600debug_command(d, "exitRom", out, sizeof out);
    check(strstr(out, "not available") != NULL, "exitRom is refused");
    n = a2600debug_completions(d, "trap", out, sizeof out);
    check(n >= 3 && strstr(out, "trapRead") != NULL, "completions for 'trap'");

    /* registers editable */
    a2600debug_cpu_set(d, A2600_REG_A, 0x5a);
    a2600debug_cpu_get(d, &cpu);
    check(cpu.a == 0x5a, "A is editable");
    a2600debug_cpu_set(d, A2600_FLAG_C, 1);
    a2600debug_cpu_get(d, &cpu);
    check(cpu.c == 1, "the carry flag is editable");

    /* RIOT, TIA, RAM */
    a2600debug_riot_get(d, &riot);
    check(riot.swchb != 0 || riot.swcha != 0 || riot.tim_divider > 0, "RIOT state reads");
    a2600debug_tia_get(d, &tia);
    check(tia.scanlines >= 0 && tia.frame_count >= 0, "TIA state reads");
    check(a2600debug_tia_set(d, "colubk", 0x44) == 0, "a TIA register is settable");
    a2600debug_tia_get(d, &tia);
    check(tia.colubk == 0x44, "and reads back");
    check(a2600debug_tia_set(d, "nosuch", 1) == -1, "an unknown register is refused");
    check(a2600debug_tia_color(d, 0x0e) != 0, "a TIA colour maps to RGB");
    a2600debug_ram_get(d, ram);
    a2600debug_write(d, 0x80, 0xa5);
    a2600debug_ram_get(d, ram);
    check(ram[0] == 0xa5, "zero-page RAM is editable");
    h = a2600debug_frame_snapshot(d, px, 0);
    check(h >= 100, "a frame snapshot has lines");
    h = a2600debug_frame_snapshot(d, px, 1);
    check(h >= 100, "a partial frame snapshot has lines");

    /* frame / scanline stepping keep the machine stopped */
    a2600debug_frame(d, 1);
    check(a2600debug_is_stopped(d), "frame+1 leaves it stopped");
    a2600debug_scanline(d, 1);
    check(a2600debug_is_stopped(d), "scanline+1 leaves it stopped");
    a2600debug_trace(d);
    check(a2600debug_is_stopped(d), "trace leaves it stopped");
    a2600debug_cart_info(d, out, sizeof out);
    printf("cart: %s\n", out);
    check(strstr(out, "FUJI") != NULL, "cart info names the FujiNet scheme");

    a2600debug_resume(d);
    check(!a2600debug_is_stopped(d), "resume runs the machine");
    sleep_ms(200);
    check(!a2600debug_is_stopped(d), "and it stays running");

    a2600session_stop(s);
    a2600session_free(s);
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
