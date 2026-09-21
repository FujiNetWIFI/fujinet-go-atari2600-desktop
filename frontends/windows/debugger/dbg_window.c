/*
 * Debugger window (Win32) over Stella's own debugger engine, via
 * core/include/a2600debug.h. Mirrors the GTK and Qt debuggers tab for tab
 * -- Prompt, CPU & RAM, Disassembly, TIA, I/O, Breaks & Traps, States &
 * Cart -- built from plain common controls.
 *
 * Every a2600debug call blocks briefly on the Stella thread, so the window
 * refreshes on a timer keyed to the engine's generation counter rather
 * than on every tick.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "dbg_window.h"

#include <commctrl.h>
#include <commdlg.h>
#include <windowsx.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "a2600debug.h"

#define DISASM_WINDOW 48

#define WM_DBG_ACCEPT (WM_APP + 2)   /* wp: control id -- Enter in an edit */
#define WM_DBG_TAB    (WM_APP + 3)   /* Tab in the prompt: complete */

#define TIMER_REFRESH 1

enum {
    PAGE_PROMPT = 0, PAGE_CPU, PAGE_DISASM, PAGE_TIA, PAGE_RIOT, PAGE_BREAKS, PAGE_STATES,
    PAGE_COUNT
};

enum {
    IDC_RUN = 1000, IDC_STEP, IDC_TRACE, IDC_SCAN, IDC_FRAME, IDC_REWIND, IDC_UNWIND,
    IDC_STATUS, IDC_TABS,
    IDC_PROMPT_OUT, IDC_PROMPT_IN, IDC_LOAD_SYMBOLS,
    IDC_REG0, IDC_REG_LAST = IDC_REG0 + 5,
    IDC_FLAG0, IDC_FLAG_LAST = IDC_FLAG0 + 6,
    IDC_CYCLES, IDC_RAM_VIEW, IDC_RAM_ADDR, IDC_RAM_VAL,
    IDC_BANK, IDC_FOLLOW_PC, IDC_JUMP, IDC_DISASM,
    IDC_TIA_TEXT, IDC_TIA_REG, IDC_TIA_VAL, IDC_TIA_PIC, IDC_TIA_PARTIAL,
    IDC_RIOT,
    IDC_BP_ENTRY, IDC_BP_CLEAR, IDC_BP_LIST,
    IDC_SAVE0, IDC_SAVE_LAST = IDC_SAVE0 + 9,
    IDC_LOAD0, IDC_LOAD_LAST = IDC_LOAD0 + 9,
    IDC_FILE0, IDC_FILE_LAST = IDC_FILE0 + 4,
    IDC_CART_INFO,
    IDC_LABEL_FIRST
};

typedef struct {
    HWND hwnd;
    HWND tabs;
    HWND run_btn, step_btn, trace_btn, scan_btn, frame_btn, rewind_btn, unwind_btn, status;

    HWND prompt_out, prompt_in, load_symbols;

    HWND reg_label[6], reg_edit[6], flag[7], cycles, ram_label, ram_view;
    HWND ram_addr_label, ram_addr, ram_val_label, ram_val;

    HWND bank, follow_pc, jump_label, jump, disasm_hint, disasm;

    HWND tia_text, tia_reg_label, tia_reg, tia_val_label, tia_val, tia_pic, tia_partial;

    HWND riot;

    HWND bp_entry, bp_clear, bp_list;

    HWND states_label, save_btn[10], load_btn[10], files_label, file_btn[5], cart_label, cart_info;

    HFONT mono, ui;
    HACCEL accel;
    HBRUSH accent;

    a2600session *session;
    a2600debug *dbg;

    int page;
    unsigned seen_gen;
    int was_stopped;
    int running_ticks;

    int disasm_first;
    int disasm_bank;         /* -1 = the PC's */
    uint16_t line_addr[DISASM_WINDOW];
    int line_count;

    uint32_t *tia_px;
    int tia_h;
} debugger;

static debugger *g_dbg;

static const char *const kFileKinds[5] = { "dis", "rom", "access", "ses", "snap" };
static const char *const kFileTitles[5] = { "Disassembly...", "ROM (patched)...", "Access counters...",
                                            "Session...", "TIA snapshot..." };

/* ---- helpers ---------------------------------------------------------------- */

static void set_text(HWND h, const char *text) { SetWindowTextA(h, text); }

/* EDIT controls want CRLF and render control characters as boxes; Stella's
 * prompt output carries a few of the latter. */
static void set_text_lf(HWND h, const char *text)
{
    size_t n = strlen(text), i, o = 0;
    char *buf = malloc(n * 2 + 1);
    if (!buf) { set_text(h, text); return; }
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c == '\n') { buf[o++] = '\r'; buf[o++] = '\n'; }
        else if (c >= 0x20 || c == '\t') buf[o++] = (char)c;
    }
    buf[o] = '\0';
    set_text(h, buf);
    free(buf);
}

static void append_text_lf(HWND h, const char *text)
{
    size_t n = strlen(text), i, o = 0;
    char *buf = malloc(n * 2 + 1);
    int len;
    if (!buf) return;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c == '\n') { buf[o++] = '\r'; buf[o++] = '\n'; }
        else if (c >= 0x20 || c == '\t') buf[o++] = (char)c;
    }
    buf[o] = '\0';
    len = GetWindowTextLengthA(h);
    SendMessageA(h, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    SendMessageA(h, EM_REPLACESEL, FALSE, (LPARAM)buf);
    SendMessageA(h, EM_SCROLLCARET, 0, 0);
    free(buf);
}

static void edit_text(HWND h, char *out, int outsz) { GetWindowTextA(h, out, outsz); }

/* $hex, 0xhex, #dec, or bare hex, as Stella's prompt reads numbers. */
static int parse_num(const char *text, long *out)
{
    char buf[64];
    const char *p;
    char *end;
    long v;
    int base = 16;

    snprintf(buf, sizeof buf, "%s", text);
    p = buf;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '$') p++;
    else if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
    else if (*p == '#') { p++; base = 10; }
    if (!*p) return 0;
    v = strtol(p, &end, base);
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') end++;
    if (*end) return 0;
    *out = v;
    return 1;
}

static int resolve_addr(debugger *d, const char *text)
{
    long v;
    int addr = a2600debug_label_address(d->dbg, text);
    if (addr < 0 && parse_num(text, &v) && v >= 0 && v <= 0xffff) addr = (int)v;
    return addr;
}

/* ---- refreshers -------------------------------------------------------------- */

