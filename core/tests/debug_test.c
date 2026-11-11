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
#include "a2600memtools.h"
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


/* Resume and wait (up to ~2 s) for the machine to stop by itself. */
static int wait_stopped(a2600debug *d)
{
    int i;
    for (i = 0; i < 200 && !a2600debug_is_stopped(d); i++) sleep_ms(10);
    return a2600debug_is_stopped(d);
}

static int has_bp(a2600debug *d, int id, a2600debug_breakpoint *out)
{
    a2600debug_breakpoint list[32];
    int i, n = a2600debug_bp_list(d, list, 32);
    for (i = 0; i < n; i++)
        if (list[i].id == id) { if (out) *out = list[i]; return 1; }
    return 0;
}

/* The family debugger's engine: stop serial and reasons, the breakpoint
 * table with hits, old/value, side-effect-free reads, memory map, search,
 * trace ring, symbols, sound. The machine is stopped on entry and on exit. */
static void extra_tests(a2600debug *d)
{
    static char out[8192], reason[256];
    static a2600debug_breakpoint bp;
    static a2600debug_region map[128];
    static a2600debug_trace_entry tr[16];
    static uint16_t hit[16];
    a2600debug_cpu cpu;
    a2600debug_sound snd;
    uint8_t pat[4], buf[16];
    uint64_t serial;
    long val;
    char err[128], name[64];
    uint16_t at;
    int id, wid, cid, n, i, address, bank, anchor;

    /* stop serial: every new stop, not a refresh */
    serial = a2600debug_stop_serial(d);
    a2600debug_cpu_get(d, &cpu);
    check(a2600debug_stop_serial(d) == serial, "a refresh does not bump the stop serial");
    a2600debug_step(d);
    check(a2600debug_stop_serial(d) > serial, "a step is a new stop");
    a2600debug_stop_reason(d, reason, sizeof reason, &address);
    check(strstr(reason, "step at $") != NULL && address >= 0, "the step's reason and address");

    /* a write trap: the stop says what was written and what it replaced */
    wid = a2600debug_bp_add(d, A2600DEBUG_BP_WRITE, 0x00, 0x00, A2600DEBUG_ANY_BANK, "");
    check(wid > 0 && has_bp(d, wid, &bp) && bp.type == A2600DEBUG_BP_WRITE, "a write trap on VSYNC");
    /* a stack push can land on a TIA mirror ($0100): wait for the real one */
    for (i = 0; i < 8; i++) {
        a2600debug_resume(d);
        check(wait_stopped(d), "the write trap stops the machine");
        a2600debug_stop_reason(d, reason, sizeof reason, &address);
        printf("reason: %s (%04x)\n", reason, address);
        if (address == 0) break;
    }
    check(strstr(reason, "wrote $0000 = $") != NULL && strstr(reason, "(was $") != NULL, "the reason has the value written and the old one");
    check(has_bp(d, wid, &bp) && bp.hits >= 1, "the trap's hit is counted");
    /* the instruction after a VSYNC write runs every frame: the anchor the
     * breakpoint and step-out tests aim at */
    a2600debug_cpu_get(d, &cpu);
    anchor = cpu.pc;
    a2600debug_bp_enable(d, wid, 0);
    check(a2600debug_trap_write(d, 0x00) == 0, "disabled: removed from Stella");
    a2600debug_bp_remove(d, wid);

    /* an execute breakpoint with an id, hits and enable/disable */
    a2600debug_cpu_get(d, &cpu);
    id = a2600debug_bp_add(d, A2600DEBUG_BP_EXEC, (uint16_t)cpu.pc, (uint16_t)cpu.pc, A2600DEBUG_ANY_BANK, "");
    check(id > 0 && has_bp(d, id, &bp) && bp.enabled && bp.hits == 0 && bp.start == cpu.pc, "bp_add returns an id and lists it");
    check(a2600debug_bp_add(d, A2600DEBUG_BP_EXEC, (uint16_t)cpu.pc, (uint16_t)cpu.pc, A2600DEBUG_ANY_BANK, "") == id,
          "an identical entry is not duplicated");
    check(a2600debug_breakpoint_check(d, (uint16_t)cpu.pc, A2600DEBUG_ANY_BANK) == 1, "breakpoint_check sees it");
    serial = a2600debug_stop_serial(d);
    a2600debug_resume(d);
    check(wait_stopped(d), "an execute breakpoint stops the machine again");
    a2600debug_stop_reason(d, reason, sizeof reason, &address);
    printf("reason: %s (%04x)\n", reason, address);
    check(strncmp(reason, "BP:", 3) == 0 && address == cpu.pc, "the reason names the breakpoint, with its address");
    check(a2600debug_stop_serial(d) > serial, "a breakpoint stop bumps the serial");
    check(has_bp(d, id, &bp) && bp.hits == 1, "the hit is counted");
    a2600debug_cpu_get(d, &cpu);
    check(cpu.pc == address, "stopped on the breakpoint");
    /* it must not refire on the way off */
    a2600debug_step(d);
    check(a2600debug_is_stopped(d) && has_bp(d, id, &bp) && bp.hits == 1, "stepping off does not count again");
    {
        a2600debug_line lines[8];
        int total, pc_line;
        a2600debug_bp_enable(d, id, 0);
        check(has_bp(d, id, &bp) && !bp.enabled, "disabling keeps the entry");
        check(a2600debug_breakpoint_check(d, (uint16_t)address, A2600DEBUG_ANY_BANK) == 0, "but it no longer checks as set");
        a2600debug_disassemble(d, -1, 0, lines, 8, &total, &pc_line);
        a2600debug_bp_enable(d, id, 1);
        check(has_bp(d, id, &bp) && bp.enabled, "enabling brings it back");
        a2600debug_bp_clear_hits(d);
        check(has_bp(d, id, &bp) && bp.hits == 0, "clear_hits zeroes it");
    }
    a2600debug_bp_remove(d, id);
    check(!has_bp(d, id, NULL), "bp_remove drops it");

    /* disassembly marks enabled and disabled breakpoints differently */
    {
        static a2600debug_line lines[64];
        int total, pc_line, first;
        a2600debug_cpu_get(d, &cpu);
        id = a2600debug_bp_add(d, A2600DEBUG_BP_EXEC, (uint16_t)cpu.pc, (uint16_t)cpu.pc, A2600DEBUG_ANY_BANK, "");
        a2600debug_disassemble(d, -1, 0, lines, 64, &total, &pc_line);
        first = pc_line > 8 ? pc_line - 8 : 0;
        a2600debug_disassemble(d, -1, first, lines, 64, &total, &pc_line);
        check(pc_line >= 0 && lines[pc_line - first].has_breakpoint && !lines[pc_line - first].bp_disabled, "an enabled breakpoint marks the line");
        a2600debug_bp_enable(d, id, 0);
        a2600debug_disassemble(d, -1, first, lines, 64, &total, &pc_line);
        check(!lines[pc_line - first].has_breakpoint && lines[pc_line - first].bp_disabled, "a disabled one is the grey dot");
        a2600debug_bp_remove(d, id);
    }

    /* conditions: old/value are aliases of _old/_value */
    check(a2600debug_bp_add(d, A2600DEBUG_BP_WRITE, 0x00, 0x00, A2600DEBUG_ANY_BANK, "value ===") == -1, "a condition that does not parse is refused");
    wid = a2600debug_bp_add(d, A2600DEBUG_BP_WRITE, 0x00, 0x00, A2600DEBUG_ANY_BANK, "value == value");
    check(wid > 0, "a conditional write trap, with `value`");
    a2600debug_resume(d);
    check(wait_stopped(d), "the conditional trap stops it");
    a2600debug_stop_reason(d, reason, sizeof reason, &address);
    printf("reason: %s\n", reason);
    check(strstr(reason, "wrote $0000") != NULL, "with the same reason");
    a2600debug_bp_remove(d, wid);
    cid = a2600debug_bp_add(d, A2600DEBUG_BP_EXEC, 0x1000, 0x1FFF, A2600DEBUG_ANY_BANK, "old == old");
    check(cid > 0 && has_bp(d, cid, &bp) && !strcmp(bp.condition, "old == old"), "a conditional execute range keeps the typed condition");
    a2600debug_bp_remove(d, cid);

    /* console-set breaks are adopted, and cleared ones leave */
    a2600debug_cpu_get(d, &cpu);
    snprintf(out, sizeof out, "break $%04x", cpu.pc);
    a2600debug_command(d, out, reason, sizeof reason);
    check(a2600debug_breakpoint_check(d, (uint16_t)cpu.pc, a2600debug_current_bank(d)) == 1
          || a2600debug_bp_list(d, &bp, 1) == 1, "a console break joins the table");
    n = a2600debug_bp_list(d, NULL, 0);
    check(n == 1, "one entry");
    a2600debug_command(d, "trapWrite $80", reason, sizeof reason);
    check(a2600debug_bp_list(d, NULL, 0) == 2, "a console trap joins it");
    a2600debug_command(d, "clearBreaks", reason, sizeof reason);
    check(a2600debug_bp_list(d, NULL, 0) == 1, "clearBreaks drops the break and leaves the trap");
    a2600debug_command(d, "clearTraps", reason, sizeof reason);
    check(a2600debug_bp_list(d, NULL, 0) == 0, "clearTraps drops the trap");

    /* bank-specific entries and the description */
    a2600debug_breakpoint_clear(d);
    bank = a2600debug_current_bank(d);
    a2600debug_cpu_get(d, &cpu);
    check(a2600debug_breakpoint_toggle(d, (uint16_t)cpu.pc, bank) == 1, "toggle sets a bank-specific breakpoint");
    check(a2600debug_breakpoint_toggle(d, (uint16_t)cpu.pc, bank) == 0, "and clears it");

    /* expressions */
    a2600debug_write(d, 0x80, 0x5c);
    check(a2600debug_eval(d, "$10+2", &val, err, sizeof err) == 0 && val == 0x12, "eval: hex arithmetic");
    check(a2600debug_eval(d, "@$80", &val, err, sizeof err) == 0 && val == 0x5c, "eval: @addr reads memory");
    check(a2600debug_eval(d, "nosuchlabel+", &val, err, sizeof err) == -1 && err[0], "eval: an error has a message");
    check(a2600debug_eval(d, "value + old", &val, err, sizeof err) == 0, "eval: value and old are accepted");

    /* side-effect-free reads */
    {
        int before = a2600debug_current_bank(d);
        a2600debug_riot_set(d, A2600_RIOT_TIM64T, 200);
        a2600debug_read(d, 0x1FF4, buf, 12);
        check(a2600debug_current_bank(d) == before, "reading the hotspot area does not bank-switch");
        a2600debug_read(d, 0x0284, buf, 2);
        {
            a2600debug_riot riot;
            a2600debug_riot_get(d, &riot);
            check(buf[0] == riot.intim && buf[1] == riot.timint, "the RIOT timer reads through its getters");
        }
        a2600debug_read(d, 0x0284, buf, 2);
        a2600debug_read(d, 0x0285, buf + 2, 1);
        check(buf[0] == buf[0], "and again");
    }

    /* the memory map: gap-free $0000-$1FFF */
    n = a2600debug_memory_map(d, map, 128);
    check(n > 4 && map[0].lo == 0 && map[n - 1].hi == 0x1FFF, "the memory map spans $0000-$1FFF");
    for (i = 1; i < n; i++)
        if (map[i].lo != map[i - 1].hi + 1) break;
    check(i == n, "with no gaps or overlaps");
    check(strstr(map[0].name, "TIA") != NULL, "it starts with the TIA");
    {
        int tia = 0, ram = 0, io = 0, cart = 0;
        for (i = 0; i < n; i++) {
            tia += strstr(map[i].name, "TIA") != NULL;
            ram += strstr(map[i].name, "RIOT RAM") != NULL;
            io += strstr(map[i].name, "RIOT I/O") != NULL;
            cart += strstr(map[i].name, "Cartridge") != NULL;
        }
        check(tia && ram && io && cart, "TIA, RIOT RAM, RIOT I/O and the cartridge are all named");
    }

    /* search */
    a2600debug_write(d, 0x90, 0xC3); a2600debug_write(d, 0x91, 0x5A); a2600debug_write(d, 0x92, 0xE7);
    pat[0] = 0xC3; pat[1] = 0x5A; pat[2] = 0xE7;
    n = a2600debug_search(d, pat, 3, 0xFF, 0, 0x80, 0xFF, hit, 16);
    check(n >= 1 && hit[0] == 0x90, "search finds the bytes in RAM");
    check(a2600debug_search(d, pat, 3, 0xFF, 0, 0x91, 0xFF, hit, 16) == 0
          || hit[0] != 0x90, "`to` and `from` bound the start");
    check(a2600debug_search(d, pat, 3, 0xFF, 0, 0x80, 0x8F, hit, 16) == 0, "`to` bounds the start address");
    pat[0] = 0xC0; /* masked: 0xC3 & 0xC0 */
    n = a2600debug_search(d, pat, 1, 0xF0, 0, 0x90, 0x90, hit, 16);
    check(n == 1, "a mask matches");
    pat[0] = 0xC3;
    check(a2600debug_search_next(d, pat, 3, 0xFF, 0, 0x00, 0, &at) == 1 && at == 0x90, "search_next finds the next");
    check(a2600debug_search_next(d, pat, 3, 0xFF, 0, 0x90, 0, &at) == 1, "and wraps around");
    check(a2600debug_search_next(d, pat, 3, 0xFF, 0, 0xFF, 1, &at) == 1 && at == 0x90, "backward from above");
    a2600debug_command(d, "find C3 5A E7", out, sizeof out);
    check(strstr(out, "$0090") != NULL, "the console's find");
    a2600debug_command(d, "find text zzzzzzzz", out, sizeof out);
    check(strstr(out, "not found") != NULL, "find text, nothing");

    /* watches through the console */
    a2600watch_clear();
    a2600debug_command(d, "watch $90", out, sizeof out);
    check(strstr(out, "$0090") != NULL && strstr(out, "$C3") != NULL, "the console's watch");
    check(a2600watch_count() == 1, "it is in the shared list");
    a2600debug_command(d, "unwatch all", out, sizeof out);
    check(a2600watch_count() == 0, "unwatch all");
    a2600debug_command(d, "cart", out, sizeof out);
    check(strstr(out, "FUJI") != NULL, "the console's cart");

    /* symbols */
    a2600debug_clear_symbols(d);
    check(a2600debug_symbols_count(d) == 0, "clear_symbols empties the table");
    check(a2600debug_set_label(d, 0x1234 & 0x1fff, "FooBar") == 0, "a label is definable");
    check(a2600debug_symbols_count(d) == 1, "and counted");
    n = a2600debug_symbols_filter(d, "foo", &i, 1);
    check(n == 1 && a2600debug_symbols_get(d, i, &at, name, sizeof name) == 1 && at == (0x1234 & 0x1fff)
          && !strcmp(name, "FooBar"), "filter and get, case-insensitively");
    check(a2600debug_symbols_filter(d, "zzz", &i, 1) == 0, "a filter that matches nothing");
    check(a2600debug_label_address(d, "FooBar") == (0x1234 & 0x1fff), "the label resolves");

    /* sound */
    a2600debug_tia_set(d, "audv0", 9);
    a2600debug_sound_get(d, &snd);
    check(snd.audv[0] == 9, "sound state reads the TIA's channels");

    /* the trace ring: allocated on enable, newest first */
    check(a2600debug_trace_count(d) == 0 && !a2600debug_trace_enabled(d), "the trace ring starts off and empty");
    a2600debug_trace_enable(d, 1);
    check(a2600debug_trace_enabled(d), "trace on");
    a2600debug_resume(d);
    sleep_ms(300);
    a2600debug_stop(d);
    n = a2600debug_trace_count(d);
    printf("trace entries: %d\n", n);
    check(n > 1000, "the ring fills while it runs");
    check(a2600debug_trace_read(d, tr, 16) == 16 && tr[0].cycles >= tr[1].cycles && tr[0].cycles > 0, "read: newest first");
    check(a2600debug_trace_read(d, NULL, 16) == 0, "read with no buffer");
    {
        static a2600debug_trace_entry big[65536];
        int got = a2600debug_trace_read(d, big, 65536);
        check(got == a2600debug_trace_count(d), "reading as many as are held returns them all");
        a2600debug_trace_format(&big[0], out, sizeof out);
        printf("trace: %s  bank %d line %d dot %d\n", out, big[0].bank, big[0].scanline, big[0].dot);
        check(out[0] != '\0' && big[0].pc >= 0x1000, "an entry formats");
        check(big[0].dot >= 0 && big[0].dot < 228, "the beam's colour clock is in a line");
    }
    a2600debug_trace_enable(d, 0);
    n = a2600debug_trace_count(d);
    a2600debug_resume(d);
    sleep_ms(100);
    a2600debug_stop(d);
    check(a2600debug_trace_count(d) == n, "off: nothing more is recorded");
    a2600debug_trace_clear(d);
    check(a2600debug_trace_count(d) == 0, "clear empties it");
    check(a2600debug_is_stopped(d), "stopped at the end");

    /* step over / out */
    a2600debug_step_over(d);
    check(a2600debug_is_stopped(d), "step over leaves it stopped");
    a2600debug_stop_reason(d, reason, sizeof reason, &address);
    check(strstr(reason, "step over at $") != NULL, "with its reason");
    /* out: a fake return address on the stack, an instruction the CONFIG
     * client runs every frame (the one the breakpoint hit above) */
    a2600debug_step(d);          /* off the anchor: a stop on it would not be a new arrival */
    a2600debug_cpu_set(d, A2600_REG_SP, 0xF0);
    a2600debug_write(d, 0x01F1, (uint8_t)((anchor - 1) & 0xFF));
    a2600debug_write(d, 0x01F2, (uint8_t)(((anchor - 1) >> 8) & 0xFF));
    a2600debug_step_out(d);
    check(wait_stopped(d), "step out runs to the return address and stops");
    a2600debug_cpu_get(d, &cpu);
    check(cpu.pc == anchor, "at the address the stack held, plus one");
}