static void refresh_status(debugger *d)
{
    char reason[160], info[256], buf[200];
    int addr;
    const int stopped = a2600debug_is_stopped(d->dbg);
    a2600debug_stop_reason(d->dbg, reason, sizeof reason, &addr);
    a2600debug_cart_info(d->dbg, info, sizeof info);
    if (stopped) snprintf(buf, sizeof buf, "Stopped%s%s", reason[0] ? ": " : "", reason);
    else snprintf(buf, sizeof buf, "Running");
    set_text(d->status, buf);
    set_text_lf(d->cart_info, info);
    set_text(d->run_btn, stopped ? "Run (F5)" : "Stop (F5)");
}

static void refresh_cpu(debugger *d)
{
    a2600debug_cpu c;
    char buf[96];
    int i;
    const int *vals;
    a2600debug_cpu_get(d->dbg, &c);
    {
        const int v[6] = { c.pc, c.sp, c.a, c.x, c.y, c.ps };
        vals = v;
        for (i = 0; i < 6; i++) {
            if (GetFocus() == d->reg_edit[i]) continue;   /* do not fight the user's typing */
            snprintf(buf, sizeof buf, i == 0 ? "%04X" : "%02X", vals[i]);
            set_text(d->reg_edit[i], buf);
        }
    }
    {
        const int flags[7] = { c.n, c.v, c.b, c.d, c.i, c.z, c.c };
        for (i = 0; i < 7; i++)
            SendMessageA(d->flag[i], BM_SETCHECK, flags[i] ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    snprintf(buf, sizeof buf, "last instruction: %d cycles, total %llu", c.cycles,
             (unsigned long long)c.total_cycles);
    set_text(d->cycles, buf);
}

static void refresh_ram(debugger *d)
{
    uint8_t ram[128];
    char text[1024];
    size_t len = 0;
    int row, col;
    a2600debug_ram_get(d->dbg, ram);
    len += (size_t)snprintf(text + len, sizeof text - len,
                            "      0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F\r\n");
    for (row = 0; row < 8; row++) {
        len += (size_t)snprintf(text + len, sizeof text - len, "$%02X: ", 0x80 + row * 16);
        for (col = 0; col < 16; col++)
            len += (size_t)snprintf(text + len, sizeof text - len, "%02X ", ram[row * 16 + col]);
        len += (size_t)snprintf(text + len, sizeof text - len, "\r\n");
    }
    set_text(d->ram_view, text);
}

static void refresh_disasm(debugger *d)
{
    static a2600debug_line lines[DISASM_WINDOW];
    int total = 0, pc_line = -1, n, i;
    char *text;
    size_t cap = DISASM_WINDOW * 160, len = 0;
    const int follow = SendMessageA(d->follow_pc, BM_GETCHECK, 0, 0) == BST_CHECKED;

    a2600debug_disassemble(d->dbg, d->disasm_bank, 0, lines, 1, &total, &pc_line);
    if (follow && pc_line >= 0)
        d->disasm_first = pc_line > DISASM_WINDOW / 3 ? pc_line - DISASM_WINDOW / 3 : 0;
    if (d->disasm_first > total - 1) d->disasm_first = total > 0 ? total - 1 : 0;
    if (d->disasm_first < 0) d->disasm_first = 0;
    n = a2600debug_disassemble(d->dbg, d->disasm_bank, d->disasm_first, lines, DISASM_WINDOW, &total, &pc_line);

    text = malloc(cap);
    if (!text) return;
    text[0] = '\0';
    d->line_count = n;
    for (i = 0; i < n; i++) {
        d->line_addr[i] = lines[i].address;
        len += (size_t)snprintf(text + len, cap - len, "%c%c %04X  %-10s %-14s %-22s %s\r\n",
                                lines[i].has_breakpoint ? '*' : ' ', lines[i].is_pc ? '>' : ' ',
                                lines[i].address, lines[i].bytes, lines[i].label, lines[i].disasm,
                                lines[i].cycles);
        if (len + 200 >= cap) break;
    }
    if (n == 0) snprintf(text, cap, "(no disassembly)\r\n");
    set_text(d->disasm, text);
    free(text);
}

static void refresh_tia(debugger *d)
{
    a2600debug_tia t;
    static const char *const coll[15] = { "M0-P1", "M0-P0", "M1-P0", "M1-P1", "P0-PF", "P0-BL", "P1-PF",
        "P1-BL", "M0-PF", "M0-BL", "M1-PF", "M1-BL", "BL-PF", "P0-P1", "M0-M1" };
    char s[2048];
    size_t len = 0;
    int i, any = 0;

    a2600debug_tia_get(d->dbg, &t);
    len += (size_t)snprintf(s + len, sizeof s - len,
        "Frame %d   scanline %d (last frame %d)   frame cycles %d (WSYNC %d)\n",
        t.frame_count, t.scanlines, t.scanlines_last, t.frame_cycles, t.wsync_cycles);
    len += (size_t)snprintf(s + len, sizeof s - len,
        "clocks this line %d   cycles this line %d   beam %d,%d\nVSYNC %d   VBLANK %d\n\n",
        t.clocks_this_line, t.cycles_this_line, t.beam_x, t.beam_y, t.vsync, t.vblank);
    len += (size_t)snprintf(s + len, sizeof s - len,
        "Colours   COLUP0 %02X   COLUP1 %02X   COLUPF %02X   COLUBK %02X\n",
        t.colup0, t.colup1, t.colupf, t.colubk);
    len += (size_t)snprintf(s + len, sizeof s - len,
        "Players   GRP0 %02X  GRP1 %02X   NUSIZ0 %02X  NUSIZ1 %02X   REFP0 %d  REFP1 %d   VDELP0 %d  VDELP1 %d\n",
        t.graphics_p0, t.graphics_p1, t.nusiz0, t.nusiz1, t.refp0, t.refp1, t.vdelp0, t.vdelp1);
    len += (size_t)snprintf(s + len, sizeof s - len,
        "Missiles  ENAM0 %d  ENAM1 %d   RESMP0 %d  RESMP1 %d\nBall      ENABL %d   VDELBL %d\n",
        t.enam0, t.enam1, t.resmp0, t.resmp1, t.enabl, t.vdelbl);
    len += (size_t)snprintf(s + len, sizeof s - len,
        "Playfield PF0 %02X  PF1 %02X  PF2 %02X   CTRLPF %02X   REF %d  SCORE %d  PRIORITY %d\n\n",
        t.pf0, t.pf1, t.pf2, t.ctrlpf, t.refpf, t.scorepf, t.pripf);
    len += (size_t)snprintf(s + len, sizeof s - len,
        "Positions      P0 %3d   P1 %3d   M0 %3d   M1 %3d   BL %3d\n",
        t.pos_p0, t.pos_p1, t.pos_m0, t.pos_m1, t.pos_bl);
    len += (size_t)snprintf(s + len, sizeof s - len,
        "Motion (HM)    P0 %02X    P1 %02X    M0 %02X    M1 %02X    BL %02X\n\n",
        t.hm_p0, t.hm_p1, t.hm_m0, t.hm_m1, t.hm_bl);
    len += (size_t)snprintf(s + len, sizeof s - len,
        "Audio     AUDC0 %02X  AUDF0 %02X  AUDV0 %02X  (%s)\n          AUDC1 %02X  AUDF1 %02X  AUDV1 %02X  (%s)\n\nCollisions:",
        t.audc0, t.audf0, t.audv0, t.aud_freq0, t.audc1, t.audf1, t.audv1, t.aud_freq1);
    for (i = 0; i < 15; i++)
        if (t.collisions & (1u << i)) { len += (size_t)snprintf(s + len, sizeof s - len, " %s", coll[i]); any = 1; }
    if (!any) len += (size_t)snprintf(s + len, sizeof s - len, " none");
    snprintf(s + len, sizeof s - len,
        "\n\nSet a register: name and value below (colup0, pf1, posp0, refp0, vsync ...); "
        "strobes: wsync rsync resp0 resp1 resm0 resm1 resbl hmove hmclr cxclr\n");
    set_text_lf(d->tia_text, s);

    d->tia_h = a2600debug_frame_snapshot(d->dbg, d->tia_px,
                                         SendMessageA(d->tia_partial, BM_GETCHECK, 0, 0) == BST_CHECKED);
    InvalidateRect(d->tia_pic, NULL, FALSE);
}

static void refresh_riot(debugger *d)
{
    a2600debug_riot r;
    char s[1024];
    size_t len = 0;
    int i;
    a2600debug_riot_get(d->dbg, &r);
    len += (size_t)snprintf(s + len, sizeof s - len,
        "SWCHA  %02X   SWACNT %02X     left: %s   right: %s\nSWCHB  %02X   SWBCNT %02X\nINPT0-5  ",
        r.swcha, r.swacnt, r.dir_left, r.dir_right, r.swchb, r.swbcnt);
    for (i = 0; i < 6; i++) len += (size_t)snprintf(s + len, sizeof s - len, "%02X ", r.inpt[i]);
    len += (size_t)snprintf(s + len, sizeof s - len,
        "\n\nTimer  INTIM %02X   TIMINT %02X   clocks %d   divider %d\n\n", r.intim, r.timint,
        r.tim_clocks, r.tim_divider);
    len += (size_t)snprintf(s + len, sizeof s - len,
        "Switches   Select %s   Reset %s   %s   Left difficulty %s   Right difficulty %s\n\n",
        r.select ? "pressed" : "up", r.reset ? "pressed" : "up", r.color ? "Color" : "B&W",
        r.diff_left_a ? "A" : "B", r.diff_right_a ? "A" : "B");
    snprintf(s + len, sizeof s - len,
        "The switches can be flipped from the menu, the keypad window or the prompt (swchb $xx); "
        "the ports with joy0up, joy0fire ...");
    set_text_lf(d->riot, s);
}

static void refresh_bps(debugger *d)
{
    uint32_t bps[64];
    static char out[8192];
    static char text[32768];
    size_t len = 0;
    int i;
    const int n = a2600debug_breakpoint_list(d->dbg, bps, 64);
    len += (size_t)snprintf(text + len, sizeof text - len, "Breakpoints (%d):\n", n);
    for (i = 0; i < n; i++) {
        const unsigned bank = bps[i] >> 16;
        if (bank == A2600DEBUG_ANY_BANK)
            len += (size_t)snprintf(text + len, sizeof text - len, "  $%04X  any bank\n", bps[i] & 0xffff);
        else
            len += (size_t)snprintf(text + len, sizeof text - len, "  $%04X  bank %u\n", bps[i] & 0xffff, bank);
    }
    a2600debug_command(d->dbg, "listTraps", out, sizeof out);
    len += (size_t)snprintf(text + len, sizeof text - len, "\nTraps:\n%s\n", out);
    a2600debug_command(d->dbg, "listBreaks", out, sizeof out);
    snprintf(text + len, sizeof text - len, "\nConditional (breakIf):\n%s\n", out);
    set_text_lf(d->bp_list, text);
}

static void refresh_all(debugger *d)
{
    refresh_status(d);
    refresh_cpu(d);
    refresh_ram(d);
    refresh_disasm(d);
    refresh_tia(d);
    refresh_riot(d);
    refresh_bps(d);
}

/* ---- actions ------------------------------------------------------------------ */

static void toggle_run(debugger *d)
{
    if (a2600debug_is_stopped(d->dbg)) a2600debug_resume(d->dbg);
    else a2600debug_stop(d->dbg);
    refresh_all(d);
}

static void run_prompt(debugger *d)
{
    static char out[65536];
    char cmd[512], echo[540];
    edit_text(d->prompt_in, cmd, sizeof cmd);
    if (!cmd[0]) return;
    snprintf(echo, sizeof echo, "> %s\n", cmd);
    append_text_lf(d->prompt_out, echo);
    a2600debug_command(d->dbg, cmd, out, sizeof out);
    append_text_lf(d->prompt_out, out);
    append_text_lf(d->prompt_out, "\n");
    set_text(d->prompt_in, "");
    refresh_all(d);
}

static void complete_prompt(debugger *d)
{
    char text[512], comps[4096];
    const char *word;
    char *sp;
    int n;
    edit_text(d->prompt_in, text, sizeof text);
    sp = strrchr(text, ' ');
    word = sp ? sp + 1 : text;
    n = a2600debug_completions(d->dbg, word, comps, sizeof comps);
    if (n == 1) {
        char line[256], joined[800];
        char *nl;
        snprintf(line, sizeof line, "%s", comps);
        nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        if (sp) sp[1] = '\0'; else text[0] = '\0';
        snprintf(joined, sizeof joined, "%s%s ", text, line);
        set_text(d->prompt_in, joined);
        SendMessageA(d->prompt_in, EM_SETSEL, (WPARAM)strlen(joined), (LPARAM)strlen(joined));
    } else if (n > 1) {
        append_text_lf(d->prompt_out, comps);
    }
}

static void jump_to(debugger *d, const char *text)
{
    static a2600debug_line lines[256];
    int total = 0, pc_line = -1, first = 0;
    const int addr = resolve_addr(d, text);
    if (addr < 0) return;
    SendMessageA(d->follow_pc, BM_SETCHECK, BST_UNCHECKED, 0);
    for (;;) {
        int n = a2600debug_disassemble(d->dbg, d->disasm_bank, first, lines, 256, &total, &pc_line), i;
        if (n == 0) break;
        for (i = 0; i < n; i++)
            if (lines[i].address >= addr) { d->disasm_first = first + i; refresh_disasm(d); return; }
        first += n;
        if (first >= total) break;
    }
}

static void save_file(debugger *d, int which)
{
    OPENFILENAMEA ofn;
    char path[MAX_PATH] = "", msg[512], title[64];
    memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = d->hwnd;
    ofn.lpstrFile = path;
    ofn.nMaxFile = sizeof path;
    snprintf(title, sizeof title, "Save %s", kFileTitles[which]);
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameA(&ofn)) return;
    a2600debug_save(d->dbg, kFileKinds[which], path, msg, sizeof msg);
    append_text_lf(d->prompt_out, msg);
    append_text_lf(d->prompt_out, "\n");
}

static void on_accept(debugger *d, int id)
{
    char buf[512];
    long v;

    if (id >= IDC_REG0 && id <= IDC_REG_LAST) {
        static const int reg_ids[6] = { A2600_REG_PC, A2600_REG_SP, A2600_REG_A, A2600_REG_X, A2600_REG_Y, A2600_REG_PS };
        edit_text(d->reg_edit[id - IDC_REG0], buf, sizeof buf);
        if (parse_num(buf, &v)) a2600debug_cpu_set(d->dbg, reg_ids[id - IDC_REG0], (int)v);
        refresh_all(d);
        return;
    }
    switch (id) {
    case IDC_PROMPT_IN: run_prompt(d); break;
    case IDC_RAM_ADDR:
    case IDC_RAM_VAL: {
        char abuf[64];
        long a;
        edit_text(d->ram_addr, abuf, sizeof abuf);
        edit_text(d->ram_val, buf, sizeof buf);
        if (parse_num(abuf, &a) && parse_num(buf, &v)) a2600debug_write(d->dbg, (uint16_t)a, (uint8_t)v);
        refresh_all(d);
        break;
    }
    case IDC_JUMP:
        edit_text(d->jump, buf, sizeof buf);
        jump_to(d, buf);
        break;
    case IDC_TIA_REG:
    case IDC_TIA_VAL: {
        char reg[64];
        char *p;
        edit_text(d->tia_reg, reg, sizeof reg);
        for (p = reg; *p; p++) if (*p >= 'A' && *p <= 'Z') *p = (char)(*p + 32);
        p = reg;
        while (*p == ' ') p++;
        if (a2600debug_tia_strobe(d->dbg, p) == 0) { refresh_all(d); break; }
        edit_text(d->tia_val, buf, sizeof buf);
        if (parse_num(buf, &v)) a2600debug_tia_set(d->dbg, p, (int)v);
        refresh_all(d);
        break;
    }
    case IDC_BP_ENTRY: {
        int addr;
        char *p = buf;
        edit_text(d->bp_entry, buf, sizeof buf);
        while (*p == ' ') p++;
        if (!*p) break;
        addr = a2600debug_label_address(d->dbg, p);
        if (addr < 0 && strchr("$#0123456789abcdefABCDEF", *p) && parse_num(p, &v) && v >= 0 && v <= 0xffff)
            addr = (int)v;
        if (addr >= 0) {
            a2600debug_breakpoint_toggle(d->dbg, (uint16_t)addr, A2600DEBUG_ANY_BANK);
        } else {
            static char out[2048];
            a2600debug_command(d->dbg, p, out, sizeof out);
        }
        set_text(d->bp_entry, "");
        refresh_all(d);
        break;
    }
    default: break;
    }
}

/* ---- the TIA picture ------------------------------------------------------------- */

static LRESULT CALLBACK pic_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    debugger *d = g_dbg;
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT c;
        GetClientRect(hwnd, &c);
        FillRect(dc, &c, (HBRUSH)GetStockObject(BLACK_BRUSH));
        if (d && d->tia_px && d->tia_h > 0) {
            BITMAPINFO bmi;
            double want = 4.0 / 3.0, w = c.right, h = c.bottom, sw, sh;
            if (w / h > want) { sh = h; sw = sh * want; } else { sw = w; sh = sw / want; }
            memset(&bmi, 0, sizeof bmi);
            bmi.bmiHeader.biSize = sizeof bmi.bmiHeader;
            bmi.bmiHeader.biWidth = A2600SESSION_FB_WIDTH;
            bmi.bmiHeader.biHeight = -d->tia_h;
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 32;
            bmi.bmiHeader.biCompression = BI_RGB;
            SetStretchBltMode(dc, COLORONCOLOR);
            StretchDIBits(dc, (int)((w - sw) / 2), (int)((h - sh) / 2), (int)sw, (int)sh,
                          0, 0, A2600SESSION_FB_WIDTH, d->tia_h, d->tia_px, &bmi, DIB_RGB_COLORS, SRCCOPY);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    if (msg == WM_ERASEBKGND) return 1;
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* ---- edit subclassing ------------------------------------------------------------- */

static WNDPROC g_edit_proc;
static WNDPROC g_disasm_proc;

/* Enter in a single-line field means "apply this value"; Tab in the prompt
 * completes. Neither may beep its way through the default handler. */
static LRESULT CALLBACK edit_subclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    const int id = (int)GetWindowLongPtrA(hwnd, GWLP_ID);
    if (msg == WM_KEYDOWN && wp == VK_RETURN) {
        PostMessageA(GetParent(hwnd), WM_DBG_ACCEPT, (WPARAM)id, 0);
        return 0;
    }
    if (msg == WM_CHAR && wp == VK_RETURN) return 0;
    if (id == IDC_PROMPT_IN && msg == WM_KEYDOWN && wp == VK_TAB) {
        PostMessageA(GetParent(hwnd), WM_DBG_TAB, 0, 0);
        return 0;
    }
    if (id == IDC_PROMPT_IN && msg == WM_CHAR && wp == VK_TAB) return 0;
    if (id == IDC_PROMPT_IN && msg == WM_GETDLGCODE) return DLGC_WANTALLKEYS;
    return CallWindowProcA(g_edit_proc, hwnd, msg, wp, lp);
}

/* A click in the disassembly toggles the breakpoint on that line; the
 * wheel browses (and turns Follow PC off, as in the other frontends). */
static LRESULT CALLBACK disasm_subclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    debugger *d = g_dbg;
    if (!d) return CallWindowProcA(g_disasm_proc, hwnd, msg, wp, lp);
    if (msg == WM_LBUTTONDOWN) {
        LRESULT pos = SendMessageA(hwnd, EM_CHARFROMPOS, 0, lp);
        int line = HIWORD(pos);
        if (line >= 0 && line < d->line_count) {
            const int bank = d->disasm_bank < 0 ? a2600debug_current_bank(d->dbg) : d->disasm_bank;
            a2600debug_breakpoint_toggle(d->dbg, d->line_addr[line], bank);
            refresh_disasm(d);
            refresh_bps(d);
        }
        return 0;
    }
    if (msg == WM_MOUSEWHEEL) {
        SendMessageA(d->follow_pc, BM_SETCHECK, BST_UNCHECKED, 0);
        d->disasm_first -= GET_WHEEL_DELTA_WPARAM(wp) / 40;
        refresh_disasm(d);
        return 0;
    }
    return CallWindowProcA(g_disasm_proc, hwnd, msg, wp, lp);
}

/* ---- construction ------------------------------------------------------------------ */

static HWND child(debugger *d, const char *cls, const char *text, DWORD style, int id, HFONT font)
{
    HWND h = CreateWindowExA(0, cls, text, WS_CHILD | style, 0, 0, 10, 10, d->hwnd,
                             (HMENU)(INT_PTR)id, (HINSTANCE)GetWindowLongPtrA(d->hwnd, GWLP_HINSTANCE), NULL);
    SendMessageA(h, WM_SETFONT, (WPARAM)font, TRUE);
    return h;
}

static HWND mono_view(debugger *d, int id, int wrap)
{
    return child(d, "EDIT", "",
                 WS_BORDER | WS_VSCROLL | (wrap ? 0 : WS_HSCROLL) | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                 id, d->mono);
}

static HWND field(debugger *d, int id, HFONT font)
{
    HWND h = child(d, "EDIT", "", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, id, font);
    g_edit_proc = (WNDPROC)SetWindowLongPtrA(h, GWLP_WNDPROC, (LONG_PTR)edit_subclass);
    return h;
}

static HWND label(debugger *d, const char *text)
{
    static int next_id;
    return child(d, "STATIC", text, SS_LEFT, IDC_LABEL_FIRST + next_id++, d->ui);
}

static HWND button(debugger *d, const char *text, int id)
{
    return child(d, "BUTTON", text, BS_PUSHBUTTON | WS_TABSTOP, id, d->ui);
}