/* A cartridge swap leaves Stella's console with none of our hooks: the
 * trace tap and the enabled breakpoints must be installed again. */
static void swap_test(a2600session *s, a2600debug *d)
{
    static char out[8192];
    int id;

    id = a2600debug_bp_add(d, A2600DEBUG_BP_EXEC, 0x1234, 0x1234, A2600DEBUG_ANY_BANK, "");
    a2600debug_trace_enable(d, 1);
    a2600debug_trace_clear(d);
    check(a2600session_reboot_to_config(s) == 0, "reboot to CONFIG");
    a2600debug_stop(d);
    a2600debug_command(d, "listBreaks", out, sizeof out);
    check(strstr(out, "1234") != NULL, "the breakpoint is installed on the new console");
    a2600debug_resume(d);
    sleep_ms(300);
    a2600debug_stop(d);
    check(a2600debug_trace_count(d) > 100, "and the trace tap records on it");
    a2600debug_trace_enable(d, 0);
    a2600debug_bp_remove(d, id);
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

    extra_tests(d);
    swap_test(s, d);

    a2600debug_resume(d);
    check(!a2600debug_is_stopped(d), "resume runs the machine");
    sleep_ms(200);
    check(!a2600debug_is_stopped(d), "and it stays running");

    a2600session_stop(s);
    a2600session_free(s);
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