static HWND checkbox(debugger *d, const char *text, int id)
{
    return child(d, "BUTTON", text, BS_AUTOCHECKBOX | WS_TABSTOP, id, d->ui);
}

static void build_controls(debugger *d)
{
    TCITEMA item;
    static const char *const tabs[PAGE_COUNT] = { "Prompt", "CPU && RAM", "Disassembly", "TIA", "I/O",
                                                  "Breaks && Traps", "States && Cart" };
    static const char *const reg_names[6] = { "PC", "SP", "A", "X", "Y", "PS" };
    static const char *const flag_names[7] = { "N", "V", "B", "D", "I", "Z", "C" };
    int i, n;

    d->tabs = child(d, WC_TABCONTROLA, "", WS_VISIBLE | WS_CLIPSIBLINGS, IDC_TABS, d->ui);
    memset(&item, 0, sizeof item);
    item.mask = TCIF_TEXT;
    for (i = 0; i < PAGE_COUNT; i++) {
        item.pszText = (char *)tabs[i];
        SendMessageA(d->tabs, TCM_INSERTITEMA, (WPARAM)i, (LPARAM)&item);
    }

    /* Toolbar */
    d->run_btn = button(d, "Stop (F5)", IDC_RUN);
    d->step_btn = button(d, "Step (F7)", IDC_STEP);
    d->trace_btn = button(d, "Trace (F8)", IDC_TRACE);
    d->scan_btn = button(d, "Scan+1", IDC_SCAN);
    d->frame_btn = button(d, "Frame+1 (Shift+F8)", IDC_FRAME);
    d->rewind_btn = button(d, "Rewind", IDC_REWIND);
    d->unwind_btn = button(d, "Unwind", IDC_UNWIND);
    d->status = child(d, "STATIC", "Running", SS_RIGHT | SS_ENDELLIPSIS, IDC_STATUS, d->ui);
    {
        HWND bar[8] = { d->run_btn, d->step_btn, d->trace_btn, d->scan_btn, d->frame_btn, d->rewind_btn, d->unwind_btn, d->status };
        for (i = 0; i < 8; i++) ShowWindow(bar[i], SW_SHOW);
    }

    /* Prompt */
    d->prompt_out = mono_view(d, IDC_PROMPT_OUT, 1);
    set_text(d->prompt_out, "Stella debugger prompt. Type 'help' for every command.\r\n");
    d->prompt_in = field(d, IDC_PROMPT_IN, d->mono);
    SendMessageA(d->prompt_in, EM_SETCUEBANNER, TRUE,
                 (LPARAM)L"Stella debugger command (help, break, breakIf, trap, watch, frame, tia, ...) - Tab completes");
    d->load_symbols = button(d, "Load symbols", IDC_LOAD_SYMBOLS);

    /* CPU & RAM */
    for (i = 0; i < 6; i++) {
        d->reg_label[i] = label(d, reg_names[i]);
        d->reg_edit[i] = field(d, IDC_REG0 + i, d->mono);
        SendMessageA(d->reg_edit[i], EM_SETLIMITTEXT, 6, 0);
    }
    for (i = 0; i < 7; i++) d->flag[i] = checkbox(d, flag_names[i], IDC_FLAG0 + i);
    d->cycles = label(d, "");
    d->ram_label = label(d, "Zero-page RAM ($80-$FF)");
    d->ram_view = mono_view(d, IDC_RAM_VIEW, 0);
    d->ram_addr_label = label(d, "Write address");
    d->ram_addr = field(d, IDC_RAM_ADDR, d->mono);
    d->ram_val_label = label(d, "value");
    d->ram_val = field(d, IDC_RAM_VAL, d->mono);

    /* Disassembly */
    d->bank = child(d, "COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IDC_BANK, d->ui);
    SendMessageA(d->bank, CB_ADDSTRING, 0, (LPARAM)"PC's bank");
    n = a2600debug_bank_count(d->dbg);
    for (i = 0; i < n; i++) {
        char name[32];
        snprintf(name, sizeof name, "Bank %d", i);
        SendMessageA(d->bank, CB_ADDSTRING, 0, (LPARAM)name);
    }
    SendMessageA(d->bank, CB_SETCURSEL, 0, 0);
    d->follow_pc = checkbox(d, "Follow PC", IDC_FOLLOW_PC);
    SendMessageA(d->follow_pc, BM_SETCHECK, BST_CHECKED, 0);
    d->jump_label = label(d, "Jump to");
    d->jump = field(d, IDC_JUMP, d->mono);
    d->disasm_hint = label(d, "Click a line to toggle its breakpoint; scroll to browse");
    d->disasm = mono_view(d, IDC_DISASM, 0);
    g_disasm_proc = (WNDPROC)SetWindowLongPtrA(d->disasm, GWLP_WNDPROC, (LONG_PTR)disasm_subclass);

    /* TIA */
    d->tia_text = mono_view(d, IDC_TIA_TEXT, 1);
    d->tia_reg_label = label(d, "Register / strobe");
    d->tia_reg = field(d, IDC_TIA_REG, d->mono);
    d->tia_val_label = label(d, "value");
    d->tia_val = field(d, IDC_TIA_VAL, d->mono);
    d->tia_pic = child(d, "A2600DbgPixels", "", 0, IDC_TIA_PIC, d->ui);
    d->tia_partial = checkbox(d, "Frame in progress (to the beam)", IDC_TIA_PARTIAL);

    /* I/O */
    d->riot = mono_view(d, IDC_RIOT, 1);

    /* Breaks & Traps */
    d->bp_entry = field(d, IDC_BP_ENTRY, d->mono);
    SendMessageA(d->bp_entry, EM_SETCUEBANNER, TRUE,
                 (LPARAM)L"address/label to toggle, or breakIf {..}, trap $80, trapWrite $80 $ff, watch a ...");
    d->bp_clear = button(d, "Clear all", IDC_BP_CLEAR);
    d->bp_list = mono_view(d, IDC_BP_LIST, 0);

    /* States & Cart */
    d->states_label = label(d, "Emulator states (Stella's saveState / loadState slots)");
    for (i = 0; i < 10; i++) {
        char name[16];
        snprintf(name, sizeof name, "Save %d", i);
        d->save_btn[i] = button(d, name, IDC_SAVE0 + i);
        snprintf(name, sizeof name, "Load %d", i);
        d->load_btn[i] = button(d, name, IDC_LOAD0 + i);
    }
    d->files_label = label(d, "Save to a file");
    for (i = 0; i < 5; i++) d->file_btn[i] = button(d, kFileTitles[i], IDC_FILE0 + i);
    d->cart_label = label(d, "Cartridge");
    d->cart_info = child(d, "STATIC", "", SS_LEFT, IDC_CART_INFO, d->ui);
}

static void show_page(debugger *d, int page)
{
    HWND prompt[] = { d->prompt_out, d->prompt_in, d->load_symbols };
    HWND cpu[] = { d->cycles, d->ram_label, d->ram_view, d->ram_addr_label, d->ram_addr, d->ram_val_label, d->ram_val };
    HWND dis[] = { d->bank, d->follow_pc, d->jump_label, d->jump, d->disasm_hint, d->disasm };
    HWND tia[] = { d->tia_text, d->tia_reg_label, d->tia_reg, d->tia_val_label, d->tia_val, d->tia_pic, d->tia_partial };
    HWND brk[] = { d->bp_entry, d->bp_clear, d->bp_list };
    HWND st[] = { d->states_label, d->files_label, d->cart_label, d->cart_info };
    size_t i;
    d->page = page;
#define SHOW(arr, p) for (i = 0; i < sizeof(arr) / sizeof((arr)[0]); i++) ShowWindow((arr)[i], page == (p) ? SW_SHOW : SW_HIDE)
    SHOW(prompt, PAGE_PROMPT);
    SHOW(cpu, PAGE_CPU);
    for (i = 0; i < 6; i++) { ShowWindow(d->reg_label[i], page == PAGE_CPU ? SW_SHOW : SW_HIDE); ShowWindow(d->reg_edit[i], page == PAGE_CPU ? SW_SHOW : SW_HIDE); }
    for (i = 0; i < 7; i++) ShowWindow(d->flag[i], page == PAGE_CPU ? SW_SHOW : SW_HIDE);
    SHOW(dis, PAGE_DISASM);
    SHOW(tia, PAGE_TIA);
    ShowWindow(d->riot, page == PAGE_RIOT ? SW_SHOW : SW_HIDE);
    SHOW(brk, PAGE_BREAKS);
    SHOW(st, PAGE_STATES);
    for (i = 0; i < 10; i++) { ShowWindow(d->save_btn[i], page == PAGE_STATES ? SW_SHOW : SW_HIDE); ShowWindow(d->load_btn[i], page == PAGE_STATES ? SW_SHOW : SW_HIDE); }
    for (i = 0; i < 5; i++) ShowWindow(d->file_btn[i], page == PAGE_STATES ? SW_SHOW : SW_HIDE);
#undef SHOW
}

static void layout(debugger *d)
{
    RECT client, page;
    int y = 8, bx = 8, px, py, pw, ph, i;

    GetClientRect(d->hwnd, &client);

    MoveWindow(d->run_btn, bx, y, 104, 26, TRUE);    bx += 110;
    MoveWindow(d->step_btn, bx, y, 104, 26, TRUE);   bx += 110;
    MoveWindow(d->trace_btn, bx, y, 104, 26, TRUE);  bx += 110;
    MoveWindow(d->scan_btn, bx, y, 80, 26, TRUE);    bx += 86;
    MoveWindow(d->frame_btn, bx, y, 170, 26, TRUE);  bx += 176;
    MoveWindow(d->rewind_btn, bx, y, 80, 26, TRUE);  bx += 86;
    MoveWindow(d->unwind_btn, bx, y, 80, 26, TRUE);  bx += 86;
    MoveWindow(d->status, bx, y + 5, client.right - bx - 8 > 40 ? client.right - bx - 8 : 40, 20, TRUE);

    MoveWindow(d->tabs, 8, 40, client.right - 16, client.bottom - 48, TRUE);
    page = (RECT){ 8, 40, client.right - 8, client.bottom - 8 };
    SendMessageA(d->tabs, TCM_ADJUSTRECT, FALSE, (LPARAM)&page);
    px = page.left + 4; py = page.top + 4;
    pw = page.right - page.left - 8; ph = page.bottom - page.top - 8;
    if (pw < 100) pw = 100;
    if (ph < 100) ph = 100;

    /* Prompt */
    MoveWindow(d->prompt_out, px, py, pw, ph - 32, TRUE);
    MoveWindow(d->prompt_in, px, py + ph - 26, pw - 120, 24, TRUE);
    MoveWindow(d->load_symbols, px + pw - 112, py + ph - 27, 112, 26, TRUE);

    /* CPU & RAM */
    {
        int x = px, ry = py;
        for (i = 0; i < 6; i++) {
            MoveWindow(d->reg_label[i], x, ry + 4, 24, 18, TRUE); x += 26;
            MoveWindow(d->reg_edit[i], x, ry, 62, 24, TRUE); x += 74;
        }
        ry += 32;
        x = px;
        for (i = 0; i < 7; i++) { MoveWindow(d->flag[i], x, ry, 40, 22, TRUE); x += 44; }
        MoveWindow(d->cycles, x + 8, ry + 3, pw - (x - px) - 8, 18, TRUE);
        ry += 30;
        MoveWindow(d->ram_label, px, ry, pw, 18, TRUE); ry += 20;
        MoveWindow(d->ram_view, px, ry, pw, ph - (ry - py) - 34, TRUE);
        ry = py + ph - 26;
        MoveWindow(d->ram_addr_label, px, ry + 4, 90, 18, TRUE);
        MoveWindow(d->ram_addr, px + 94, ry, 80, 24, TRUE);
        MoveWindow(d->ram_val_label, px + 184, ry + 4, 40, 18, TRUE);
        MoveWindow(d->ram_val, px + 226, ry, 60, 24, TRUE);
    }

    /* Disassembly */
    MoveWindow(d->bank, px, py, 110, 200, TRUE);
    MoveWindow(d->follow_pc, px + 118, py + 2, 90, 22, TRUE);
    MoveWindow(d->jump_label, px + 214, py + 4, 50, 18, TRUE);
    MoveWindow(d->jump, px + 266, py, 150, 24, TRUE);
    MoveWindow(d->disasm_hint, px + 426, py + 4, pw - 426 > 40 ? pw - 426 : 40, 18, TRUE);
    MoveWindow(d->disasm, px, py + 30, pw, ph - 30, TRUE);

    /* TIA: text left, picture right */
    {
        int lw = pw * 55 / 100, rx = px + lw + 10, rw = pw - lw - 10;
        MoveWindow(d->tia_text, px, py, lw, ph - 32, TRUE);
        MoveWindow(d->tia_reg_label, px, py + ph - 22, 100, 18, TRUE);
        MoveWindow(d->tia_reg, px + 104, py + ph - 26, 100, 24, TRUE);
        MoveWindow(d->tia_val_label, px + 212, py + ph - 22, 40, 18, TRUE);
        MoveWindow(d->tia_val, px + 254, py + ph - 26, 70, 24, TRUE);
        MoveWindow(d->tia_pic, rx, py, rw, ph - 32, TRUE);
        MoveWindow(d->tia_partial, rx, py + ph - 24, rw, 22, TRUE);
    }

    /* I/O */
    MoveWindow(d->riot, px, py, pw, ph, TRUE);

    /* Breaks & Traps */
    MoveWindow(d->bp_entry, px, py, pw - 100, 24, TRUE);
    MoveWindow(d->bp_clear, px + pw - 92, py - 1, 92, 26, TRUE);
    MoveWindow(d->bp_list, px, py + 30, pw, ph - 30, TRUE);

    /* States & Cart */
    {
        int ry = py, col;
        MoveWindow(d->states_label, px, ry, pw, 18, TRUE); ry += 22;
        for (i = 0; i < 10; i++) {
            int row = i / 5;
            col = i % 5;
            MoveWindow(d->save_btn[i], px + col * 96, ry + row * 62, 90, 26, TRUE);
            MoveWindow(d->load_btn[i], px + col * 96, ry + row * 62 + 30, 90, 26, TRUE);
        }
        ry += 2 * 62 + 6;
        MoveWindow(d->files_label, px, ry, pw, 18, TRUE); ry += 22;
        for (i = 0; i < 5; i++) MoveWindow(d->file_btn[i], px + i * 136, ry, 130, 26, TRUE);
        ry += 36;
        MoveWindow(d->cart_label, px, ry, pw, 18, TRUE); ry += 22;
        MoveWindow(d->cart_info, px, ry, pw, 60, TRUE);
    }
}

/* ---- window proc -------------------------------------------------------------------- */

static LRESULT CALLBACK dbg_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    debugger *d = g_dbg;
    if (!d || d->hwnd != hwnd) return DefWindowProcA(hwnd, msg, wp, lp);

    switch (msg) {
    case WM_SIZE:
        layout(d);
        return 0;
    case WM_TIMER:
        if (wp == TIMER_REFRESH && IsWindowVisible(hwnd)) {
            const unsigned gen = a2600debug_generation(d->dbg);
            const int stopped = a2600debug_is_stopped(d->dbg);
            if (gen != d->seen_gen || stopped != d->was_stopped) {
                d->seen_gen = gen;
                d->was_stopped = stopped;
                refresh_all(d);
            } else if (!stopped && ++d->running_ticks >= 5) {
                d->running_ticks = 0;
                refresh_status(d);
                if (d->page == PAGE_CPU) refresh_cpu(d);
                else if (d->page == PAGE_TIA) refresh_tia(d);
                else if (d->page == PAGE_RIOT) refresh_riot(d);
            }
        }
        return 0;
    case WM_DBG_ACCEPT:
        on_accept(d, (int)wp);
        return 0;
    case WM_DBG_TAB:
        complete_prompt(d);
        return 0;
    case WM_NOTIFY:
        if (((LPNMHDR)lp)->code == (UINT)TCN_SELCHANGE) {
            show_page(d, (int)SendMessageA(d->tabs, TCM_GETCURSEL, 0, 0));
            layout(d);
            return 0;
        }
        break;
    case WM_CTLCOLORSTATIC:
        if ((HWND)lp == d->status || (HWND)lp == d->cycles || (HWND)lp == d->cart_info || (HWND)lp == d->disasm_hint) {
            SetTextColor((HDC)wp, GetSysColor(COLOR_GRAYTEXT));
            SetBkColor((HDC)wp, GetSysColor(COLOR_BTNFACE));
            return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
        }
        break;
    case WM_COMMAND: {
        const int id = LOWORD(wp);
        if (id >= IDC_FLAG0 && id <= IDC_FLAG_LAST && HIWORD(wp) == BN_CLICKED) {
            static const int flag_ids[7] = { A2600_FLAG_N, A2600_FLAG_V, A2600_FLAG_B, A2600_FLAG_D, A2600_FLAG_I, A2600_FLAG_Z, A2600_FLAG_C };
            if (a2600debug_is_stopped(d->dbg))
                a2600debug_cpu_set(d->dbg, flag_ids[id - IDC_FLAG0],
                                   SendMessageA(d->flag[id - IDC_FLAG0], BM_GETCHECK, 0, 0) == BST_CHECKED);
            refresh_cpu(d);
            return 0;
        }
        if (id >= IDC_SAVE0 && id <= IDC_SAVE_LAST) { a2600debug_state_save(d->dbg, id - IDC_SAVE0); refresh_all(d); return 0; }
        if (id >= IDC_LOAD0 && id <= IDC_LOAD_LAST) { a2600debug_state_load(d->dbg, id - IDC_LOAD0); refresh_all(d); return 0; }
        if (id >= IDC_FILE0 && id <= IDC_FILE_LAST) { save_file(d, id - IDC_FILE0); return 0; }
        switch (id) {
        case IDC_RUN: toggle_run(d); return 0;
        case IDC_STEP: a2600debug_step(d->dbg); refresh_all(d); return 0;
        case IDC_TRACE: a2600debug_trace(d->dbg); refresh_all(d); return 0;
        case IDC_SCAN: a2600debug_scanline(d->dbg, 1); refresh_all(d); return 0;
        case IDC_FRAME: a2600debug_frame(d->dbg, 1); refresh_all(d); return 0;
        case IDC_REWIND: a2600debug_rewind(d->dbg, 1); refresh_all(d); return 0;
        case IDC_UNWIND: a2600debug_unwind(d->dbg, 1); refresh_all(d); return 0;
        case IDC_LOAD_SYMBOLS: {
            char m[512];
            a2600debug_load_symbols(d->dbg, m, sizeof m);
            append_text_lf(d->prompt_out, m);
            append_text_lf(d->prompt_out, "\n");
            refresh_all(d);
            return 0;
        }
        case IDC_BANK:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                const int sel = (int)SendMessageA(d->bank, CB_GETCURSEL, 0, 0);
                d->disasm_bank = sel - 1;
                if (sel > 0) SendMessageA(d->follow_pc, BM_SETCHECK, BST_UNCHECKED, 0);
                d->disasm_first = 0;
                refresh_disasm(d);
            }
            return 0;
        case IDC_FOLLOW_PC: refresh_disasm(d); return 0;
        case IDC_TIA_PARTIAL: refresh_tia(d); return 0;
        case IDC_BP_CLEAR: {
            static char out[512];
            a2600debug_breakpoint_clear(d->dbg);
            a2600debug_command(d->dbg, "clearTraps", out, sizeof out);
            refresh_all(d);
            return 0;
        }
        default: break;
        }
        break;
    }
    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);   /* keep state; F12 brings it back */
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, TIMER_REFRESH);
        DeleteObject(d->mono);
        DeleteObject(d->accent);
        DestroyAcceleratorTable(d->accel);
        free(d->tia_px);
        free(d);
        g_dbg = NULL;
        return 0;
    default: break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* ---- entry points ---------------------------------------------------------------------- */

void a2600_debugger_show(HWND parent, a2600session *session)
{
    HINSTANCE inst;
    WNDCLASSA wc;
    debugger *d;
    ACCEL accels[5];
    const char *tab;

    if (g_dbg) {
        ShowWindow(g_dbg->hwnd, SW_SHOW);
        SetForegroundWindow(g_dbg->hwnd);
        a2600debug_stop(g_dbg->dbg);
        refresh_all(g_dbg);
        return;
    }

    inst = (HINSTANCE)GetWindowLongPtrA(parent, GWLP_HINSTANCE);

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = dbg_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "A2600DebuggerWindow";
    RegisterClassA(&wc);

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = pic_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "A2600DbgPixels";
    RegisterClassA(&wc);

    d = calloc(1, sizeof *d);
    if (!d) return;
    d->session = session;
    d->dbg = a2600session_debugger(session);
    d->disasm_bank = -1;
    d->tia_px = calloc((size_t)A2600SESSION_FB_WIDTH * A2600SESSION_FB_MAX_HEIGHT, sizeof *d->tia_px);
    g_dbg = d;

    d->hwnd = CreateWindowExA(0, "A2600DebuggerWindow", "Debugger", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                              CW_USEDEFAULT, CW_USEDEFAULT, 1100, 760, NULL, NULL, inst, NULL);
    if (!d->hwnd) { free(d->tia_px); free(d); g_dbg = NULL; return; }

    d->ui = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    d->mono = CreateFontA(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                          CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");
    d->accent = CreateSolidBrush(RGB((A2600SESSION_ACCENT_RGB >> 16) & 0xff, (A2600SESSION_ACCENT_RGB >> 8) & 0xff,
                                     A2600SESSION_ACCENT_RGB & 0xff));

    build_controls(d);

    /* F5/F7/F8/Shift+F8/F12 work wherever the focus is inside the window. */
    accels[0].fVirt = FVIRTKEY;          accels[0].key = VK_F5;  accels[0].cmd = IDC_RUN;
    accels[1].fVirt = FVIRTKEY;          accels[1].key = VK_F7;  accels[1].cmd = IDC_STEP;
    accels[2].fVirt = FVIRTKEY;          accels[2].key = VK_F8;  accels[2].cmd = IDC_TRACE;
    accels[3].fVirt = FVIRTKEY | FSHIFT; accels[3].key = VK_F8;  accels[3].cmd = IDC_FRAME;
    accels[4].fVirt = FVIRTKEY;          accels[4].key = VK_F12; accels[4].cmd = IDCANCEL;
    d->accel = CreateAcceleratorTableA(accels, 5);

    tab = getenv("A2600_DEBUGGER_TAB");
    d->page = PAGE_PROMPT;
    if (tab && *tab) {
        int t = atoi(tab);
        if (t >= 0 && t < PAGE_COUNT) d->page = t;
    }
    SendMessageA(d->tabs, TCM_SETCURSEL, (WPARAM)d->page, 0);
    show_page(d, d->page);
    layout(d);

    a2600debug_stop(d->dbg);
    d->seen_gen = a2600debug_generation(d->dbg);
    d->was_stopped = 1;
    refresh_all(d);
    SetTimer(d->hwnd, TIMER_REFRESH, 100, NULL);
    ShowWindow(d->hwnd, SW_SHOW);
}

int a2600_debugger_pretranslate(MSG *msg)
{
    if (!g_dbg || !g_dbg->accel) return 0;
    if (msg->hwnd != g_dbg->hwnd && !IsChild(g_dbg->hwnd, msg->hwnd)) return 0;
    if (msg->message == WM_KEYDOWN && msg->wParam == VK_F12) { ShowWindow(g_dbg->hwnd, SW_HIDE); return 1; }
    return TranslateAcceleratorA(g_dbg->hwnd, g_dbg->accel, msg) ? 1 : 0;
}
