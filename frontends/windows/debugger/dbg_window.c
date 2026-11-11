/*
 * Debugger window (Win32) over Stella's own debugger engine (the
 * DebuggerParser, CpuDebug, RiotDebug, TIADebug, CartDebug and DiStella),
 * via core/include/a2600debug.h. Mirrors the GNOME, Qt and SMS debuggers tab
 * for tab -- Console, Disassembly, Memory, Breakpoints, Watches, Cart & I/O,
 * Trace, History, Video, Sound, Symbols, FujiNet -- built from plain common
 * controls, with the disassembly, the memory dump and the video pictures
 * drawn by hand. The registers and flags sit in a strip under the tabs, on
 * every tab.
 *
 * Each tab's controls are children of a container window of their own, so
 * Pop Out can SetParent a tab into a window titled "Atari 2600 Debugger - <Tab>"
 * (Dock, or closing that window, puts it back in its place; one tab always
 * stays).
 *
 * Showing the window (F12, from the main window) stops the machine, as on
 * every sibling, so there is something to look at; hiding it (the close box
 * or F12) lets the machine run on. The toolbar carries a single Stop/Run
 * button (the accent while stopped), the stepping controls (Step, Over, Out,
 * Run to Cursor, Scan+1, Frame+1, Rewind, Unwind), Clear Breakpoints, a
 * "Break at" box, and symbol loading and clearing; F5 F7 F8 Shift+F8 F9 F12
 * work wherever the focus is inside the window (a2600_debugger_pretranslate).
 *
 * A 100 ms timer polls the engine's generation counter (never its stop
 * callback, which fires off the UI thread) and refreshes the tab showing,
 * and only that one, when something changed; while the machine runs it
 * refreshes the live values of that tab twice a second. Every new stop
 * (a2600debug_stop_serial) brings every view to the spot: the Disassembly to
 * the PC, the Memory dump to the PC's neighbourhood, Trace and History to
 * their newest line. After a step the window waits a few milliseconds for
 * the stop to settle so the status does not flash "Running".
 *
 * The Video tab (dbg_video.c, over core/include/a2600vidtab.h: a picture in
 * four views, an inspector, registers, colours, lines, TIA writes and
 * timing) and the FujiNet tab (every FujiBus packet on the BoIP link, both
 * ways) carry over. Address fields take a label, else bare hex, else the
 * engine's grammar ($ or 0x, registers, arithmetic); the register and byte
 * boxes show hex without a prefix and read it back the same way. Only the
 * console reads bare digits as Stella does.
 *
 * A2600_DEBUGGER_TAB (a tab number, or the start of a tab's name or label, in
 * any case) picks the tab the window opens on: 0 Console, 1 Disassembly,
 * 2 Memory, 3 Breakpoints, 4 Watches, 5 Cart & I/O, 6 Trace, 7 History,
 * 8 Video, 9 Sound, 10 Symbols, 11 FujiNet. For captures: A2600_DEBUGGER_WATCH
 * (a comma-separated list of watches to add), A2600_DEBUGGER_WATCH_BREAK (a
 * watch added and a break-on-change armed over it), A2600_DEBUGGER_MEM_GOTO
 * (the Memory tab's Go to), A2600_DEBUGGER_HISTORY=1 (recording on) and
 * A2600_DEBUGGER_POPOUT (a tab to pop out, named like A2600_DEBUGGER_TAB).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "dbg_window.h"

#include <commctrl.h>
#include <commdlg.h>
#include <windowsx.h>

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "a2600debug.h"
#include "a2600memtools.h"
#include "a2600session.h"
#include "dbg_video.h"
#include "resource.h"
#include "../help/help_window.h"

/* A2600SESSION_ACCENT_RGB with A2600SESSION_ACCENT_DARK_RGB ink on it. */
#define A2600_ACCENT_COLORREF RGB((A2600SESSION_ACCENT_RGB >> 16) & 255, (A2600SESSION_ACCENT_RGB >> 8) & 255, A2600SESSION_ACCENT_RGB & 255)
#define A2600_ACCENT_TEXT RGB((A2600SESSION_ACCENT_DARK_RGB >> 16) & 255, (A2600SESSION_ACCENT_DARK_RGB >> 8) & 255, A2600SESSION_ACCENT_DARK_RGB & 255)

static HBRUSH a2600_accent_brush(void)
{
    static HBRUSH brush;
    if (!brush) brush = CreateSolidBrush(A2600_ACCENT_COLORREF);
    return brush;
}

static HBRUSH a2600_accent_hot_brush(void)
{
    static HBRUSH brush;
    if (!brush) brush = CreateSolidBrush(RGB(0xFF, 0xA6 + (255 - 0xA6) / 5, 0x45 + (255 - 0x45) / 5));
    return brush;
}

/* A checkable push button's NM_CUSTOMDRAW: the accent while it is on, the
 * stock look (and focus, hover and press) while off. */
static LRESULT a2600_accent_customdraw(const NMCUSTOMDRAW *cd, int on)
{
    char text[64];
    RECT r;
    if (cd->dwDrawStage != CDDS_PREPAINT || !on) return CDRF_DODEFAULT;
    r = cd->rc;
    FillRect(cd->hdc, &r, (cd->uItemState & CDIS_HOT) ? a2600_accent_hot_brush() : a2600_accent_brush());
    FrameRect(cd->hdc, &r, (HBRUSH)GetStockObject(GRAY_BRUSH));
    SetBkMode(cd->hdc, TRANSPARENT);
    SetTextColor(cd->hdc, A2600_ACCENT_TEXT);
    text[0] = '\0';
    GetWindowTextA(cd->hdr.hwndFrom, text, sizeof text);
    {
        HFONT f = (HFONT)SendMessageA(cd->hdr.hwndFrom, WM_GETFONT, 0, 0);
        HGDIOBJ old = f ? SelectObject(cd->hdc, f) : NULL;
        DrawTextA(cd->hdc, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        if (old) SelectObject(cd->hdc, old);
    }
    if (GetFocus() == cd->hdr.hwndFrom) {
        InflateRect(&r, -3, -3);
        DrawFocusRect(cd->hdc, &r);
    }
    return CDRF_SKIPDEFAULT;
}

#define DISASM_MAX 96        /* rows fetched at most; the view shows what fits */
#define MEM_ROWS 512         /* the 6507's 13-bit bus ($0000-$1FFF), 16 bytes a row */
#define MEM_BYTES (MEM_ROWS * 16)
#define MAX_BPS 128
#define MAX_BANKS 64        /* banks the bank choosers list */
#define MEM_TOP 0x1FFF      /* the last address on the bus */
#define HISTORY_MAX 64
#define TRACE_MAX 512        /* FujiNet packets shown */
#define HIST_RING 65536      /* the engine's instruction ring */
#define HIST_SHOWN 2000      /* the newest matching lines a Trace/History tab shows */
#define HIST_TICKS 4         /* the Trace tab, every ~400 ms while the machine runs */
#define MEM_ADDR_W 6         /* the dump's address column, in characters */
#define MEM_REGION_MAX 64    /* regions of the memory map held */
#define SYMBOL_MAX 4096      /* symbols the Symbols tab lists at most */
#define CONSOLE_KEEP (256 * 1024)
#define NOTICE_MS 4000       /* how long a notice holds the status line */

/* A disassembly row's columns, in characters: address, bytes, label, the
 * instruction, and the comment. */
#define COL_BYTES 6
#define COL_LABEL 19
#define LABEL_W 22
#define COL_DISASM (COL_LABEL + LABEL_W + 1)
#define COMMENT_COL (COL_DISASM + 24)

/* Sizes at 96 dpi; S() scales them to the window's. */
#define GUTTER_W 18          /* the disassembly's breakpoint column */
#define MONO_PX 13           /* the monospace font */
#define START_W 1400
#define START_H 800

#define WM_DBG_ACCEPT  (WM_APP + 2)   /* wp: control id -- Enter in an edit */
#define WM_DBG_TAB     (WM_APP + 3)   /* Tab in the console: complete */
#define WM_DBG_HISTORY (WM_APP + 4)   /* wp: 0 Up (older), 1 Down (newer) in the console */
#define WM_DBG_SYNC    (WM_APP + 5)   /* resync once a list's notification is over */

#define TIMER_REFRESH 1

#define DBG_CLASS "A2600DebuggerWindow"
#define DISASM_CLASS "A2600DbgDisasm"
#define MEMVIEW_CLASS "A2600DbgMem"
#define PAGE_CLASS "A2600DbgPage"
#define POP_CLASS "A2600DbgPop"

enum { TAB_CONSOLE, TAB_DISASM, TAB_MEMORY, TAB_BREAKS, TAB_WATCHES, TAB_CART, TAB_TRACE, TAB_HISTORY,
       TAB_VIDEO, TAB_SOUND, TAB_SYMBOLS, TAB_FUJINET, TAB_COUNT };

/* The tabs, in order: the names A2600_DEBUGGER_TAB takes, and their labels
 * (which it takes too). */
static const struct { const char *name, *label; } k_tabs[TAB_COUNT] = {
    { "console", "Console" }, { "disassembly", "Disassembly" }, { "memory", "Memory" },
    { "breakpoints", "Breakpoints" }, { "watches", "Watches" }, { "cart", "Cart & I/O" },
    { "trace", "Trace" }, { "history", "History" }, { "video", "Video" }, { "sound", "Sound" },
    { "symbols", "Symbols" }, { "fujinet", "FujiNet" },
};

#define NREGS 6             /* the editable registers */
#define NCHECKS 7           /* the seven flags */
#define NSAVE 5
#define NSLOTS 10           /* state slots */

enum {
    IDC_RUN = 1000, IDC_STEP, IDC_OVER, IDC_OUT, IDC_RUN_TO, IDC_SCAN, IDC_FRAME, IDC_REWIND, IDC_UNWIND, IDC_SYMBOLS,
    IDC_STATUS, IDC_TABS, IDC_TOGGLE_BP, IDC_HELP_BTN, IDC_POPOUT, IDC_DOCK, IDC_CLEAR_BPS,
    IDC_BREAK_AT, IDC_CLEAR_SYMBOLS, IDC_SYM_FILTER, IDC_SYM_LIST,
    IDC_CON_OUT, IDC_CON_PROMPT, IDC_CON_IN,
    IDC_REG0, IDC_REG_LAST = IDC_REG0 + NREGS - 1,
    IDC_CHK0, IDC_CHK_LAST = IDC_CHK0 + NCHECKS - 1,
    IDC_MEM_VIEW, IDC_MEM_GOTO, IDC_MEM_WRITE,
    IDC_FOLLOW_PC, IDC_JUMP, IDC_DISASM, IDC_BANK, IDC_BP_BANK,
    IDC_BP_LIST, IDC_BP_REMOVE, IDC_BP_CLEAR, IDC_BP_TYPE, IDC_BP_START, IDC_BP_END, IDC_BP_COND,
    IDC_BP_ADD, IDC_BP_CLEAR_HITS,
    IDC_WT_ENTRY, IDC_WT_ADD, IDC_WT_LIST, IDC_WT_REMOVE, IDC_WT_MEM, IDC_WT_BRK,
    IDC_SRCH_KIND, IDC_SRCH_ENTRY, IDC_SRCH_NEXT, IDC_SRCH_PREV,
    IDC_CART_TEXT, IDC_SOUND_TEXT,
    IDC_H_REC0, IDC_H_REC1, IDC_H_CLEAR0, IDC_H_CLEAR1, IDC_H_FILTER0, IDC_H_FILTER1, IDC_H_RANGE0, IDC_H_RANGE1,
    IDC_H_TEXT0, IDC_H_TEXT1,
    IDC_FUJI_CLEAR, IDC_FUJI_TEXT, IDC_FUJI_STATE,
    IDC_SAVE_KIND0, IDC_SAVE_KIND_LAST = IDC_SAVE_KIND0 + NSAVE - 1,
    IDC_SLOT_SAVE0, IDC_SLOT_SAVE_LAST = IDC_SLOT_SAVE0 + NSLOTS - 1,
    IDC_SLOT_LOAD0, IDC_SLOT_LOAD_LAST = IDC_SLOT_LOAD0 + NSLOTS - 1,
    IDC_TIA_REG, IDC_TIA_VAL, IDC_TIA_SET,
    IDC_LABEL_FIRST
};

typedef struct {
    HWND hwnd, tabs;
    HWND run_btn, step_btn, over_btn, out_btn, run_to_btn, scan_btn, frame_btn, rewind_btn, unwind_btn, clear_bps_btn,
         break_at_label, break_at, sym_btn, clear_sym_btn, popout_btn, help_btn, status;

    /* Each tab's controls are children of a container of its own, so a tab can
     * be peeled off into a window (SetParent) and put back. */
    HWND pg[TAB_COUNT];
    HWND popped[TAB_COUNT];     /* the window a tab lives in, or NULL */
    int tab_page[TAB_COUNT];    /* the tab at each tab-control index */
    int ntabs;
    HWND new_parent;            /* where child() puts the next control */

    /* console */
    HWND con_out, con_prompt, con_in;
    char history[HISTORY_MAX][256];
    int nhistory, history_pos;

    /* the registers strip, under the tabs */
    HWND reg_label[NREGS], reg[NREGS], chk[NCHECKS], beam;
    int strip_rows;             /* register rows the strip's last layout used */

    /* memory */
    HWND mem_title, mem_view, goto_label, mem_goto, write_label, mem_write;
    HWND srch_kind, srch_entry, srch_next, srch_prev, srch_note;
    int mem_hl;                 /* a highlighted range (Go to, Find, a watch) */
    uint16_t mem_hl_lo, mem_hl_hi;
    uint8_t mem_buf[MEM_BYTES]; /* what the dump shows */
    a2600debug_region mem_map[MEM_REGION_MAX];   /* the memory map the rows are labelled from */
    int mem_map_count;
    int mem_first;              /* the dump's first visible text line */
    int mem_follow;             /* scroll the highlight into view on the next refresh */
    int mem_recentre;           /* a stop happened: bring the PC's row into view */

    /* disassembly */
    HWND bank, follow_pc, jump_label, jump, disasm_hint, disasm;
    int disasm_bank;            /* the bank listed, -1 = the PC's */
    int disasm_first;           /* index of the first line shown in the bank's listing */
    int disasm_total;           /* lines in the bank's listing */
    int pc_listed;              /* the PC's line index in the listing, -1 if not in this bank */
    int recentre;               /* a stop happened: bring the PC into view */
    uint64_t stop_serial;       /* the last stop the window followed */
    int selected;               /* the selected line's address, or -1 */
    a2600debug_line lines[DISASM_MAX];
    int line_count, pc_line;

    /* video: the page holds its own controls (dbg_video.c) */
    HWND video;

    /* sound */
    HWND sound_text;

    /* symbols */
    HWND sym_filter_label, sym_filter, sym_count, sym_list;
    int sym_indices[SYMBOL_MAX];
    int sym_shown;
    char sym_sig[128];
    int sym_force;              /* rebuild the list even if its signature held (symbols loaded) */

    /* breakpoints */
    HWND bp_hint, bp_stop, bp_remove, bp_clear, bp_clear_hits, bp_list, bp_empty;
    HWND bp_type, bp_start, bp_end, bp_bank, bp_cond, bp_add, bp_msg;
    a2600debug_breakpoint bps[MAX_BPS];
    int nbps;
    char bp_sig[MAX_BPS * 200]; /* what the list shows, to rebuild on change */
    int bp_sig_ok;

    /* watches */
    HWND wt_label, wt_entry, wt_add, wt_list, wt_remove, wt_mem, wt_brk, wt_msg;
    char wt_sig[A2600WATCH_MAX * 120];
    int wt_sig_ok;

    /* cart & I/O */
    HWND cart_text, states_label, slot_save[NSLOTS], slot_load[NSLOTS], files_label, file_btn[NSAVE];
    HWND tia_label, tia_reg, tia_val, tia_set;

    /* trace (0) and history (1) share the engine's ring */
    HWND h_rec[2], h_clear[2], h_filter[2], h_range[2], h_note[2], h_text[2];
    uint64_t h_sig[2];
    int h_ok[2];
    int h_ticks;

    /* fujinet */
    HWND fuji_title, fuji_note, fuji_clear, fuji_text, fuji_state_title, fuji_state;
    uint64_t fuji_last;
    int fuji_count;

    HWND views[24];             /* the read-only text views, drawn on the window colour */
    int nviews;
    HWND dim[64];               /* captions and hints, drawn grey */
    int ndim, nlabels;

    HFONT ui, ui_bold, mono;
    int dpi;
    int mono_w, mono_h, line_h;
    HACCEL accel;
    HBRUSH bp_on;

    a2600session *session;
    a2600debug *dbg;

    int page;
    unsigned seen_gen;
    int was_stopped;
    int running_ticks;
    int updating;               /* a refresh is setting controls */

    char notice[200];           /* a message holding the status line a moment */
    DWORD notice_until;
} debugger;

static debugger *g_dbg;

/* The editable registers, in the order the strip lays them out. */
static const char *const k_reg_names[NREGS] = { "PC", "SP", "A", "X", "Y", "P" };
static const int k_reg_ids[NREGS] = { A2600_REG_PC, A2600_REG_SP, A2600_REG_A, A2600_REG_X, A2600_REG_Y,
                                      A2600_REG_PS };

/* The check boxes: the flags in P. */
static const char *const k_chk_names[NCHECKS] = { "N", "V", "B", "D", "I", "Z", "C" };
static const int k_chk_ids[NCHECKS] = { A2600_FLAG_N, A2600_FLAG_V, A2600_FLAG_B, A2600_FLAG_D, A2600_FLAG_I,
                                        A2600_FLAG_Z, A2600_FLAG_C };

static const char *const k_save_kinds[NSAVE] = { "dis", "rom", "access", "ses", "snap" };
static const char *const k_save_titles[NSAVE] = { "Disassembly...", "ROM (patched)...", "Access counters...",
                                                  "Session...", "Snapshot..." };

#define NBPTYPES 4
static const char *const k_bp_types[NBPTYPES] = { "Execute", "Read", "Write", "Access" };
static const int k_bp_type_ids[NBPTYPES] = { A2600DEBUG_BP_EXEC, A2600DEBUG_BP_READ, A2600DEBUG_BP_WRITE,
                                             A2600DEBUG_BP_ACCESS };

/* ---- helpers ---------------------------------------------------------------- */

static void edit_text(HWND h, char *out, int outsz) { GetWindowTextA(h, out, outsz); }

static void wide(const char *utf8, wchar_t *out, int outn)
{
    if (outn <= 0) return;
    out[0] = 0;
    if (utf8 && MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out, outn) <= 0) out[0] = 0;
    out[outn - 1] = 0;
}

/* A label set only when it changed (no flicker on the live refresh). */
static void set_label(HWND h, const char *utf8)
{
    wchar_t cur[512], w[512];
    wide(utf8, w, 512);
    GetWindowTextW(h, cur, 512);
    if (wcscmp(cur, w) != 0) SetWindowTextW(h, w);
}

/* EDIT controls want CR LF and draw control characters as boxes: engine
 * text (UTF-8) as the wide text a view shows. */
static wchar_t *wide_view_text(const char *text)
{
    size_t n = strlen(text), i, o = 0;
    char *buf = malloc(n * 2 + 1);
    wchar_t *w = NULL;
    int wn;
    if (!buf) return NULL;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c == '\n') { buf[o++] = '\r'; buf[o++] = '\n'; }
        else if (c >= 0xC2 && c <= 0xF4) {
            /* a UTF-8 lead byte: kept with its continuation bytes if they are all there (Stella marks some of
             * its text with single bytes that are not UTF-8; those are dropped) */
            const size_t extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
            size_t k, ok = i + extra < n + 0 ? 1 : 0;
            for (k = 1; ok && k <= extra; k++)
                if ((((unsigned char)text[i + k]) & 0xC0) != 0x80) ok = 0;
            if (ok) {
                for (k = 0; k <= extra; k++) buf[o++] = text[i + k];
                i += extra;
            }
        }
        else if (c >= 0x20 && c < 0x80) buf[o++] = (char)c;
        else if (c == '\t') buf[o++] = (char)c;
    }
    buf[o] = '\0';
    wn = MultiByteToWideChar(CP_UTF8, 0, buf, -1, NULL, 0);
    if (wn > 0 && (w = malloc((size_t)wn * sizeof *w)) != NULL)
        MultiByteToWideChar(CP_UTF8, 0, buf, -1, w, wn);
    free(buf);
    return w;
}

static size_t addf(char *s, size_t len, size_t cap, const char *fmt, ...)
{
    va_list ap;
    int n;
    if (len >= cap) return len;
    va_start(ap, fmt);
    n = vsnprintf(s + len, cap - len, fmt, ap);
    va_end(ap);
    if (n < 0) return len;
    return len + (size_t)n < cap ? len + (size_t)n : cap - 1;
}

/* Blanks off both ends, in place. */
static char *strip(char *s)
{
    char *e;
    while (*s == ' ' || *s == '\t') s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = '\0';
    return s;
}

/* An expression through the engine's range grammar (labels, $ and 0x,
 * registers, label+n, [ptr]); one value. 0 and *value, or -1 with the
 * reason in err. */
static int eval_expression(debugger *d, const char *text, long *value, char *err, int errsz)
{
    uint16_t low, high;
    if (!a2600sym_parse_range(d->dbg, text, &low, &high)) {
        snprintf(err, (size_t)errsz, "Not an address, label or expression");
        return -1;
    }
    *value = low;
    return 0;
}

/* An address field: a label first, then bare digits as hex (as the dumps
 * show them), else the engine's grammar ($ or 0x, registers, arithmetic),
 * wrapped to 16 bits. -1 with the reason in err when it is none of them.
 * (The console keeps the engine's own rule: bare digits decimal.) */
static int resolve_addr(debugger *d, const char *text, char *err, int errsz)
{
    char buf[128], *s, *end;
    long v = 0;
    int a;
    snprintf(buf, sizeof buf, "%s", text);
    s = strip(buf);
    if (!*s) {
        snprintf(err, (size_t)errsz, "Nothing typed");
        return -1;
    }
    err[0] = '\0';
    a = a2600debug_label_address(d->dbg, s);
    if (a >= 0) return a & 0xFFFF;
    v = strtol(s, &end, 16);
    if (end != s && !*end && *s != '-' && *s != '+') return (int)(v & 0xFFFF);
    if (eval_expression(d, s, &v, err, errsz) != 0) {
        if (!err[0]) snprintf(err, (size_t)errsz, "Not an address or label");
        return -1;
    }
    return (int)(v & 0xFFFF);
}

/* A range field: "lo-hi" or "lo..hi" (bare digits hex, as the dumps show
 * them), else a label, an address or an expression (one byte). The engine's
 * own range grammar (labels, $ and 0x, label+n) is the fallback. */
static int hex_only(const char *t, size_t n)
{
    size_t i;
    if (!n) return 0;
    for (i = 0; i < n; i++)
        if (!((t[i] >= '0' && t[i] <= '9') || (t[i] >= 'a' && t[i] <= 'f') || (t[i] >= 'A' && t[i] <= 'F'))) return 0;
    return 1;
}

static int parse_range_ui(debugger *d, const char *text, uint16_t *lo, uint16_t *hi, char *err, int errsz)
{
    char buf[128], *s, *sep = NULL;
    size_t split_len = 1;
    int a;
    snprintf(buf, sizeof buf, "%s", text);
    s = strip(buf);
    if (!*s) {
        snprintf(err, (size_t)errsz, "Nothing typed");
        return 0;
    }
    sep = strstr(s, "..");
    if (sep) split_len = 2;
    else sep = strchr(s + 1, '-');
    if (sep && sep > s) {
        char *left = s, *right = sep + split_len;
        *sep = '\0';
        left = strip(left);
        right = strip(right);
        if (hex_only(left, strlen(left)) && hex_only(right, strlen(right))) {
            long x = strtol(left, NULL, 16) & 0xFFFF, y = strtol(right, NULL, 16) & 0xFFFF;
            *lo = (uint16_t)(x < y ? x : y);
            *hi = (uint16_t)(x < y ? y : x);
            return 1;
        }
        *sep = split_len == 2 ? '.' : '-';          /* put the text back for the engine */
    }
    if (strstr(s, "..") || strchr(s + 1, '-') || strchr(s, '+')) {
        if (a2600sym_parse_range(d->dbg, s, lo, hi)) return 1;
        snprintf(err, (size_t)errsz, "Not an address, label or range");
        return 0;
    }
    a = resolve_addr(d, s, err, errsz);
    if (a < 0) return 0;
    *lo = *hi = (uint16_t)a;
    return 1;
}

/* A register or byte box: hex without a prefix, as the boxes show it, or
 * else the engine's grammar. */
static int parse_hexish(debugger *d, const char *text, long *out)
{
    char buf[64], err[64], *s, *end;
    long v;
    snprintf(buf, sizeof buf, "%s", text);
    s = strip(buf);
    if (!*s) return 0;
    v = strtol(s, &end, 16);
    if (end != s && !*end) {
        *out = v;
        return 1;
    }
    err[0] = '\0';
    return eval_expression(d, s, out, err, sizeof err) == 0;
}

static const char *base_name(const char *path)
{
    const char *a = strrchr(path, '\\'), *b = strrchr(path, '/');
    if (b && (!a || b > a)) a = b;
    return a ? a + 1 : path;
}

/* ---- scale and fonts --------------------------------------------------------- */

static int system_dpi(void)
{
    HDC dc = GetDC(NULL);
    int v = dc ? GetDeviceCaps(dc, LOGPIXELSY) : 96;
    if (dc) ReleaseDC(NULL, dc);
    return v > 0 ? v : 96;
}

/* The window's DPI (Windows 10 1607+), else the system's. */
static int window_dpi(HWND hwnd)
{
    typedef UINT (WINAPI *dpi_fn)(HWND);
    static dpi_fn fn;
    static int looked;
    if (!looked) {
        HMODULE user = GetModuleHandleA("user32.dll");
        fn = user ? (dpi_fn)(void (*)(void))GetProcAddress(user, "GetDpiForWindow") : NULL;
        looked = 1;
    }
    if (fn && hwnd) {
        const UINT v = fn(hwnd);
        if (v) return (int)v;
    }
    return system_dpi();
}

static int S(const debugger *d, int v) { return MulDiv(v, d->dpi, 96); }

/* The message font at dpi (system metrics come at the system DPI), at its
 * own size (px 0) or px pixels at 96 dpi; face NULL keeps the message
 * font's, otherwise a fixed-pitch face (Consolas). */
static HFONT make_font(int dpi, int px, int weight, const char *face)
{
    NONCLIENTMETRICSA ncm;
    LOGFONTA lf;
    ZeroMemory(&ncm, sizeof ncm);
    ncm.cbSize = sizeof ncm;
    if (SystemParametersInfoA(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0))
        lf = ncm.lfMessageFont;
    else
        GetObjectA(GetStockObject(DEFAULT_GUI_FONT), sizeof lf, &lf);
    if (px > 0) lf.lfHeight = -MulDiv(px, dpi, 96);
    else lf.lfHeight = -MulDiv(abs(lf.lfHeight ? lf.lfHeight : 12), dpi, system_dpi());
    lf.lfWidth = 0;
    lf.lfWeight = weight;
    if (face) {
        snprintf(lf.lfFaceName, sizeof lf.lfFaceName, "%s", face);
        lf.lfPitchAndFamily = FIXED_PITCH | FF_MODERN;
    }
    return CreateFontIndirectA(&lf);
}

static BOOL CALLBACK refont_child(HWND h, LPARAM lp)
{
    const HFONT *map = (const HFONT *)lp;     /* old ui, bold, mono; new ui, bold, mono */
    const HFONT f = (HFONT)SendMessageA(h, WM_GETFONT, 0, 0);
    int i;
    for (i = 0; i < 3; i++)
        if (f && f == map[i]) SendMessageA(h, WM_SETFONT, (WPARAM)map[i + 3], TRUE);
    return TRUE;
}

/* The message font and Consolas at the window's DPI; the controls already
 * made take the new ones. */
static void make_fonts(debugger *d)
{
    HFONT old[3] = { d->ui, d->ui_bold, d->mono };
    HDC dc;
    TEXTMETRICA tm;
    HGDIOBJ prev;
    int i;

    d->ui = make_font(d->dpi, 0, FW_NORMAL, NULL);
    d->ui_bold = make_font(d->dpi, 0, FW_BOLD, NULL);
    d->mono = make_font(d->dpi, MONO_PX, FW_NORMAL, "Consolas");

    /* the monospace cell, for the hand-drawn views */
    dc = GetDC(d->hwnd);
    prev = SelectObject(dc, d->mono);
    d->mono_w = S(d, 7);
    d->mono_h = S(d, 15);
    if (GetTextMetricsA(dc, &tm)) {
        d->mono_w = tm.tmAveCharWidth;
        d->mono_h = tm.tmHeight;
    }
    d->line_h = d->mono_h + S(d, 2);
    SelectObject(dc, prev);
    ReleaseDC(d->hwnd, dc);

    if (old[0]) {
        HFONT map[6] = { old[0], old[1], old[2], d->ui, d->ui_bold, d->mono };
        int p;
        EnumChildWindows(d->hwnd, refont_child, (LPARAM)map);
        for (p = 0; p < TAB_COUNT; p++)
            if (d->popped[p]) EnumChildWindows(d->popped[p], refont_child, (LPARAM)map);
        if (d->video) dbg_video_fonts(d->video, d->ui, d->mono, d->dpi);
    }
    for (i = 0; i < 3; i++)
        if (old[i]) DeleteObject(old[i]);
}

static int text_w(debugger *d, HFONT f, const char *text)
{
    HDC dc = GetDC(d->hwnd);
    HGDIOBJ old = SelectObject(dc, f);
    SIZE sz = { 0, 0 };
    GetTextExtentPoint32A(dc, text, (int)strlen(text), &sz);
    SelectObject(dc, old);
    ReleaseDC(d->hwnd, dc);
    return sz.cx;
}

static int btn_w(debugger *d, const char *text)
{
    const int w = text_w(d, d->ui, text) + S(d, 24);
    return w < S(d, 64) ? S(d, 64) : w;
}

static int in_list(HWND h, const HWND *list, int n)
{
    int i;
    for (i = 0; i < n; i++)
        if (list[i] == h) return 1;
    return 0;
}

/* ---- the text views ------------------------------------------------------------ */

/* The rows an EDIT view shows. */
static int edit_rows(debugger *d, HWND h)
{
    RECT c;
    GetClientRect(h, &c);
    return d->mono_h > 0 && c.bottom > d->mono_h ? c.bottom / d->mono_h : 1;
}

/* Whether a log's reader is at its end (it then follows new lines). */
static int edit_at_end(debugger *d, HWND h)
{
    const int first = (int)SendMessageW(h, EM_GETFIRSTVISIBLELINE, 0, 0);
    const int count = (int)SendMessageW(h, EM_GETLINECOUNT, 0, 0);
    return first + edit_rows(d, h) >= count;
}

static void edit_to_end(HWND h)
{
    SendMessageW(h, EM_LINESCROLL, 0, SendMessageW(h, EM_GETLINECOUNT, 0, 0));
}

static void edit_scroll_to(HWND h, int first)
{
    SendMessageW(h, EM_LINESCROLL, 0, first - (int)SendMessageW(h, EM_GETFIRSTVISIBLELINE, 0, 0));
}

/* Replaces a view's text where the reader left it (a refresh must not throw
 * a scrolled view back to its top), or, following, at its end. */
static void set_text_at(HWND h, const char *text, int follow)
{
    const int first = (int)SendMessageW(h, EM_GETFIRSTVISIBLELINE, 0, 0);
    wchar_t *w = wide_view_text(text);
    if (!w) return;
    SendMessageW(h, WM_SETREDRAW, FALSE, 0);
    SetWindowTextW(h, w);
    if (follow) edit_to_end(h);
    else edit_scroll_to(h, first);
    SendMessageW(h, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(h, NULL, TRUE);
    free(w);
}

static void refresh_status(debugger *d);

static int notice_showing(debugger *d)
{
    return d->notice[0] && (LONG)(GetTickCount() - d->notice_until) < 0;
}

/* The other frontends' toasts: the message holds the status line a moment. */
static void toast(debugger *d, const char *msg)
{
    snprintf(d->notice, sizeof d->notice, "%s", msg);
    d->notice_until = GetTickCount() + NOTICE_MS;
    refresh_status(d);
}

/* ---- the console ---------------------------------------------------------------- */

/* Long sessions keep the last CONSOLE_KEEP characters, whole lines. */
static void console_trim(debugger *d)
{
    const int len = GetWindowTextLengthW(d->con_out);
    wchar_t *all, *nl;
    int cut;
    if (len <= CONSOLE_KEEP) return;
    all = malloc(((size_t)len + 1) * sizeof *all);
    if (!all) return;
    GetWindowTextW(d->con_out, all, len + 1);
    cut = len - CONSOLE_KEEP * 3 / 4;
    nl = wcschr(all + cut, L'\n');
    if (nl) cut = (int)(nl - all) + 1;
    free(all);
    SendMessageW(d->con_out, EM_SETSEL, 0, (LPARAM)cut);
    SendMessageW(d->con_out, EM_REPLACESEL, FALSE, (LPARAM)L"");
}

/* Onto the console's transcript, which follows its end only while the
 * reader is there. */
static void console_append(debugger *d, const char *text)
{
    const int follow = edit_at_end(d, d->con_out);
    const int first = (int)SendMessageW(d->con_out, EM_GETFIRSTVISIBLELINE, 0, 0);
    wchar_t *w = wide_view_text(text);
    int len;
    if (!w) return;
    len = GetWindowTextLengthW(d->con_out);
    SendMessageW(d->con_out, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    SendMessageW(d->con_out, EM_REPLACESEL, FALSE, (LPARAM)w);
    free(w);
    console_trim(d);
    if (follow) edit_to_end(d->con_out);
    else edit_scroll_to(d->con_out, first);
}

/* Command output, ended with a newline. */
static void console_output(debugger *d, const char *text)
{
    const size_t n = strlen(text);
    if (!n) return;
    console_append(d, text);
    if (text[n - 1] != '\n') console_append(d, "\n");
}

/* Messages from Load Symbols and Save land in the console and, since it may
 * not be the tab showing, on the status line. */
static void say(debugger *d, const char *msg)
{
    console_output(d, msg);
    toast(d, msg);
}

/* ---- refreshers -------------------------------------------------------------- */

static void refresh_status(debugger *d)
{
    char reason[160], text[220];
    int addr;
    const int stopped = a2600debug_is_stopped(d->dbg) != 0;

    if (notice_showing(d)) {
        snprintf(text, sizeof text, "%s", d->notice);
    } else if (stopped) {
        reason[0] = '\0';
        a2600debug_stop_reason(d->dbg, reason, sizeof reason, &addr);
        snprintf(text, sizeof text, "Stopped%s%s", reason[0] ? ": " : "", reason);
    } else {
        snprintf(text, sizeof text, "Running");
    }
    set_label(d->status, text);
    set_label(d->run_btn, stopped ? "Run (F5)" : "Stop (F5)");
    InvalidateRect(d->run_btn, NULL, FALSE);
    InvalidateRect(d->status, NULL, TRUE);
}

static void refresh_cpu(debugger *d)
{
    a2600debug_cpu c;
    char buf[96];
    int i;
    a2600debug_cpu_get(d->dbg, &c);
    {
        const int v[NREGS] = { c.pc, c.sp, c.a, c.x, c.y, c.ps };
        for (i = 0; i < NREGS; i++) {
            if (GetFocus() == d->reg[i]) continue;   /* do not fight the user's typing */
            /* PC as four digits, the rest as two */
            snprintf(buf, sizeof buf, i == 0 ? "%04X" : "%02X", v[i] & (i == 0 ? 0xFFFF : 0xFF));
            set_label(d->reg[i], buf);
        }
    }
    {
        const int on[NCHECKS] = { c.n, c.v, c.b, c.d, c.i, c.z, c.c };
        d->updating = 1;
        for (i = 0; i < NCHECKS; i++)
            SendMessageA(d->chk[i], BM_SETCHECK, on[i] ? BST_CHECKED : BST_UNCHECKED, 0);
        d->updating = 0;
    }
    {
        a2600debug_tia tia;
        a2600debug_tia_get(d->dbg, &tia);
        snprintf(buf, sizeof buf, "Line %d   Clock %d   Frame %d   Cycles %llu (last %d)", tia.scanlines,
                 tia.clocks_this_line, tia.frame_count, (unsigned long long)c.total_cycles, c.cycles);
    }
    set_label(d->beam, buf);
}

/* ---- the memory dump ------------------------------------------------------------ */

/* An EDIT cannot give a byte range its own background, so the dump is a
 * small window class: a header line and MEM_ROWS rows of 16 bytes, painted by
 * hand, scrolled on its own scroll bar, the highlighted range (Go to, Find,
 * a watch) in the accent. */
#define MEM_LINES (MEM_ROWS + 1)

static int mem_vis_rows(debugger *d)
{
    RECT c;
    GetClientRect(d->mem_view, &c);
    return d->line_h > 0 && c.bottom > d->line_h ? c.bottom / d->line_h : 1;
}

static void mem_sync_scroll(debugger *d)
{
    SCROLLINFO si;
    const int vis = mem_vis_rows(d);
    int max_first = MEM_LINES - vis;
    if (max_first < 0) max_first = 0;
    if (d->mem_first > max_first) d->mem_first = max_first;
    if (d->mem_first < 0) d->mem_first = 0;
    memset(&si, 0, sizeof si);
    si.cbSize = sizeof si;
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = MEM_LINES - 1;
    si.nPage = (UINT)vis;
    si.nPos = d->mem_first;
    SetScrollInfo(d->mem_view, SB_VERT, &si, TRUE);
}

static void mem_scroll_to(debugger *d, int first)
{
    d->mem_first = first;
    mem_sync_scroll(d);
    InvalidateRect(d->mem_view, NULL, FALSE);
}

static void paint_mem(debugger *d, HDC dc, const RECT *c)
{
    const int x0 = S(d, 4), cw = d->mono_w;
    int i, col;
    FillRect(dc, c, GetSysColorBrush(COLOR_WINDOW));
    SelectObject(dc, d->mono);
    SetBkMode(dc, TRANSPARENT);
    for (i = 0; (d->mem_first + i) < MEM_LINES && i * d->line_h < c->bottom; i++) {
        const int line = d->mem_first + i, y = i * d->line_h, ty = y + (d->line_h - d->mono_h) / 2;
        char t[8];
        if (line == 0) {
            SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
            for (col = 0; col < 16; col++) {
                const int len = snprintf(t, sizeof t, "%X", col);
                TextOutA(dc, x0 + cw * (MEM_ADDR_W + col * 3 + (col >= 8)), ty, t, len);
            }
        } else {
            const int base = (line - 1) * 16;
            const uint8_t *r = d->mem_buf + (line - 1) * 16;
            int len = snprintf(t, sizeof t, "%04X", base);
            SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
            TextOutA(dc, x0, ty, t, len);
            for (col = 0; col < 16; col++) {
                const int hl = d->mem_hl && base + col >= d->mem_hl_lo && base + col <= d->mem_hl_hi;
                const int bx = x0 + cw * (MEM_ADDR_W + col * 3 + (col >= 8));
                const int ax = x0 + cw * (MEM_ADDR_W + 16 * 3 + 2 + col);
                char a = (char)(r[col] >= 0x20 && r[col] < 0x7F ? r[col] : '.');
                if (hl) {
                    RECT b1 = { bx - 1, y, bx + 2 * cw + 1, y + d->line_h };
                    RECT b2 = { ax, y, ax + cw, y + d->line_h };
                    FillRect(dc, &b1, a2600_accent_brush());
                    FillRect(dc, &b2, a2600_accent_brush());
                    SetTextColor(dc, A2600_ACCENT_TEXT);
                } else {
                    SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
                }
                len = snprintf(t, sizeof t, "%02X", r[col]);
                TextOutA(dc, bx, ty, t, len);
                TextOutA(dc, ax, ty, &a, 1);
            }
            {
                char region[64];
                const int rx = x0 + cw * (MEM_ADDR_W + 16 * 3 + 2 + 16 + 2);
                region[0] = '\0';
                a2600mem_region_name(d->mem_map, d->mem_map_count, (uint16_t)base, region, sizeof region);
                SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
                TextOutA(dc, rx, ty, region, (int)strlen(region));
            }
        }
    }
}

static LRESULT CALLBACK memview_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    debugger *d = g_dbg;
    if (!d) return DefWindowProcA(hwnd, msg, wp, lp);
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps), mem;
        RECT c;
        HBITMAP bmp;
        HGDIOBJ oldbmp;
        GetClientRect(hwnd, &c);
        mem = CreateCompatibleDC(dc);
        bmp = CreateCompatibleBitmap(dc, c.right > 0 ? c.right : 1, c.bottom > 0 ? c.bottom : 1);
        oldbmp = SelectObject(mem, bmp);
        paint_mem(d, mem, &c);
        BitBlt(dc, 0, 0, c.right, c.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldbmp);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_PRINTCLIENT: {
        RECT c;
        GetClientRect(hwnd, &c);
        paint_mem(d, (HDC)wp, &c);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        if (d->mem_view == hwnd) mem_sync_scroll(d);
        return 0;
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS;
    case WM_LBUTTONDOWN:
        SetFocus(hwnd);
        return 0;
    case WM_MOUSEWHEEL: {
        const int delta = GET_WHEEL_DELTA_WPARAM(wp);
        int rows = -delta * 3 / WHEEL_DELTA;
        if (!rows) rows = delta > 0 ? -1 : 1;
        mem_scroll_to(d, d->mem_first + rows);
        return 0;
    }
    case WM_VSCROLL: {
        SCROLLINFO si;
        const int vis = mem_vis_rows(d);
        int first = d->mem_first;
        memset(&si, 0, sizeof si);
        si.cbSize = sizeof si;
        si.fMask = SIF_TRACKPOS;
        GetScrollInfo(hwnd, SB_VERT, &si);
        switch (LOWORD(wp)) {
        case SB_LINEUP: first--; break;
        case SB_LINEDOWN: first++; break;
        case SB_PAGEUP: first -= vis; break;
        case SB_PAGEDOWN: first += vis; break;
        case SB_TOP: first = 0; break;
        case SB_BOTTOM: first = MEM_LINES; break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: first = si.nTrackPos; break;
        default: break;
        }
        mem_scroll_to(d, first);
        return 0;
    }
    case WM_KEYDOWN: {
        const int vis = mem_vis_rows(d);
        int first = d->mem_first;
        switch (wp) {
        case VK_UP: first--; break;
        case VK_DOWN: first++; break;
        case VK_PRIOR: first -= vis; break;
        case VK_NEXT: first += vis; break;
        case VK_HOME: first = 0; break;
        case VK_END: first = MEM_LINES; break;
        default: return DefWindowProcA(hwnd, msg, wp, lp);
        }
        mem_scroll_to(d, first);
        return 0;
    }
    default: break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static void refresh_mem(debugger *d)
{
    memset(d->mem_buf, 0, sizeof d->mem_buf);
    a2600debug_read(d->dbg, 0, d->mem_buf, MEM_BYTES);
    d->mem_map_count = a2600debug_memory_map(d->dbg, d->mem_map, MEM_REGION_MAX);
    if (d->mem_map_count < 0) d->mem_map_count = 0;
    if (d->mem_recentre) {
        /* a new stop: the dump to the PC, a third of the way down */
        a2600debug_cpu cpu;
        a2600debug_cpu_get(d->dbg, &cpu);
        d->mem_recentre = 0;
        mem_scroll_to(d, 1 + (cpu.pc & MEM_TOP) / 16 - mem_vis_rows(d) / 3);
    }
    mem_sync_scroll(d);
    InvalidateRect(d->mem_view, NULL, FALSE);
}

/* lo..hi highlighted and scrolled to in the dump (a Go to, a Find, a
 * watch's Memory button). */
static void show_in_memory(debugger *d, uint16_t lo, uint16_t hi)
{
    int line;
    lo &= MEM_TOP;                 /* the bus has 13 address lines */
    hi &= MEM_TOP;
    if (hi < lo) hi = lo;
    d->mem_hl = 1;
    d->mem_hl_lo = lo;
    d->mem_hl_hi = hi;
    refresh_mem(d);
    /* the row 30% of the way down */
    line = 1 + (int)lo / 16 - mem_vis_rows(d) * 3 / 10;
    mem_scroll_to(d, line > 0 ? line : 0);
}

/* The disassembly rows the view has room for. */
static int disasm_rows(debugger *d)
{
    RECT c;
    int rows;
    if (!d->disasm || d->line_h <= 0) return 1;
    GetClientRect(d->disasm, &c);
    rows = (c.bottom - c.top) / d->line_h;
    if (rows < 1) rows = 1;
    if (rows > DISASM_MAX) rows = DISASM_MAX;
    return rows;
}

static int following(debugger *d)
{
    return SendMessageA(d->follow_pc, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

/* The bank a line's breakpoint belongs to: the one listed, or the PC's. */
static int listed_bank(debugger *d)
{
    return d->disasm_bank < 0 ? a2600debug_current_bank(d->dbg) : d->disasm_bank;
}

/* The listing is the whole bank (Stella's DiStella run), shown a window at a
 * time from line disasm_first. Follow PC, and every new stop, put the PC a
 * third of the way down. */
static void refresh_disasm(debugger *d)
{
    int total = 0, pc_line = -1;
    const int rows = disasm_rows(d);

    a2600debug_disassemble(d->dbg, d->disasm_bank, 0, d->lines, 1, &total, &pc_line);
    d->disasm_total = total;
    d->pc_listed = pc_line;
    if ((following(d) || d->recentre) && pc_line >= 0) d->disasm_first = pc_line - rows / 3;
    if (d->disasm_first > total - rows) d->disasm_first = total - rows;
    if (d->disasm_first < 0) d->disasm_first = 0;
    d->recentre = 0;
    d->line_count = a2600debug_disassemble(d->dbg, d->disasm_bank, d->disasm_first, d->lines, rows, &total, &pc_line);
    if (d->line_count < 0) d->line_count = 0;
    d->pc_line = pc_line;
    InvalidateRect(d->disasm, NULL, FALSE);
}

/* Scrolls the listing by `rows` lines (turning Follow PC off). */
static void disasm_scroll(debugger *d, int rows)
{
    SendMessageA(d->follow_pc, BM_SETCHECK, BST_UNCHECKED, 0);
    InvalidateRect(d->follow_pc, NULL, FALSE);
    d->disasm_first += rows;
    refresh_disasm(d);
}

static void lv_set(HWND lv, int row, int col, const char *text)
{
    wchar_t w[256];
    LVITEMW it;
    wide(text, w, 256);
    memset(&it, 0, sizeof it);
    it.iSubItem = col;
    it.pszText = w;
    SendMessageW(lv, LVM_SETITEMTEXTW, (WPARAM)row, (LPARAM)&it);
}

static int page_visible(debugger *d, int page)
{
    return d->popped[page] != NULL || d->page == page;
}

/* The list, or (empty) the hint in its place. */
static void show_bp_list(debugger *d)
{
    const int on = page_visible(d, TAB_BREAKS);
    ShowWindow(d->bp_list, on && d->nbps > 0 ? SW_SHOW : SW_HIDE);
    ShowWindow(d->bp_empty, on && d->nbps == 0 ? SW_SHOW : SW_HIDE);
}

/* "Stopped: <why>" / "Running." over the list. */
static void refresh_bp_stop(debugger *d)
{
    char reason[160], text[200];
    int addr;
    if (a2600debug_is_stopped(d->dbg)) {
        reason[0] = '\0';
        a2600debug_stop_reason(d->dbg, reason, sizeof reason, &addr);
        snprintf(text, sizeof text, reason[0] ? "Stopped: %s" : "Stopped.", reason);
    } else {
        snprintf(text, sizeof text, "Running.");
    }
    set_label(d->bp_stop, text);
}

/* Rebuilt only when something in it changed (each step refreshes), keeping
 * the selected breakpoint selected. */
static void refresh_bps(debugger *d)
{
    static char sig[sizeof d->bp_sig];
    size_t len = 0;
    int i, sel_id = -1;
    const int sel = (int)SendMessageA(d->bp_list, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);

    refresh_bp_stop(d);
    if (sel >= 0 && sel < d->nbps) sel_id = d->bps[sel].id;
    d->nbps = a2600debug_bp_list(d->dbg, d->bps, MAX_BPS);
    if (d->nbps < 0) d->nbps = 0;
    sig[0] = '\0';
    for (i = 0; i < d->nbps; i++) {
        char desc[256];
        a2600bp_describe(d->dbg, &d->bps[i], desc, sizeof desc);
        len = addf(sig, len, sizeof sig, "%d %d %u %s\n", d->bps[i].id, d->bps[i].enabled, d->bps[i].hits, desc);
    }
    if (d->bp_sig_ok && strcmp(sig, d->bp_sig) == 0) { show_bp_list(d); return; }
    memcpy(d->bp_sig, sig, sizeof sig);
    d->bp_sig_ok = 1;

    d->updating = 1;
    SendMessageA(d->bp_list, LVM_DELETEALLITEMS, 0, 0);
    for (i = 0; i < d->nbps; i++) {
        const a2600debug_breakpoint *b = &d->bps[i];
        LVITEMA it;
        char id[16], desc[256], hits[24];
        snprintf(id, sizeof id, "#%d", b->id);
        a2600bp_describe(d->dbg, b, desc, sizeof desc);
        snprintf(hits, sizeof hits, "%u", b->hits);
        memset(&it, 0, sizeof it);
        it.mask = LVIF_TEXT;
        it.iItem = i;
        it.pszText = id;
        SendMessageA(d->bp_list, LVM_INSERTITEMA, 0, (LPARAM)&it);
        lv_set(d->bp_list, i, 1, desc);
        lv_set(d->bp_list, i, 2, hits);
        memset(&it, 0, sizeof it);
        it.stateMask = LVIS_STATEIMAGEMASK | LVIS_SELECTED | LVIS_FOCUSED;
        it.state = INDEXTOSTATEIMAGEMASK(b->enabled ? 2 : 1);
        if (b->id == sel_id) it.state |= LVIS_SELECTED | LVIS_FOCUSED;
        SendMessageA(d->bp_list, LVM_SETITEMSTATE, (WPARAM)i, (LPARAM)&it);
    }
    d->updating = 0;
    show_bp_list(d);
}

/* ---- watches ---------------------------------------------------------------------- */

/* The rows are rebuilt when a value or the list changed, keeping the
 * selection. */
static void refresh_watches(debugger *d)
{
    static char sig[sizeof d->wt_sig];
    const int n = a2600watch_count();
    const int keep = (int)SendMessageA(d->wt_list, LB_GETCURSEL, 0, 0);
    char lines[A2600WATCH_MAX][120];
    size_t len = 0;
    int i;

    sig[0] = '\0';
    for (i = 0; i < n; i++) {
        a2600watch_format(d->dbg, i, lines[i], sizeof lines[i]);
        len = addf(sig, len, sizeof sig, "%s\n", lines[i]);
    }
    if (d->wt_sig_ok && strcmp(sig, d->wt_sig) == 0) return;
    memcpy(d->wt_sig, sig, sizeof sig);
    d->wt_sig_ok = 1;
    SendMessageA(d->wt_list, WM_SETREDRAW, FALSE, 0);
    SendMessageA(d->wt_list, LB_RESETCONTENT, 0, 0);
    for (i = 0; i < n; i++) SendMessageA(d->wt_list, LB_ADDSTRING, 0, (LPARAM)lines[i]);
    if (keep >= 0 && keep < n) SendMessageA(d->wt_list, LB_SETCURSEL, (WPARAM)keep, 0);
    SendMessageA(d->wt_list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(d->wt_list, NULL, TRUE);
}

/* ---- cart & I/O ------------------------------------------------------------------- */

/* The cartridge, the RIOT and the memory map as mapped now, in one view. */
static void refresh_cart(debugger *d)
{
    static char text[8192], current[8192];
    a2600debug_riot riot;
    size_t len = 0;
    int index;
    char info[256];

    a2600debug_cart_info(d->dbg, info, sizeof info);
    a2600debug_riot_get(d->dbg, &riot);
    d->mem_map_count = a2600debug_memory_map(d->dbg, d->mem_map, MEM_REGION_MAX);
    if (d->mem_map_count < 0) d->mem_map_count = 0;
    len = addf(text, len, sizeof text, "Cartridge\n  %s\n", info);
    len = addf(text, len, sizeof text, "\nRIOT\n  SWCHA  $%02X   SWACNT $%02X   left: %s   right: %s\n  SWCHB  $%02X   SWBCNT $%02X\n  INPT0-5 ",
               riot.swcha, riot.swacnt, riot.dir_left, riot.dir_right, riot.swchb, riot.swbcnt);
    for (index = 0; index < 6; index++) len = addf(text, len, sizeof text, " %02X", riot.inpt[index]);
    len = addf(text, len, sizeof text, "\n  Timer  INTIM $%02X   TIMINT $%02X   clocks %d   divider %d\n",
               riot.intim, riot.timint, riot.tim_clocks, riot.tim_divider);
    len = addf(text, len, sizeof text, "  Switches  Select %s   Reset %s   %s   left difficulty %s   right difficulty %s\n",
               riot.select ? "pressed" : "up", riot.reset ? "pressed" : "up", riot.color ? "Color" : "B&W",
               riot.diff_left_a ? "A" : "B", riot.diff_right_a ? "A" : "B");
    len = addf(text, len, sizeof text, "\nMemory map (as mapped now)\n");
    for (index = 0; index < d->mem_map_count; index++)
        len = addf(text, len, sizeof text, "  %04X-%04X  %c%c  %s\n", d->mem_map[index].lo, d->mem_map[index].hi,
                   d->mem_map[index].readable ? 'r' : '-', d->mem_map[index].writable ? 'w' : '-',
                   d->mem_map[index].name);
    if (strcmp(text, current) == 0) return;       /* unchanged keeps a selection */
    memcpy(current, text, sizeof current);
    set_text_at(d->cart_text, text, 0);
}

/* The TIA's two channels. */
static void refresh_sound(debugger *d)
{
    static char text[2048], current[2048];
    a2600debug_sound snd;
    size_t len = 0;
    int index;

    a2600debug_sound_get(d->dbg, &snd);
    len = addf(text, len, sizeof text, "TIA\n");
    for (index = 0; index < 2; index++)
        len = addf(text, len, sizeof text, "  Channel %d   AUDC%d $%02X   AUDF%d $%02X   AUDV%d $%02X   %s%s\n", index,
                   index, snd.audc[index], index, snd.audf[index], index, snd.audv[index], snd.freq[index],
                   (snd.audv[index] & 15) == 0 ? "  (silent)" : "");
    if (strcmp(text, current) == 0) return;
    memcpy(current, text, sizeof current);
    set_text_at(d->sound_text, text, 0);
}

/* The symbol table, filtered by the box: address and name, in the
 * engine's order (address, then name). The indices are fetched afresh every
 * time: the engine reorders them when symbols are loaded or cleared. */
static void refresh_symbols(debugger *d)
{
    char filter[96], signature[128], count_text[96];
    int total, shown, index;

    edit_text(d->sym_filter, filter, sizeof filter);
    total = a2600debug_symbols_count(d->dbg);
    shown = a2600debug_symbols_filter(d->dbg, strip(filter), d->sym_indices, SYMBOL_MAX);
    if (shown < 0) shown = 0;
    snprintf(signature, sizeof signature, "%d %d %s", total, shown, filter);
    if (d->sym_shown >= 0 && strcmp(signature, d->sym_sig) == 0 && !d->sym_force) return;
    d->sym_force = 0;
    snprintf(d->sym_sig, sizeof d->sym_sig, "%s", signature);
    d->sym_shown = shown;
    SendMessageA(d->sym_list, WM_SETREDRAW, FALSE, 0);
    SendMessageA(d->sym_list, LVM_DELETEALLITEMS, 0, 0);
    for (index = 0; index < shown; index++) {
        uint16_t address = 0;
        char name[96], address_text[16];
        LVITEMA item;
        if (!a2600debug_symbols_get(d->dbg, d->sym_indices[index], &address, name, sizeof name)) continue;
        snprintf(address_text, sizeof address_text, "%04X", address);
        memset(&item, 0, sizeof item);
        item.mask = LVIF_TEXT;
        item.iItem = index;
        item.pszText = address_text;
        SendMessageA(d->sym_list, LVM_INSERTITEMA, 0, (LPARAM)&item);
        lv_set(d->sym_list, index, 1, name);
    }
    SendMessageA(d->sym_list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(d->sym_list, NULL, TRUE);
    snprintf(count_text, sizeof count_text, "%d of %d symbol%s", shown, total, total == 1 ? "" : "s");
    set_label(d->sym_count, count_text);
}

/* ---- trace and history ------------------------------------------------------------- */

/* Both tabs read the engine's one ring (newest first) and show the newest
 * HIST_SHOWN matching entries, oldest first. Record on either toggles the
 * same recorder. */
static void hist_refresh(debugger *d, int idx, int force)
{
    static a2600debug_trace_entry ring[HIST_RING];
    static int hit[HIST_SHOWN];
    static char text[HIST_SHOWN * 120];
    const int on = a2600debug_trace_enabled(d->dbg);
    const int total = a2600debug_trace_count(d->dbg);
    uint16_t lo = 0, hi = 0xFFFF;
    int filter = A2600DEBUG_TRACE_ALL, n, i, shown = 0, k;
    uint64_t sig;
    size_t len = 0;
    char note[120], range[96], err[96];

    for (k = 0; k < 2; k++) {                /* both Record buttons follow the engine */
        const int was = SendMessageA(d->h_rec[k], BM_GETCHECK, 0, 0) == BST_CHECKED;
        if (was != (on != 0)) {
            SendMessageA(d->h_rec[k], BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
            InvalidateRect(d->h_rec[k], NULL, FALSE);
        }
    }
    switch ((int)SendMessageA(d->h_filter[idx], CB_GETCURSEL, 0, 0)) {
    case 1: filter = A2600DEBUG_TRACE_BRANCHES; break;
    case 2:
        edit_text(d->h_range[idx], range, sizeof range);
        if (parse_range_ui(d, range, &lo, &hi, err, sizeof err)) filter = A2600DEBUG_TRACE_RANGE;
        else {
            set_label(d->h_note[idx], err);
            return;
        }
        break;
    default: break;
    }
    n = total > 0 ? a2600debug_trace_read(d->dbg, ring, HIST_RING) : 0;
    /* what the view shows depends on the newest entry, the count and the filter */
    sig = (uint64_t)n * 1000003u + (n ? ring[0].cycles * 31u + ring[0].pc : 0) + (uint64_t)filter * 7919u +
          lo * 131u + hi + (uint64_t)on * 17u;
    if (!force && d->h_ok[idx] && sig == d->h_sig[idx]) return;
    d->h_sig[idx] = sig;
    d->h_ok[idx] = 1;
    for (i = 0; i < n && shown < HIST_SHOWN; i++)       /* newest matches first */
        if (a2600debug_trace_match(&ring[i], filter, lo, hi)) hit[shown++] = i;
    text[0] = '\0';
    for (i = shown - 1; i >= 0; i--) {                   /* oldest first */
        char line[160];
        a2600debug_trace_format(&ring[hit[i]], line, sizeof line);
        len = addf(text, len, sizeof text, "%s\n", line);
    }
    if (!shown) len = addf(text, len, sizeof text, on ? "(nothing recorded yet)" : "(recording is off: press Record)");
    set_text_at(d->h_text[idx], text, 1);
    if (!on && n == 0) snprintf(note, sizeof note, "recording off");
    else snprintf(note, sizeof note, "%s%d instruction%s recorded, %d shown", on ? "Recording. " : "", n, n == 1 ? "" : "s",
                  shown);
    set_label(d->h_note[idx], note);
}

/* The packet log is rebuilt only when it changed: a new packet, or a
 * repeat (which moves the newest serial on). */
static void refresh_fujinet(debugger *d, int force)
{
    static a2600_fuji_packet p[TRACE_MAX];
    static char t[TRACE_MAX * 320 + 128];   /* an entry is at most ~300 */
    static char state[4096];
    char note[200];
    size_t len = 0;
    int n = a2600debug_fujinet_trace(d->dbg, p, TRACE_MAX), i, follow;

    if (a2600session_fujinet_external(d->session)) {
        const int link = a2600session_cart_link_up(d->session);
        snprintf(note, sizeof note, "External FujiNet at %s%s", a2600session_boip_endpoint(d->session),
                 link > 0 ? ", connected." : link == 0 ? ", not connected." : "; a local cartridge has no link.");
    } else if (!a2600session_fujinet_running(d->session)) {
        snprintf(note, sizeof note, "FujiNet is off (Preferences \xe2\x96\xb8 Host \xe2\x96\xb8 Enable FujiNet).");
    } else {
        const int link = a2600session_cart_link_up(d->session);
        snprintf(note, sizeof note, "FujiNet is on BoIP %d%s", A2600SESSION_BOIP_PORT,
                 link > 0 ? ", connected." : link == 0 ? ", not connected." : "; a local cartridge has no link.");
    }
    set_label(d->fuji_note, note);
    state[0] = '\0';
    a2600debug_fujinet_state(d->dbg, state, (int)sizeof state);
    set_text_at(d->fuji_state, state, 0);
    if (n < 0) n = 0;

    if (!force && n == d->fuji_count && (n == 0 || p[n - 1].serial == d->fuji_last)) return;
    follow = force || d->fuji_count < 0 || edit_at_end(d, d->fuji_text);
    d->fuji_count = n;
    d->fuji_last = n > 0 ? p[n - 1].serial : 0;
    t[0] = '\0';
    for (i = 0; i < n; i++) {
        int k;
        len = addf(t, len, sizeof t, "%s%s", i ? "\n" : "", p[i].text);
        if (p[i].repeat > 1) len = addf(t, len, sizeof t, " \xc3\x97%u", (unsigned)p[i].repeat);
        len = addf(t, len, sizeof t, "\n     ");
        for (k = 0; k < p[i].nbytes && k < (int)sizeof p[i].data; k++)
            len = addf(t, len, sizeof t, "%02X ", p[i].data[k]);
        if (p[i].nbytes > (int)sizeof p[i].data) len = addf(t, len, sizeof t, "\xe2\x80\xa6");
    }
    if (n == 0) len = addf(t, len, sizeof t, "(no FujiBus packets since the trace was cleared)");
    set_text_at(d->fuji_text, t, follow);
}

/* One tab's contents. */
static void refresh_tab(debugger *d, int page)
{
    switch (page) {
    case TAB_DISASM: refresh_disasm(d); break;
    case TAB_MEMORY: refresh_mem(d); break;
    case TAB_BREAKS: refresh_bps(d); break;
    case TAB_WATCHES: refresh_watches(d); break;
    case TAB_CART: refresh_cart(d); break;
    case TAB_TRACE: hist_refresh(d, 0, 0); break;
    case TAB_HISTORY: hist_refresh(d, 1, 0); break;
    case TAB_VIDEO: if (d->video) dbg_video_refresh(d->video, 0); break;
    case TAB_SOUND: refresh_sound(d); break;
    case TAB_SYMBOLS: refresh_symbols(d); break;
    case TAB_FUJINET: refresh_fujinet(d, 0); break;
    default: break;
    }
}

/* The registers strip, and every tab on screen (the one the tab control
 * shows and the popped-out ones); the others catch up when they are shown. */
static void refresh_visible(debugger *d)
{
    int p;
    refresh_cpu(d);
    for (p = 0; p < TAB_COUNT; p++)
        if (page_visible(d, p)) refresh_tab(d, p);
}

/* At a stop the console says why, as the other frontends' do. */
static void console_stop(debugger *d)
{
    char reason[160], line[200];
    int addr;
    reason[0] = '\0';
    a2600debug_stop_reason(d->dbg, reason, sizeof reason, &addr);
    if (!reason[0]) return;
    snprintf(line, sizeof line, "\xe2\x80\x94 %s", reason);
    console_output(d, line);
}

/* After anything that may have moved the engine: the status and the tab
 * showing follow it. */
static void resync(debugger *d)
{
    const int stopped = a2600debug_is_stopped(d->dbg) != 0;
    if (stopped && !d->was_stopped) console_stop(d);
    d->was_stopped = stopped;
    d->seen_gen = a2600debug_generation(d->dbg);
    d->running_ticks = 0;
    d->h_ticks = 0;
    {
        /* a new stop brings every view to the spot: the Disassembly back to
         * the PC, wherever it was scrolled, the Memory dump to the PC's row,
         * Trace and History to their newest line (they follow the end of
         * their text when rebuilt) */
        const uint64_t serial = a2600debug_stop_serial(d->dbg);
        if (serial != d->stop_serial) {
            d->stop_serial = serial;
            d->recentre = 1;
            d->mem_recentre = 1;
            d->h_ok[0] = d->h_ok[1] = 0;
        }
    }
    refresh_status(d);
    refresh_visible(d);
}

/* A step takes microseconds: a moment's wait for the stop keeps the window
 * from flashing "Running" in between. */
static void settle_and_sync(debugger *d, int expect_stop)
{
    int i;
    d->notice[0] = '\0';        /* the machine moved: its new state replaces a notice */
    for (i = 0; expect_stop && i < 40 && !a2600debug_is_stopped(d->dbg); i++)
        Sleep(1);
    resync(d);
}

static void tick(debugger *d)
{
    unsigned gen;
    int stopped;
    if (!IsWindowVisible(d->hwnd)) return;
    if (d->notice[0] && !notice_showing(d)) {
        d->notice[0] = '\0';
        refresh_status(d);
    }
    gen = a2600debug_generation(d->dbg);
    stopped = a2600debug_is_stopped(d->dbg) != 0;
    if (gen != d->seen_gen || stopped != d->was_stopped) {
        if (stopped != d->was_stopped) d->notice[0] = '\0';   /* a stop (or a go) says so at once */
        resync(d);
    } else if (!stopped) {
        int p;
        if (++d->running_ticks >= 5) {
            /* running: the live values of the tabs showing, twice a second
             * (History waits for a stop; Trace has its own, faster, beat) */
            d->running_ticks = 0;
            refresh_status(d);
            refresh_cpu(d);
            for (p = 0; p < TAB_COUNT; p++)
                if (page_visible(d, p) && p != TAB_HISTORY && p != TAB_TRACE) refresh_tab(d, p);
        }
        if (++d->h_ticks >= HIST_TICKS) {
            d->h_ticks = 0;
            if (page_visible(d, TAB_TRACE)) hist_refresh(d, 0, 0);
        }
    }
}

/* ---- actions ------------------------------------------------------------------ */

static void on_run(debugger *d)
{
    const int stopped = a2600debug_is_stopped(d->dbg);
    if (stopped) a2600debug_resume(d->dbg);
    else a2600debug_stop(d->dbg);
    settle_and_sync(d, !stopped);
}

static void on_run_to(debugger *d)
{
    if (d->selected < 0) {
        toast(d, "Select a line in the Disassembly to run to");
        return;
    }
    a2600debug_run_to(d->dbg, (uint16_t)d->selected);
    settle_and_sync(d, 1);
}

static void toggle_breakpoint(debugger *d, int addr)
{
    a2600debug_breakpoint_toggle(d->dbg, (uint16_t)addr, listed_bank(d));
    resync(d);
}

/* F9: the breakpoint on the selected disassembly line. */
static void on_toggle_bp(debugger *d)
{
    if (d->selected < 0) {
        toast(d, "Select a line in the Disassembly first");
        return;
    }
    toggle_breakpoint(d, d->selected);
}

static void on_console(debugger *d)
{
    static char out[65536];
    char buf[256], line[300], *cmd;
    const int was = a2600debug_is_stopped(d->dbg);

    edit_text(d->con_in, buf, sizeof buf);
    cmd = strip(buf);
    if (!*cmd) return;
    /* history: the newest last, no repeats of the one before */
    if (d->nhistory == 0 || strcmp(d->history[d->nhistory - 1], cmd) != 0) {
        if (d->nhistory == HISTORY_MAX) {
            memmove(d->history[0], d->history[1], sizeof d->history[0] * (HISTORY_MAX - 1));
            d->nhistory--;
        }
        snprintf(d->history[d->nhistory++], sizeof d->history[0], "%s", cmd);
    }
    d->history_pos = d->nhistory;
    snprintf(line, sizeof line, "> %s\n", cmd);
    edit_to_end(d->con_out);     /* a command brings the reader back down */
    console_append(d, line);

    out[0] = '\0';
    a2600debug_command(d->dbg, cmd, out, sizeof out);
    SetWindowTextA(d->con_in, "");
    console_output(d, out);
    settle_and_sync(d, was);
}

/* Up and Down walk the commands already run; past the newest is an empty
 * line again. */
static void history_step(debugger *d, int newer)
{
    int len;
    if (d->nhistory == 0) return;
    d->history_pos += newer ? 1 : -1;
    if (d->history_pos < 0) d->history_pos = 0;
    if (d->history_pos >= d->nhistory) {
        d->history_pos = d->nhistory;
        SetWindowTextA(d->con_in, "");
        return;
    }
    SetWindowTextA(d->con_in, d->history[d->history_pos]);
    len = GetWindowTextLengthA(d->con_in);
    SendMessageA(d->con_in, EM_SETSEL, (WPARAM)len, (LPARAM)len);
}

/* Tab: the engine completes the end of the line; one answer takes its
 * place, several are listed (and the part they share is typed). */
static void complete_console(debugger *d)
{
    char text[256], comps[4096], common[256] = "", line[256];
    char *sp, *cur, *nl;
    int n, first = 1;
    edit_text(d->con_in, text, sizeof text);
    n = a2600debug_completions(d->dbg, text, comps, sizeof comps);
    if (n <= 0) return;
    for (cur = comps; *cur; cur = nl ? nl + 1 : cur + strlen(cur)) {
        int k = 0;
        nl = strchr(cur, '\n');
        snprintf(line, sizeof line, "%.*s", nl ? (int)(nl - cur) : (int)strlen(cur), cur);
        if (!line[0]) continue;
        if (first) {
            snprintf(common, sizeof common, "%s", line);
            first = 0;
        } else {
            while (common[k] && line[k] && common[k] == line[k]) k++;
            common[k] = '\0';
        }
    }
    if (n > 1) console_append(d, comps);
    sp = strrchr(text, ' ');
    if ((int)strlen(common) < (int)strlen(sp ? sp + 1 : text))
        snprintf(common, sizeof common, "%s", sp ? sp + 1 : text);   /* never shorter than typed */
    if (sp) sp[1] = '\0'; else text[0] = '\0';
    {
        char joined[600];
        snprintf(joined, sizeof joined, "%s%s%s", text, common, n == 1 ? " " : "");
        SetWindowTextA(d->con_in, joined);
        SendMessageA(d->con_in, EM_SETSEL, (WPARAM)strlen(joined), (LPARAM)strlen(joined));
    }
}

/* A register or flip-flop box. Refused, with a notice, unless stopped. */
static void on_reg(debugger *d, int i)
{
    char buf[32];
    long v;
    edit_text(d->reg[i], buf, sizeof buf);
    if (!parse_hexish(d, buf, &v))
        toast(d, "Not a number (hex, or an expression)");
    else if (!a2600debug_is_stopped(d->dbg))
        toast(d, "Stop the machine first to change a register");
    else
        a2600debug_cpu_set(d->dbg, k_reg_ids[i], (int)v);
    SetFocus(d->hwnd);
    resync(d);
}

static void on_check(debugger *d, int i)
{
    if (d->updating) return;
    if (!a2600debug_is_stopped(d->dbg)) {
        toast(d, "Stop the machine first to change a flag");
        refresh_cpu(d);     /* the box goes back to the CPU's */
        return;
    }
    a2600debug_cpu_set(d->dbg, k_chk_ids[i], SendMessageA(d->chk[i], BM_GETCHECK, 0, 0) == BST_CHECKED);
    resync(d);
}

/* Go to: an address, label or range; the dump from just above it, the bytes
 * found highlighted and scrolled to. */
static void on_mem_goto(debugger *d)
{
    char buf[64], err[96];
    uint16_t lo, hi;
    edit_text(d->mem_goto, buf, sizeof buf);
    if (!parse_range_ui(d, buf, &lo, &hi, err, sizeof err)) {
        d->mem_hl = 0;
        refresh_mem(d);
        toast(d, err);
        return;
    }
    show_in_memory(d, lo, hi);
}

/* Find Next / Find Previous: from the highlighted bytes on (or back), else
 * from the top of the dump; the machine must be stopped. */
static void on_search(debugger *d, int backward)
{
    char text[200];
    uint8_t pat[A2600SEARCH_MAX], mask;
    uint16_t found;
    int n = 0, flags = 0, from, r;
    edit_text(d->srch_entry, text, sizeof text);
    if (!a2600search_parse((int)SendMessageA(d->srch_kind, CB_GETCURSEL, 0, 0), text, pat, &n, &mask, &flags) ||
        n <= 0) {
        set_label(d->srch_note, "Type hex bytes (3F 00) or some text.");
        return;
    }
    if (!a2600debug_is_stopped(d->dbg)) {
        set_label(d->srch_note, "Stop the machine first");
        return;
    }
    from = d->mem_hl ? (backward ? d->mem_hl_lo : (d->mem_hl_lo + 1) & MEM_TOP) : 0;
    r = a2600debug_search_next(d->dbg, pat, n, mask, flags, (uint16_t)from, backward, &found);
    if (r > 0) {
        char note[48];
        snprintf(note, sizeof note, "Found at $%04X", found);
        set_label(d->srch_note, note);
        show_in_memory(d, found, (uint16_t)(found + n - 1));
    } else {
        set_label(d->srch_note, r < 0 ? "Stop the machine first" : "Not found");
    }
}

/* "address value [value...]": bytes into memory from the address on. */
static void on_mem_write(debugger *d)
{
    char buf[256], err[96], *tok, *save = NULL, msg[96];
    int addr, n = 0;
    edit_text(d->mem_write, buf, sizeof buf);
    tok = strtok_s(buf, " \t,", &save);
    if (!tok) return;
    addr = resolve_addr(d, tok, err, sizeof err);
    if (addr < 0) {
        toast(d, err);
        return;
    }
    while ((tok = strtok_s(NULL, " \t,", &save)) != NULL) {
        long v;
        if (!parse_hexish(d, tok, &v) || v < 0 || v > 0xFF) {
            toast(d, "A value is a byte (00-FF)");
            return;
        }
        a2600debug_write(d->dbg, (uint16_t)(addr + n), (uint8_t)v);
        n++;
    }
    if (!n) {
        toast(d, "Type: address value [value...]");
        return;
    }
    snprintf(msg, sizeof msg, "Wrote %d byte%s at $%04X", n, n == 1 ? "" : "s", addr);
    toast(d, msg);
    resync(d);
}

static void on_jump(debugger *d)
{
    char buf[64], err[96];
    int addr;
    edit_text(d->jump, buf, sizeof buf);
    addr = resolve_addr(d, buf, err, sizeof err);
    if (addr < 0) {
        toast(d, err);
        return;
    }
    SendMessageA(d->follow_pc, BM_SETCHECK, BST_UNCHECKED, 0);
    d->selected = addr;
    /* the first line of the listing at or past the address */
    {
        int first = 0, total = 0, pc_line = -1;
        for (;;) {
            static a2600debug_line page[256];
            int n = a2600debug_disassemble(d->dbg, d->disasm_bank, first, page, 256, &total, &pc_line), i;
            if (n <= 0) break;
            for (i = 0; i < n; i++)
                if (page[i].address >= addr) {
                    d->disasm_first = first + i - disasm_rows(d) / 3;
                    refresh_disasm(d);
                    return;
                }
            first += n;
            if (first >= total) break;
        }
        d->disasm_first = total;
        refresh_disasm(d);
    }
}

/* The selected breakpoint row's index, or -1. */
static int bp_selected(debugger *d)
{
    const int i = (int)SendMessageA(d->bp_list, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
    return i >= 0 && i < d->nbps ? i : -1;
}

static void on_bp_remove(debugger *d)
{
    const int i = bp_selected(d);
    if (i < 0) {
        set_label(d->bp_msg, "Select a breakpoint to remove");
        return;
    }
    set_label(d->bp_msg, "");
    a2600debug_bp_remove(d->dbg, d->bps[i].id);
    resync(d);
}

static void on_bp_add(debugger *d)
{
    char a[64], b[64], cond[256], err[160];
    int start, end, sel, id;
    edit_text(d->bp_start, a, sizeof a);
    edit_text(d->bp_end, b, sizeof b);
    edit_text(d->bp_cond, cond, sizeof cond);
    sel = (int)SendMessageA(d->bp_type, CB_GETCURSEL, 0, 0);
    if (sel < 0 || sel >= NBPTYPES) sel = 0;
    start = resolve_addr(d, a, err, sizeof err);
    if (start < 0) {
        set_label(d->bp_msg, err);
        return;
    }
    end = strip(b)[0] ? resolve_addr(d, b, err, sizeof err) : start;
    if (end < 0) {
        set_label(d->bp_msg, err);
        return;
    }
    if (end < start) end = start;
    {
        const int bank_sel = (int)SendMessageA(d->bp_bank, CB_GETCURSEL, 0, 0);
        const int bank = sel == 0 && bank_sel > 0 ? bank_sel - 1 : A2600DEBUG_ANY_BANK;
        id = a2600debug_bp_add(d->dbg, k_bp_type_ids[sel], (uint16_t)start, (uint16_t)end, bank, strip(cond));
    }
    if (id < 0) {
        set_label(d->bp_msg, "The condition does not parse");
        return;
    }
    set_label(d->bp_msg, "");
    SetWindowTextA(d->bp_start, "");
    SetWindowTextA(d->bp_end, "");
    SetWindowTextA(d->bp_cond, "");
    resync(d);
}

/* The type decides what the boxes take. */
static void on_bp_type(debugger *d)
{
    const int sel = (int)SendMessageA(d->bp_type, CB_GETCURSEL, 0, 0);
    SendMessageW(d->bp_start, EM_SETCUEBANNER, TRUE, (LPARAM)(sel == 0 ? L"F000 / label" : L"0080 / label"));
    SendMessageW(d->bp_end, EM_SETCUEBANNER, TRUE, (LPARAM)L"(end)");
    EnableWindow(d->bp_bank, sel == 0);        /* a bank is for Execute only */
}

static int pick_file(debugger *d, int save, const char *title, const char *filter, char *path, DWORD pathsz)
{
    OPENFILENAMEA ofn;
    memset(&ofn, 0, sizeof ofn);
    path[0] = '\0';
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = d->hwnd;
    ofn.lpstrFile = path;
    ofn.nMaxFile = pathsz;
    ofn.lpstrTitle = title;
    ofn.lpstrFilter = filter;
    if (save) {
        ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        return GetSaveFileNameA(&ofn) ? 1 : 0;
    }
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameA(&ofn) ? 1 : 0;
}

static void goto_tab(debugger *d, int page);

static void on_symbols(debugger *d)
{
    char msg[512];
    msg[0] = '\0';
    a2600debug_load_symbols(d->dbg, msg, sizeof msg);
    if (!msg[0]) snprintf(msg, sizeof msg, "No <rom>.sym or <rom>.lst found next to the cartridge");
    d->sym_force = 1;
    say(d, msg);
    resync(d);
}

static void on_clear_symbols(debugger *d)
{
    a2600debug_clear_symbols(d->dbg);
    d->sym_force = 1;
    d->bp_sig_ok = 0;
    say(d, "Symbols cleared (Stella's own equates stay)");
    resync(d);
}

static void on_clear_breakpoints(debugger *d)
{
    a2600debug_breakpoint_clear(d->dbg);
    set_label(d->bp_msg, "");
    resync(d);
}

/* "Break at:" -- a symbol or an address, Enter: an execute breakpoint. */
static void on_break_at(debugger *d)
{
    char text[96], err[96], msg[160];
    int address;
    edit_text(d->break_at, text, sizeof text);
    address = resolve_addr(d, text, err, sizeof err);
    if (address < 0) {
        toast(d, err);
        return;
    }
    a2600debug_bp_add(d->dbg, A2600DEBUG_BP_EXEC, (uint16_t)address, (uint16_t)address, A2600DEBUG_ANY_BANK, "");
    snprintf(msg, sizeof msg, "Breakpoint at $%04X", address);
    SetWindowTextA(d->break_at, "");
    toast(d, msg);
    resync(d);
}

/* A symbol row (double-click, Enter) goes to its address in the Disassembly. */
static void on_symbol_activate(debugger *d)
{
    const int row = (int)SendMessageA(d->sym_list, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
    uint16_t address = 0;
    char name[96], text[16];
    if (row < 0 || row >= d->sym_shown) return;
    if (!a2600debug_symbols_get(d->dbg, d->sym_indices[row], &address, name, sizeof name)) return;
    snprintf(text, sizeof text, "$%04X", address);
    goto_tab(d, TAB_DISASM);
    SetWindowTextA(d->jump, text);
    on_jump(d);
}

static void on_save(debugger *d, int which)
{
    char path[MAX_PATH], msg[512], title[64];
    size_t n;
    snprintf(title, sizeof title, "Save %s", k_save_titles[which]);
    n = strlen(title);
    while (n > 0 && title[n - 1] == '.') title[--n] = '\0';
    if (!pick_file(d, 1, title, "All files\0*.*\0\0", path, sizeof path)) return;
    msg[0] = '\0';
    a2600debug_save(d->dbg, k_save_kinds[which], path, msg, sizeof msg);
    if (!msg[0]) snprintf(msg, sizeof msg, "Saved %s", base_name(path));
    say(d, msg);
}

/* TIA register or strobe: a strobe by its name, else a register and a value. */
static void on_tia_set(debugger *d)
{
    char name[64], text[64], *reg, *end;
    long v;
    edit_text(d->tia_reg, name, sizeof name);
    for (reg = name; *reg; reg++)
        if (*reg >= 'A' && *reg <= 'Z') *reg = (char)(*reg + 32);
    reg = strip(name);
    if (!*reg) {
        toast(d, "Type a TIA register (colup0, pf1 ...) or a strobe (wsync, resp0, hmove ...)");
        return;
    }
    if (a2600debug_tia_strobe(d->dbg, reg) == 0) {
        snprintf(text, sizeof text, "Strobed %s", reg);
        toast(d, text);
        resync(d);
        return;
    }
    edit_text(d->tia_val, text, sizeof text);
    end = strip(text);
    if (!*end || !parse_hexish(d, end, &v)) {
        toast(d, "Type a value (hex)");
        return;
    }
    if (a2600debug_tia_set(d->dbg, reg, (int)v) != 0) {
        snprintf(text, sizeof text, "Not a TIA register or strobe: %s", reg);
        toast(d, text);
        return;
    }
    snprintf(text, sizeof text, "Set %s = $%02lX", reg, v & 0xFF);
    toast(d, text);
    resync(d);
}

/* ---- watches, trace and history ---------------------------------------------------- */

static void on_watch_add(debugger *d)
{
    char text[96];
    edit_text(d->wt_entry, text, sizeof text);
    if (a2600watch_add(strip(text)) < 0) {
        set_label(d->wt_msg, a2600watch_count() >= A2600WATCH_MAX ? "The watch list is full"
                                                                  : "Type an address, label or expression");
        return;
    }
    set_label(d->wt_msg, "");
    SetWindowTextA(d->wt_entry, "");
    refresh_watches(d);
}

static int watch_selected(debugger *d)
{
    const int i = (int)SendMessageA(d->wt_list, LB_GETCURSEL, 0, 0);
    return i >= 0 && i < a2600watch_count() ? i : -1;
}

static void on_watch_remove(debugger *d)
{
    const int i = watch_selected(d);
    if (i < 0) {
        set_label(d->wt_msg, "Select a watch to remove");
        return;
    }
    set_label(d->wt_msg, "");
    a2600watch_remove(i);
    refresh_watches(d);
}

static void goto_tab(debugger *d, int page);

/* The selected watch's bytes in the Memory tab. */
static void on_watch_memory(debugger *d)
{
    uint16_t lo, hi;
    const int i = watch_selected(d);
    if (i < 0) {
        set_label(d->wt_msg, "Select a watch first");
        return;
    }
    if (!a2600watch_range(d->dbg, i, &lo, &hi)) {
        set_label(d->wt_msg, "That watch does not resolve to an address");
        return;
    }
    set_label(d->wt_msg, "");
    show_in_memory(d, lo, hi);
    goto_tab(d, TAB_MEMORY);
}

/* A write breakpoint over a range that stops only when the byte changes. */
static int break_on_change(debugger *d, uint16_t lo, uint16_t hi, char *err, int errsz)
{
    const int id = a2600debug_bp_add(d->dbg, A2600DEBUG_BP_WRITE, lo, hi, A2600DEBUG_ANY_BANK, "value != old");
    if (id < 0) snprintf(err, (size_t)errsz, "That breakpoint could not be set");
    return id;
}

static void on_watch_break(debugger *d)
{
    uint16_t lo, hi;
    char err[160];
    const int i = watch_selected(d);
    if (i < 0) {
        set_label(d->wt_msg, "Select a watch first");
        return;
    }
    if (!a2600watch_range(d->dbg, i, &lo, &hi)) {
        set_label(d->wt_msg, "That watch does not resolve to an address");
        return;
    }
    err[0] = '\0';
    if (break_on_change(d, lo, hi, err, sizeof err) < 0) {
        set_label(d->wt_msg, err[0] ? err : "That breakpoint could not be set");
        return;
    }
    set_label(d->wt_msg, "Breaking when it changes (see Breakpoints)");
    resync(d);
}

/* Record on either tab toggles the one recorder. */
static void on_record(debugger *d, int idx)
{
    a2600debug_trace_enable(d->dbg, SendMessageA(d->h_rec[idx], BM_GETCHECK, 0, 0) == BST_CHECKED);
    d->h_ok[0] = d->h_ok[1] = 0;
    resync(d);
}

static void on_hist_clear(debugger *d, int idx)
{
    (void)idx;
    a2600debug_trace_clear(d->dbg);
    d->h_ok[0] = d->h_ok[1] = 0;
    if (page_visible(d, TAB_TRACE)) hist_refresh(d, 0, 1);
    if (page_visible(d, TAB_HISTORY)) hist_refresh(d, 1, 1);
}

static void on_hist_filter(debugger *d, int idx)
{
    EnableWindow(d->h_range[idx], SendMessageA(d->h_filter[idx], CB_GETCURSEL, 0, 0) == 2);
    hist_refresh(d, idx, 1);
}

static void on_accept(debugger *d, int id)
{
    if (id >= IDC_REG0 && id <= IDC_REG_LAST) {
        on_reg(d, id - IDC_REG0);
        return;
    }
    switch (id) {
    case IDC_CON_IN: on_console(d); break;
    case IDC_MEM_GOTO: on_mem_goto(d); break;
    case IDC_SRCH_ENTRY: on_search(d, 0); break;
    case IDC_WT_ENTRY: on_watch_add(d); break;
    case IDC_H_RANGE0: hist_refresh(d, 0, 1); break;
    case IDC_H_RANGE1: hist_refresh(d, 1, 1); break;
    case IDC_MEM_WRITE: on_mem_write(d); break;
    case IDC_JUMP: on_jump(d); break;
    case IDC_BREAK_AT: on_break_at(d); break;
    case IDC_BP_START:
    case IDC_BP_END:
    case IDC_BP_COND: on_bp_add(d); break;
    case IDC_TIA_REG:
    case IDC_TIA_VAL: on_tia_set(d); break;
    default: break;
    }
}

/* ---- the disassembly -------------------------------------------------------------- */

/* One run of a row's text at a column. */
static void put(debugger *d, HDC dc, int col, int ty, COLORREF fg, const char *text, int len)
{
    SetTextColor(dc, fg);
    TextOutA(dc, S(d, GUTTER_W) + d->mono_w * col, ty, text, len);
}

/* Drawn by hand, one row per line: the breakpoint gutter (a red dot when the
 * breakpoint is enabled, grey when disabled), address, bytes, label,
 * instruction, and the comment (a jump target's label, a port's name); the
 * label and the comment in grey. The selected line takes the system
 * selection colours, the PC's line (applied last, so it wins) is filled with
 * the accent and drawn in white. */
static void paint_disasm(debugger *d, HWND hwnd, HDC dc)
{
    RECT c;
    int i;
    GetClientRect(hwnd, &c);
    FillRect(dc, &c, GetSysColorBrush(COLOR_WINDOW));
    SelectObject(dc, d->mono);
    SetBkMode(dc, TRANSPARENT);
    if (d->line_count == 0) {
        put(d, dc, 0, 0, GetSysColor(COLOR_GRAYTEXT), "(no disassembly)", 16);
        return;
    }
    for (i = 0; i < d->line_count && i * d->line_h < c.bottom; i++) {
        const a2600debug_line *l = &d->lines[i];
        const int y = i * d->line_h, ty = y + (d->line_h - d->mono_h) / 2;
        const int is_pc = l->is_pc, sel = l->address == d->selected;
        COLORREF fg = GetSysColor(COLOR_WINDOWTEXT), dim = GetSysColor(COLOR_GRAYTEXT);
        RECT row;
        char text[240];
        int len;
        SetRect(&row, 0, y, c.right, y + d->line_h);
        if (sel) {
            FillRect(dc, &row, GetSysColorBrush(COLOR_HIGHLIGHT));
            fg = dim = GetSysColor(COLOR_HIGHLIGHTTEXT);
        }
        if (is_pc) {
            FillRect(dc, &row, a2600_accent_brush());
            fg = dim = A2600_ACCENT_TEXT;
        }
        if (l->has_breakpoint || l->bp_disabled) {
            /* a red dot for an enabled breakpoint, a grey hollow one for a disabled one */
            const int size = d->line_h - S(d, 6) > 4 ? d->line_h - S(d, 6) : 4;
            HGDIOBJ oldb = SelectObject(dc, l->has_breakpoint ? (is_pc ? GetStockObject(WHITE_BRUSH) : d->bp_on)
                                                              : GetStockObject(NULL_BRUSH));
            HPEN grey_pen = CreatePen(PS_SOLID, 2, is_pc ? RGB(255, 255, 255) : RGB(0x9a, 0x99, 0x96));
            HGDIOBJ oldp = SelectObject(dc, l->has_breakpoint ? GetStockObject(is_pc ? BLACK_PEN : NULL_PEN) : grey_pen);
            Ellipse(dc, S(d, 4), y + (d->line_h - size) / 2, S(d, 4) + size + 1, y + (d->line_h - size) / 2 + size + 1);
            SelectObject(dc, oldp);
            SelectObject(dc, oldb);
            DeleteObject(grey_pen);
        }
        if (!l->bytes[0] && !l->label[0] && !l->disasm[strspn(l->disasm, " ")]) continue;     /* a blank separator line */
        len = snprintf(text, sizeof text, "%04X", l->address);
        put(d, dc, 0, ty, fg, text, len);
        len = snprintf(text, sizeof text, "%.12s", l->bytes);
        put(d, dc, COL_BYTES, ty, fg, text, len);
        len = snprintf(text, sizeof text, "%.*s", LABEL_W, l->label);
        if (len > 0) put(d, dc, COL_LABEL, ty, dim, text, len);
        len = snprintf(text, sizeof text, "%s", l->disasm);
        if (len > (int)sizeof text - 1) len = (int)sizeof text - 1;
        if (len > 0) put(d, dc, COL_DISASM, ty, fg, text, len);
        if (l->cycles[0]) {
            len = snprintf(text, sizeof text, "%s", l->cycles);
            if (len > (int)sizeof text - 1) len = (int)sizeof text - 1;
            put(d, dc, COMMENT_COL, ty, dim, text, len);
        }
    }
}

/* A click selects a line (it works while the machine runs, too); a click on
 * the gutter, or a double-click, toggles its breakpoint. The wheel and the
 * keys browse by instructions (and turn Follow PC off). */
static LRESULT CALLBACK disasm_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    debugger *d = g_dbg;
    if (!d) return DefWindowProcA(hwnd, msg, wp, lp);
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT c;
        HDC mem;
        HBITMAP bmp;
        HGDIOBJ oldbmp, oldfont;
        GetClientRect(hwnd, &c);
        mem = CreateCompatibleDC(dc);
        bmp = CreateCompatibleBitmap(dc, c.right > 0 ? c.right : 1, c.bottom > 0 ? c.bottom : 1);
        oldbmp = SelectObject(mem, bmp);
        oldfont = SelectObject(mem, d->mono);
        paint_disasm(d, hwnd, mem);
        BitBlt(dc, 0, 0, c.right, c.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldfont);
        SelectObject(mem, oldbmp);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_PRINTCLIENT:
        paint_disasm(d, hwnd, (HDC)wp);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        /* more or fewer rows fit now */
        if (d->disasm == hwnd && page_visible(d, TAB_DISASM)) refresh_disasm(d);
        return 0;
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        const int line = d->line_h > 0 ? GET_Y_LPARAM(lp) / d->line_h : -1;
        const int gutter = GET_X_LPARAM(lp) < S(d, GUTTER_W);
        SetFocus(hwnd);
        if (line < 0 || line >= d->line_count) return 0;
        d->selected = d->lines[line].address;
        if (gutter ? msg == WM_LBUTTONDOWN : msg == WM_LBUTTONDBLCLK)
            toggle_breakpoint(d, d->lines[line].address);
        else
            InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        const int delta = GET_WHEEL_DELTA_WPARAM(wp);
        int rows = -delta * 3 / WHEEL_DELTA;
        if (!rows) rows = delta > 0 ? -1 : 1;
        disasm_scroll(d, rows);
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_PRIOR || wp == VK_NEXT || wp == VK_UP || wp == VK_DOWN) {
            const int half = disasm_rows(d) / 2 > 0 ? disasm_rows(d) / 2 : 1;
            const int rows = wp == VK_PRIOR ? -half : wp == VK_NEXT ? half : wp == VK_UP ? -1 : 1;
            disasm_scroll(d, rows);
            return 0;
        }
        break;
    default: break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* ---- edit subclassing ------------------------------------------------------------- */

static WNDPROC g_edit_proc;

/* Enter in a single-line field means "apply this value"; Tab in the console
 * completes and Up/Down recall. None may beep its way through the default
 * handler. */
static LRESULT CALLBACK edit_subclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    const int id = (int)GetWindowLongPtrW(hwnd, GWLP_ID);
    if (msg == WM_KEYDOWN && wp == VK_RETURN) {
        PostMessageA(g_dbg->hwnd, WM_DBG_ACCEPT, (WPARAM)id, 0);
        return 0;
    }
    if (msg == WM_CHAR && (wp == '\r' || wp == '\n')) return 0;
    if (id == IDC_CON_IN) {
        if (msg == WM_KEYDOWN && wp == VK_TAB) {
            PostMessageA(g_dbg->hwnd, WM_DBG_TAB, 0, 0);
            return 0;
        }
        if (msg == WM_KEYDOWN && (wp == VK_UP || wp == VK_DOWN)) {
            PostMessageA(g_dbg->hwnd, WM_DBG_HISTORY, wp == VK_DOWN, 0);
            return 0;
        }
        if (msg == WM_CHAR && wp == VK_TAB) return 0;
        if (msg == WM_GETDLGCODE) return DLGC_WANTALLKEYS;
    }
    return CallWindowProcW(g_edit_proc, hwnd, msg, wp, lp);
}

/* ---- construction ----------------------------------------------------------------- */

/* The standard controls are made as Unicode windows, so the engine's UTF-8
 * (the arrows and dashes of its text) shows as it is. */
static HWND child_ex(debugger *d, DWORD ex, const char *cls, const char *text, DWORD style, int id, HFONT font)
{
    wchar_t wcls[64], wtext[512];
    HWND h;
    wide(cls, wcls, 64);
    wide(text, wtext, 512);
    h = CreateWindowExW(ex, wcls, wtext, WS_CHILD | (d->new_parent ? WS_VISIBLE : 0) | style, 0, 0, 10, 10,
                        d->new_parent ? d->new_parent : d->hwnd,
                        (HMENU)(INT_PTR)id,
                        (HINSTANCE)GetWindowLongPtrA(d->hwnd, GWLP_HINSTANCE), NULL);
    SendMessageA(h, WM_SETFONT, (WPARAM)font, TRUE);
    return h;
}

static HWND child(debugger *d, const char *cls, const char *text, DWORD style, int id, HFONT font)
{
    return child_ex(d, 0, cls, text, style, id, font);
}

/* A read-only monospace text view. */
static HWND view(debugger *d, int id, DWORD extra)
{
    HWND h = child(d, "EDIT", "", WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | extra,
                   id, d->mono);
    SendMessageA(h, EM_SETLIMITTEXT, 0, 0);
    if (d->nviews < (int)(sizeof d->views / sizeof d->views[0])) d->views[d->nviews++] = h;
    return h;
}

static HWND field(debugger *d, int id, const wchar_t *cue)
{
    HWND h = child(d, "EDIT", "", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, id, d->mono);
    g_edit_proc = (WNDPROC)SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)edit_subclass);
    if (cue) SendMessageW(h, EM_SETCUEBANNER, TRUE, (LPARAM)cue);
    return h;
}

static HWND label(debugger *d, const char *text, HFONT font, DWORD style)
{
    return child(d, "STATIC", text, style | SS_NOPREFIX, IDC_LABEL_FIRST + d->nlabels++, font);
}

/* A caption or hint, drawn grey. */
static HWND dim_label(debugger *d, const char *text, DWORD style)
{
    HWND h = label(d, text, d->ui, style);
    if (d->ndim < (int)(sizeof d->dim / sizeof d->dim[0])) d->dim[d->ndim++] = h;
    return h;
}

static HWND button(debugger *d, const char *text, int id)
{
    return child(d, "BUTTON", text, BS_PUSHBUTTON | WS_TABSTOP, id, d->ui);
}

static HWND checkbox(debugger *d, const char *text, int id)
{
    return child(d, "BUTTON", text, BS_AUTOCHECKBOX | WS_TABSTOP, id, d->ui);
}

static void size_columns(debugger *d)
{
    static const int widths[3] = { 60, 700, 80 };
    int i;
    for (i = 0; i < 3; i++)
        SendMessageA(d->bp_list, LVM_SETCOLUMNWIDTH, (WPARAM)i, S(d, widths[i]));
}

/* Starts a tab: its container, which the controls made next are children of. */
static void begin_page(debugger *d, int page)
{
    HINSTANCE inst = (HINSTANCE)GetWindowLongPtrA(d->hwnd, GWLP_HINSTANCE);
    d->pg[page] = CreateWindowExA(0, PAGE_CLASS, "", WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0, 10, 10,
                                  d->hwnd, NULL, inst, NULL);
    SetWindowLongPtrA(d->pg[page], GWLP_USERDATA, (LONG_PTR)d->hwnd);
    d->new_parent = d->pg[page];
}

/* A tab's label for the tab control ("&" would underline the next letter). */
static void tab_text(int page, char *text, size_t n)
{
    const char *s;
    size_t o = 0;
    for (s = k_tabs[page].label; *s && o < n - 2; s++) {
        if (*s == '&') text[o++] = '&';
        text[o++] = *s;
    }
    text[o] = '\0';
}

static void build_controls(debugger *d)
{
    HINSTANCE inst = (HINSTANCE)GetWindowLongPtrA(d->hwnd, GWLP_HINSTANCE);
    TCITEMA item;
    int i;

    d->tabs = child(d, "SysTabControl32", "", WS_VISIBLE | WS_CLIPSIBLINGS | WS_TABSTOP, IDC_TABS, d->ui);
    memset(&item, 0, sizeof item);
    item.mask = TCIF_TEXT;
    for (i = 0; i < TAB_COUNT; i++) {
        char text[32];
        tab_text(i, text, sizeof text);
        item.pszText = text;
        SendMessageA(d->tabs, TCM_INSERTITEMA, (WPARAM)i, (LPARAM)&item);
        d->tab_page[i] = i;
    }
    d->ntabs = TAB_COUNT;

    /* The toolbar. Run/Stop is owner-drawn so it can wear the accent colour
     * while the machine is stopped. */
    d->run_btn = child(d, "BUTTON", "Stop (F5)", BS_OWNERDRAW | WS_TABSTOP, IDC_RUN, d->ui);
    d->step_btn = button(d, "Step (F7)", IDC_STEP);
    d->over_btn = button(d, "Step Over (F8)", IDC_OVER);
    d->out_btn = button(d, "Step Out (Shift+F8)", IDC_OUT);
    d->run_to_btn = button(d, "Run to Cursor", IDC_RUN_TO);
    d->scan_btn = button(d, "Scan+1", IDC_SCAN);
    d->frame_btn = button(d, "Frame+1", IDC_FRAME);
    d->rewind_btn = button(d, "Rewind", IDC_REWIND);
    d->unwind_btn = button(d, "Unwind", IDC_UNWIND);
    d->clear_bps_btn = button(d, "Clear Breakpoints", IDC_CLEAR_BPS);
    d->break_at_label = dim_label(d, "Break at:", SS_RIGHT);
    d->break_at = field(d, IDC_BREAK_AT, L"symbol or hex");
    d->sym_btn = button(d, "Load Symbols...", IDC_SYMBOLS);
    d->clear_sym_btn = button(d, "Clear Symbols", IDC_CLEAR_SYMBOLS);
    d->popout_btn = button(d, "Pop Out", IDC_POPOUT);
    d->help_btn = button(d, "Help", IDC_HELP_BTN);
    d->status = child(d, "STATIC", "", SS_RIGHT | SS_ENDELLIPSIS | SS_NOPREFIX, IDC_STATUS, d->ui);
    if (d->ndim < (int)(sizeof d->dim / sizeof d->dim[0])) d->dim[d->ndim++] = d->status;
    {
        HWND bar[17] = { d->run_btn, d->step_btn, d->over_btn, d->out_btn, d->run_to_btn, d->scan_btn, d->frame_btn,
                         d->rewind_btn, d->unwind_btn, d->clear_bps_btn, d->break_at_label, d->break_at, d->sym_btn,
                         d->clear_sym_btn, d->popout_btn, d->status, d->help_btn };
        for (i = 0; i < 17; i++) ShowWindow(bar[i], SW_SHOW);
    }

    /* The registers strip, under the tabs (children of the window itself) */
    for (i = 0; i < NREGS; i++) {
        d->reg_label[i] = dim_label(d, k_reg_names[i], SS_RIGHT);
        d->reg[i] = field(d, IDC_REG0 + i, NULL);
        SendMessageA(d->reg[i], EM_SETLIMITTEXT, 6, 0);
        ShowWindow(d->reg_label[i], SW_SHOW);
        ShowWindow(d->reg[i], SW_SHOW);
    }
    for (i = 0; i < NCHECKS; i++) {
        d->chk[i] = checkbox(d, k_chk_names[i], IDC_CHK0 + i);
        ShowWindow(d->chk[i], SW_SHOW);
    }
    d->beam = label(d, "", d->ui, SS_LEFT | SS_ENDELLIPSIS);
    ShowWindow(d->beam, SW_SHOW);

    /* Console */
    begin_page(d, TAB_CONSOLE);
    d->con_out = view(d, IDC_CON_OUT, 0);
    SetWindowTextA(d->con_out, "Atari 2600 debugger console (Stella's). Type 'help' for every command; Tab completes, "
                               "Up/Down recall.\r\n");
    d->con_prompt = child(d, "STATIC", ">", SS_LEFT, IDC_CON_PROMPT, d->mono);
    d->con_in = field(d, IDC_CON_IN, L"help, break, breakIf, trap, watch, frame, tia, ... (Tab completes)");

    /* Disassembly. Follow PC is a toggle button: the accent while it is on. */
    begin_page(d, TAB_DISASM);
    d->bank = child(d, "COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IDC_BANK, d->ui);
    SendMessageA(d->bank, CB_ADDSTRING, 0, (LPARAM)"PC's bank");
    {
        const int banks = a2600debug_bank_count(d->dbg);
        for (i = 0; i < banks && i < MAX_BANKS; i++) {
            char name[32];
            snprintf(name, sizeof name, "Bank %d", i);
            SendMessageA(d->bank, CB_ADDSTRING, 0, (LPARAM)name);
        }
    }
    SendMessageA(d->bank, CB_SETCURSEL, 0, 0);
    d->follow_pc = child(d, "BUTTON", "Follow PC", BS_AUTOCHECKBOX | BS_PUSHLIKE | WS_TABSTOP, IDC_FOLLOW_PC,
                         d->ui);
    SendMessageA(d->follow_pc, BM_SETCHECK, BST_CHECKED, 0);
    d->jump_label = dim_label(d, "Jump to", SS_LEFT);
    d->jump = field(d, IDC_JUMP, L"F000 or a label");
    d->disasm_hint = dim_label(d, "Click a line to select it (Run to Cursor); click the gutter, double-click or "
                                  "press F9 for a breakpoint; scroll to browse",
                               SS_LEFT | SS_ENDELLIPSIS);
    d->disasm = CreateWindowExA(WS_EX_CLIENTEDGE, DISASM_CLASS, "", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 10, 10,
                                d->pg[TAB_DISASM], (HMENU)(INT_PTR)IDC_DISASM, inst, NULL);

    /* Memory: the dump, Go to, Write, and the search bar */
    begin_page(d, TAB_MEMORY);
    d->mem_title = label(d, "Memory (the 6507's 8K bus, $0000-$1FFF, as mapped now)", d->ui_bold, SS_LEFT);
    d->mem_view = CreateWindowExA(WS_EX_CLIENTEDGE, MEMVIEW_CLASS, "", WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP, 0, 0,
                                  10, 10, d->pg[TAB_MEMORY], (HMENU)(INT_PTR)IDC_MEM_VIEW, inst, NULL);
    d->goto_label = dim_label(d, "Go to", SS_LEFT);
    d->mem_goto = field(d, IDC_MEM_GOTO, L"80 / label / lo-hi");
    d->write_label = dim_label(d, "Write", SS_LEFT);
    d->mem_write = field(d, IDC_MEM_WRITE, L"80 3F 00 (address, hex bytes)");
    d->srch_kind = child(d, "COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IDC_SRCH_KIND, d->ui);
    SendMessageA(d->srch_kind, CB_ADDSTRING, 0, (LPARAM)"Hex");
    SendMessageA(d->srch_kind, CB_ADDSTRING, 0, (LPARAM)"Text");
    SendMessageA(d->srch_kind, CB_SETCURSEL, 0, 0);
    d->srch_entry = field(d, IDC_SRCH_ENTRY, L"3F 00 (hex bytes) or text");
    d->srch_next = button(d, "Find Next", IDC_SRCH_NEXT);
    d->srch_prev = button(d, "Find Previous", IDC_SRCH_PREV);
    d->srch_note = dim_label(d, "", SS_LEFT | SS_ENDELLIPSIS);

    /* Breakpoints */
    begin_page(d, TAB_BREAKS);
    d->bp_stop = label(d, "Running.", d->ui_bold, SS_LEFT | SS_ENDELLIPSIS);
    d->bp_hint = dim_label(d, "Click a disassembly line's gutter for an execute breakpoint (in its bank); the box enables it.",
                           SS_LEFT | SS_ENDELLIPSIS);
    d->bp_remove = button(d, "Remove", IDC_BP_REMOVE);
    d->bp_clear = button(d, "Clear All", IDC_BP_CLEAR);
    d->bp_clear_hits = button(d, "Clear Hits", IDC_BP_CLEAR_HITS);
    d->bp_list = child_ex(d, WS_EX_CLIENTEDGE, "SysListView32", "",
                          WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_SINGLESEL | LVS_NOSORTHEADER,
                          IDC_BP_LIST, d->mono);
    SendMessageA(d->bp_list, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT);
    {
        static const char *const cols[3] = { "#", "Breakpoint", "Hits" };
        LVCOLUMNA col;
        memset(&col, 0, sizeof col);
        col.mask = LVCF_TEXT;
        for (i = 0; i < 3; i++) {
            col.pszText = (char *)cols[i];
            SendMessageA(d->bp_list, LVM_INSERTCOLUMNA, (WPARAM)i, (LPARAM)&col);
        }
        size_columns(d);
    }
    d->bp_empty = dim_label(d, "No breakpoints. Click the gutter of a disassembly line, or add one below.", SS_CENTER);
    d->bp_type = child(d, "COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IDC_BP_TYPE, d->ui);
    for (i = 0; i < NBPTYPES; i++) SendMessageA(d->bp_type, CB_ADDSTRING, 0, (LPARAM)k_bp_types[i]);
    SendMessageA(d->bp_type, CB_SETCURSEL, 0, 0);
    d->bp_start = field(d, IDC_BP_START, NULL);
    d->bp_end = field(d, IDC_BP_END, NULL);
    d->bp_bank = child(d, "COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IDC_BP_BANK, d->ui);
    SendMessageA(d->bp_bank, CB_ADDSTRING, 0, (LPARAM)"Any bank");
    {
        const int banks = a2600debug_bank_count(d->dbg);
        for (i = 0; i < banks && i < MAX_BANKS; i++) {
            char name[32];
            snprintf(name, sizeof name, "Bank %d", i);
            SendMessageA(d->bp_bank, CB_ADDSTRING, 0, (LPARAM)name);
        }
    }
    SendMessageA(d->bp_bank, CB_SETCURSEL, 0, 0);
    d->bp_cond = field(d, IDC_BP_COND, L"condition, e.g. a == $FF  (value, old on a write)");
    d->bp_add = button(d, "Add", IDC_BP_ADD);
    d->bp_msg = label(d, "", d->ui, SS_LEFT | SS_ENDELLIPSIS);
    on_bp_type(d);

    /* Watches */
    begin_page(d, TAB_WATCHES);
    d->wt_label = dim_label(d, "Watch", SS_LEFT);
    d->wt_entry = field(d, IDC_WT_ENTRY, L"an address, label or expression, or lo-hi for a range");
    d->wt_add = button(d, "Add", IDC_WT_ADD);
    d->wt_remove = button(d, "Remove", IDC_WT_REMOVE);
    d->wt_mem = button(d, "Memory", IDC_WT_MEM);
    d->wt_brk = button(d, "Break on change", IDC_WT_BRK);
    d->wt_list = child_ex(d, WS_EX_CLIENTEDGE, "LISTBOX", "",
                          WS_VSCROLL | WS_TABSTOP | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT, IDC_WT_LIST, d->mono);
    d->wt_msg = label(d, "", d->ui, SS_LEFT | SS_ENDELLIPSIS);

    /* Cart & I/O */
    begin_page(d, TAB_CART);
    d->cart_text = view(d, IDC_CART_TEXT, WS_HSCROLL);
    d->states_label = dim_label(d, "Emulator states (Stella's slots 0-9)", SS_LEFT);
    for (i = 0; i < NSLOTS; i++) {
        char name[16];
        snprintf(name, sizeof name, "Save %d", i);
        d->slot_save[i] = button(d, name, IDC_SLOT_SAVE0 + i);
        snprintf(name, sizeof name, "Load %d", i);
        d->slot_load[i] = button(d, name, IDC_SLOT_LOAD0 + i);
    }
    d->files_label = dim_label(d, "Save to a file", SS_LEFT);
    for (i = 0; i < NSAVE; i++) d->file_btn[i] = button(d, k_save_titles[i], IDC_SAVE_KIND0 + i);
    d->tia_label = dim_label(d, "TIA register or strobe", SS_LEFT);
    d->tia_reg = field(d, IDC_TIA_REG, L"colup0, pf1, posp0 ... or wsync, resp0, hmove ...");
    d->tia_val = field(d, IDC_TIA_VAL, L"value");
    d->tia_set = button(d, "Set", IDC_TIA_SET);

    /* Trace (0) and History (1) */
    for (i = 0; i < 2; i++) {
        begin_page(d, i ? TAB_HISTORY : TAB_TRACE);
        d->h_rec[i] = child(d, "BUTTON", "Record", BS_AUTOCHECKBOX | BS_PUSHLIKE | WS_TABSTOP, IDC_H_REC0 + i, d->ui);
        d->h_clear[i] = button(d, "Clear", IDC_H_CLEAR0 + i);
        d->h_filter[i] = child(d, "COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IDC_H_FILTER0 + i, d->ui);
        SendMessageA(d->h_filter[i], CB_ADDSTRING, 0, (LPARAM)"All");
        SendMessageA(d->h_filter[i], CB_ADDSTRING, 0, (LPARAM)"Branches");
        SendMessageA(d->h_filter[i], CB_ADDSTRING, 0, (LPARAM)"Range");
        SendMessageA(d->h_filter[i], CB_SETCURSEL, 0, 0);
        d->h_range[i] = field(d, IDC_H_RANGE0 + i, L"F000-FFFF or a label");
        EnableWindow(d->h_range[i], FALSE);
        d->h_note[i] = dim_label(d, "recording off", SS_LEFT | SS_ENDELLIPSIS);
        d->h_text[i] = view(d, IDC_H_TEXT0 + i, WS_HSCROLL);
    }

    /* Video: one child page with its own controls and controller */
    begin_page(d, TAB_VIDEO);
    d->video = dbg_video_create(d->pg[TAB_VIDEO], d->session, d->ui, d->mono, d->dpi);

    /* Sound: the TIA's two channels */
    begin_page(d, TAB_SOUND);
    d->sound_text = view(d, IDC_SOUND_TEXT, WS_HSCROLL);

    /* Symbols: a filter over the table, a row opens the Disassembly there */
    begin_page(d, TAB_SYMBOLS);
    d->sym_filter_label = dim_label(d, "Filter", SS_LEFT);
    d->sym_filter = field(d, IDC_SYM_FILTER, L"part of a name");
    d->sym_count = dim_label(d, "", SS_LEFT | SS_ENDELLIPSIS);
    d->sym_list = child_ex(d, WS_EX_CLIENTEDGE, "SysListView32", "",
                           WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_SINGLESEL, IDC_SYM_LIST, d->mono);
    SendMessageA(d->sym_list, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_FULLROWSELECT);
    {
        static const char *const symbol_columns[2] = { "Address", "Name" };
        LVCOLUMNA symbol_column;
        memset(&symbol_column, 0, sizeof symbol_column);
        symbol_column.mask = LVCF_TEXT | LVCF_WIDTH;
        for (i = 0; i < 2; i++) {
            symbol_column.pszText = (char *)symbol_columns[i];
            symbol_column.cx = S(d, i == 1 ? 360 : 80);
            SendMessageA(d->sym_list, LVM_INSERTCOLUMNA, (WPARAM)i, (LPARAM)&symbol_column);
        }
    }
    d->sym_shown = -1;

    /* FujiNet: the note and the log are UTF-8 (see wide_view_text) */
    begin_page(d, TAB_FUJINET);
    d->fuji_title = label(d, "FujiBus packets over BoIP", d->ui_bold, SS_LEFT);
    d->fuji_note = dim_label(d, "", SS_LEFT | SS_ENDELLIPSIS);
    d->fuji_clear = button(d, "Clear", IDC_FUJI_CLEAR);
    d->fuji_text = view(d, IDC_FUJI_TEXT, WS_HSCROLL);
    d->fuji_state_title = label(d, "BoIP link", d->ui_bold, SS_LEFT);
    d->fuji_state = view(d, IDC_FUJI_STATE, WS_HSCROLL);
    d->new_parent = NULL;
}

/* Help's contexts (data/help/help.md), one per tab */
static const char *const k_tab_contexts[TAB_COUNT] = {
    "debugger/console", "debugger/disassembly", "debugger/memory", "debugger/breakpoints", "debugger/watches",
    "debugger/cart", "debugger/trace", "debugger/history", "debugger/video", "debugger/sound",
    "debugger/symbols", "debugger/fujinet",
};

/* The tab control's index for a tab, or -1 when it is in a window of its own. */
static int tab_index(debugger *d, int page)
{
    int i;
    for (i = 0; i < d->ntabs; i++)
        if (d->tab_page[i] == page) return i;
    return -1;
}

static void show_page(debugger *d, int page)
{
    int p;
    d->page = page;
    /* Help, F1 and Ctrl+Shift+H open the tab's section */
    if (page >= 0 && page < TAB_COUNT) a2600_help_set_context(d->hwnd, k_tab_contexts[page], 1);
    for (p = 0; p < TAB_COUNT; p++)   /* a tab's controls follow its container */
        ShowWindow(d->pg[p], d->popped[p] || p == page ? SW_SHOW : SW_HIDE);
    show_bp_list(d);
    if (page == TAB_VIDEO && d->video) dbg_video_refresh(d->video, 1);
}

static void place(HWND h, int x, int y, int w, int hgt)
{
    MoveWindow(h, x, y, w > 1 ? w : 1, hgt > 1 ? hgt : 1, TRUE);
}

/* One tab's controls inside its container, which is pw x ph. */
static void layout_page(debugger *d, int page, int pw, int ph)
{
    const int bh = S(d, 26), eh = S(d, 24), lh = S(d, 18), gap = S(d, 6);
    const int px = 0, py = 0;
    int x, y, w, i;

    switch (page) {
    case TAB_CONSOLE:
        /* the transcript, then the prompt and its line */
        w = d->mono_w + S(d, 6);
        place(d->con_out, px, py, pw, ph - eh - S(d, 8));
        place(d->con_prompt, px, py + ph - eh + (eh - d->mono_h) / 2, w, d->mono_h);
        place(d->con_in, px + w + S(d, 2), py + ph - eh, pw - w - S(d, 2), eh);
        break;

    case TAB_DISASM:
        /* Follow PC and Jump to over the lines */
        place(d->bank, px, py, S(d, 110), S(d, 240));
        x = px + S(d, 118);
        w = btn_w(d, "Follow PC");
        place(d->follow_pc, x, py - S(d, 1), w, bh);
        x += w + S(d, 12);
        w = text_w(d, d->ui, "Jump to") + S(d, 6);
        place(d->jump_label, x, py + (eh - lh) / 2, w, lh);
        x += w;
        place(d->jump, x, py, S(d, 150), eh);
        x += S(d, 162);
        place(d->disasm_hint, x, py + (eh - lh) / 2, px + pw - x, lh);
        place(d->disasm, px, py + eh + S(d, 6), pw, ph - eh - S(d, 6));
        break;

    case TAB_MEMORY: {
        /* the dump, then Go to / paging / Write, then the search bar */
        const int row2 = py + ph - eh, row1 = row2 - eh - S(d, 8);
        place(d->mem_title, px, py, pw, lh);
        place(d->mem_view, px, py + lh + S(d, 4), pw, row1 - S(d, 6) - (py + lh + S(d, 4)));
        y = row1 + (eh - lh) / 2;
        x = px;
        w = text_w(d, d->ui, "Go to") + S(d, 6);
        place(d->goto_label, x, y, w, lh);
        x += w;
        place(d->mem_goto, x, row1, S(d, 170), eh);
        x += S(d, 176);
        x += S(d, 24);
        w = text_w(d, d->ui, "Write") + S(d, 6);
        place(d->write_label, x, y, w, lh);
        x += w;
        place(d->mem_write, x, row1, S(d, 260), eh);

        x = px;
        place(d->srch_kind, x, row2, S(d, 70), S(d, 120));
        x += S(d, 76);
        place(d->srch_entry, x, row2, S(d, 250), eh);
        x += S(d, 256);
        w = btn_w(d, "Find Next");
        place(d->srch_next, x, row2 - S(d, 1), w, bh);
        x += w + S(d, 4);
        w = btn_w(d, "Find Previous");
        place(d->srch_prev, x, row2 - S(d, 1), w, bh);
        x += w + S(d, 12);
        place(d->srch_note, x, row2 + (eh - lh) / 2, px + pw - x, lh);
        break;
    }

    case TAB_BREAKS: {
        /* "Stopped: ..." over the hint and the buttons; the list; the add row
         * and a message under it */
        const int msg_y = py + ph - lh, add_y = msg_y - S(d, 8) - eh;
        const int row_y = py + lh + S(d, 4), list_y = row_y + bh + S(d, 6);
        int bw = btn_w(d, "Clear All"), hw = btn_w(d, "Clear Hits"), rbw = btn_w(d, "Remove");
        place(d->bp_stop, px, py, pw, lh);
        place(d->bp_clear, px + pw - bw, row_y, bw, bh);
        place(d->bp_clear_hits, px + pw - bw - gap - hw, row_y, hw, bh);
        place(d->bp_remove, px + pw - bw - hw - 2 * gap - rbw, row_y, rbw, bh);
        place(d->bp_hint, px, row_y + (bh - lh) / 2, pw - bw - hw - rbw - 4 * gap, lh);
        place(d->bp_list, px, list_y, pw, add_y - gap - list_y);
        {
            RECT lc;
            int cw;
            GetClientRect(d->bp_list, &lc);
            cw = lc.right - S(d, 60) - S(d, 80) - GetSystemMetrics(SM_CXVSCROLL);
            SendMessageA(d->bp_list, LVM_SETCOLUMNWIDTH, 1, cw > S(d, 200) ? cw : S(d, 200));
        }
        place(d->bp_empty, px, list_y + S(d, 12), pw, lh);
        x = px;
        place(d->bp_type, x, add_y, S(d, 120), S(d, 240));
        x += S(d, 126);
        place(d->bp_start, x, add_y, S(d, 140), eh);
        x += S(d, 146);
        place(d->bp_end, x, add_y, S(d, 110), eh);
        x += S(d, 116);
        place(d->bp_bank, x, add_y, S(d, 100), S(d, 240));
        x += S(d, 106);
        bw = btn_w(d, "Add");
        place(d->bp_cond, x, add_y, px + pw - x - bw - gap, eh);
        place(d->bp_add, px + pw - bw, add_y - S(d, 1), bw, bh);
        place(d->bp_msg, px, msg_y, pw, lh);
        break;
    }

    case TAB_WATCHES: {
        const int msg_y = py + ph - lh, row_y = py + eh + S(d, 8);
        int bw = btn_w(d, "Add");
        w = text_w(d, d->ui, "Watch") + S(d, 6);
        place(d->wt_label, px, py + (eh - lh) / 2, w, lh);
        place(d->wt_entry, px + w, py, pw - w - bw - gap, eh);
        place(d->wt_add, px + pw - bw, py - S(d, 1), bw, bh);
        x = px;
        bw = btn_w(d, "Remove");
        place(d->wt_remove, x, row_y, bw, bh);
        x += bw + gap;
        bw = btn_w(d, "Memory");
        place(d->wt_mem, x, row_y, bw, bh);
        x += bw + gap;
        bw = btn_w(d, "Break on change");
        place(d->wt_brk, x, row_y, bw, bh);
        place(d->wt_list, px, row_y + bh + S(d, 6), pw, msg_y - gap - (row_y + bh + S(d, 6)));
        place(d->wt_msg, px, msg_y, pw, lh);
        break;
    }

    case TAB_CART: {
        /* the text, then the TIA row, the file buttons and the state slots */
        const int tia_y = py + ph - eh, files_y = tia_y - bh - gap, load_y = files_y - bh - gap, save_y = load_y - bh - gap;
        const int lab_w = text_w(d, d->ui, "Emulator states (Stella's slots 0-9)") + S(d, 8);
        place(d->cart_text, px, py, pw, save_y - gap - py);
        place(d->states_label, px, save_y + (bh - lh) / 2, lab_w, lh);
        for (i = 0; i < NSLOTS; i++) {
            const int sw = btn_w(d, "Save 0") - S(d, 14);
            place(d->slot_save[i], px + lab_w + i * (sw + S(d, 4)), save_y, sw, bh);
            place(d->slot_load[i], px + lab_w + i * (sw + S(d, 4)), load_y, sw, bh);
        }
        w = text_w(d, d->ui, "Save to a file") + S(d, 8);
        place(d->files_label, px, files_y + (bh - lh) / 2, lab_w > w ? lab_w : w, lh);
        x = px + (lab_w > w ? lab_w : w);
        for (i = 0; i < NSAVE; i++) {
            w = btn_w(d, k_save_titles[i]);
            place(d->file_btn[i], x, files_y, w, bh);
            x += w + gap;
        }
        w = text_w(d, d->ui, "TIA register or strobe") + S(d, 8);
        place(d->tia_label, px, tia_y + (eh - lh) / 2, w, lh);
        x = px + w;
        place(d->tia_reg, x, tia_y, S(d, 210), eh);
        x += S(d, 216);
        place(d->tia_val, x, tia_y, S(d, 90), eh);
        x += S(d, 96);
        w = btn_w(d, "Set");
        place(d->tia_set, x, tia_y - S(d, 1), w, bh);
        break;
    }

    case TAB_TRACE:
    case TAB_HISTORY: {
        const int k = page == TAB_HISTORY;
        x = px;
        w = btn_w(d, "Record");
        place(d->h_rec[k], x, py - S(d, 1), w, bh);
        x += w + gap;
        w = btn_w(d, "Clear");
        place(d->h_clear[k], x, py - S(d, 1), w, bh);
        x += w + S(d, 16);
        place(d->h_filter[k], x, py, S(d, 100), S(d, 120));
        x += S(d, 106);
        place(d->h_range[k], x, py, S(d, 190), eh);
        x += S(d, 202);
        place(d->h_note[k], x, py + (eh - lh) / 2, px + pw - x, lh);
        place(d->h_text[k], px, py + bh + S(d, 6), pw, ph - bh - S(d, 6));
        break;
    }

    case TAB_VIDEO:
        if (d->video) place(d->video, px, py, pw, ph);
        break;

    case TAB_SOUND:
        place(d->sound_text, px, py, pw, ph);
        break;

    case TAB_SYMBOLS: {
        w = text_w(d, d->ui, "Filter") + S(d, 6);
        place(d->sym_filter_label, px, py + (eh - lh) / 2, w, lh);
        place(d->sym_filter, px + w, py, S(d, 260), eh);
        place(d->sym_count, px + w + S(d, 272), py + (eh - lh) / 2, pw - w - S(d, 272), lh);
        place(d->sym_list, px, py + eh + S(d, 6), pw, ph - eh - S(d, 6));
        break;
    }

    case TAB_FUJINET: {
        /* the packets left (a note and Clear over them), the link's state and
         * counts right */
        const int rw = pw / 3 > S(d, 340) ? S(d, 340) : pw / 3 > S(d, 220) ? pw / 3 : S(d, 220);
        const int lw = pw - rw - S(d, 12) > S(d, 120) ? pw - rw - S(d, 12) : S(d, 120);
        const int cw = btn_w(d, "Clear");
        place(d->fuji_title, px, py, lw, lh);
        place(d->fuji_clear, px + lw - cw, py + lh + S(d, 4), cw, bh);
        place(d->fuji_note, px, py + lh + S(d, 4) + (bh - lh) / 2, lw - cw - gap, lh);
        place(d->fuji_text, px, py + lh + bh + S(d, 10), lw, ph - lh - bh - S(d, 10));
        place(d->fuji_state_title, px + lw + S(d, 12), py, rw, lh);
        place(d->fuji_state, px + lw + S(d, 12), py + lh + S(d, 4), rw, ph - lh - S(d, 4));
        break;
    }
    default: break;
    }
    (void)i;
}

/* The registers strip: PC SP AF BC DE HL IX IY and the shadow set with I R IM
 * (one line, or two when narrow), then the flags and the beam. Fills w x h
 * at (x, y); returns its height. */
static int strip_height(debugger *d, int width, int *rows)
{
    const int eh = S(d, 24), gap = S(d, 6);
    const int lw = text_w(d, d->ui, "WWW") + S(d, 4), rw = d->mono_w * 5 + S(d, 10);
    const int one_line = NREGS * (lw + S(d, 4) + rw + S(d, 8)) <= width;
    *rows = one_line ? 1 : 2;
    return *rows * (eh + gap) + S(d, 22) + gap;
}

static void layout_strip(debugger *d, int x0, int y0, int width)
{
    const int eh = S(d, 24), gap = S(d, 6), lh = S(d, 18);
    const int lw = text_w(d, d->ui, "WWW") + S(d, 4), rw = d->mono_w * 5 + S(d, 10);
    int rows, i, x, y;
    strip_height(d, width, &rows);
    {
        const int per_row = (NREGS + rows - 1) / rows;
        const int cell = (width + S(d, 8)) / per_row > lw + S(d, 4) + rw + S(d, 8) ?
                         lw + S(d, 4) + rw + S(d, 8) : (width + S(d, 8)) / per_row;
        for (i = 0; i < NREGS; i++) {
            const int row = i / per_row, col = i % per_row;
            const int ry = y0 + row * (eh + gap);
            place(d->reg_label[i], x0 + col * cell, ry + (eh - lh) / 2, lw, lh);
            place(d->reg[i], x0 + col * cell + lw + S(d, 4), ry, rw, eh);
        }
    }
    d->strip_rows = rows;
    y = y0 + rows * (eh + gap);
    x = x0;
    for (i = 0; i < NCHECKS; i++) {
        const int w = text_w(d, d->ui, k_chk_names[i]) + S(d, 28);
        place(d->chk[i], x, y, w, S(d, 22));
        x += w + S(d, 6);
    }
    place(d->beam, x + S(d, 16), y + (S(d, 22) - lh) / 2, x0 + width - x - S(d, 16), lh);
}

static void layout(debugger *d)
{
    RECT client, page;
    int x, y, w, i, px, py, pw, ph, srows, sh;
    const int bh = S(d, 26), eh = S(d, 24), lh = S(d, 18), gap = S(d, 6);

    if (!d->tabs) return;
    GetClientRect(d->hwnd, &client);

    /* The toolbar, two rows: stepping, then breakpoints, symbols, save and
     * pop out; the status after the first row, or (too narrow for the whole
     * stop reason beside it) on a line of its own. */
    {
        HWND stepping[9] = { d->run_btn, d->step_btn, d->over_btn, d->out_btn, d->run_to_btn, d->scan_btn, d->frame_btn,
                             d->rewind_btn, d->unwind_btn };
        const char *stepping_text[9] = { "Stop (F5)", "Step (F7)", "Step Over (F8)", "Step Out (Shift+F8)",
                                         "Run to Cursor", "Scan+1", "Frame+1", "Rewind", "Unwind" };
        HWND tools[3] = { d->clear_bps_btn, d->sym_btn, d->clear_sym_btn };
        const char *tools_text[3] = { "Clear Breakpoints", "Load Symbols", "Clear Symbols" };
        char status_text[220];
        int status_w, status_own_line;
        x = S(d, 8);
        for (i = 0; i < 9; i++) {
            w = btn_w(d, stepping_text[i]);
            place(stepping[i], x, S(d, 8), w, bh);
            x += w + gap + (i == 4 ? S(d, 14) : 0);
        }
        {
            const int hw = btn_w(d, "Help");
            const int sright = client.right - S(d, 8);
            GetWindowTextA(d->status, status_text, sizeof status_text);
            status_w = text_w(d, d->ui, status_text) + S(d, 16);
            if (status_w < S(d, 220)) status_w = S(d, 220);
            y = S(d, 8) + bh + S(d, 6);
            status_own_line = sright - x < status_w;
            if (!status_own_line) place(d->status, x, S(d, 8) + (bh - lh) / 2, sright - x, lh);
            place(d->help_btn, client.right - S(d, 8) - hw, y - S(d, 1), hw, bh);
        }
        x = S(d, 8);
        for (i = 0; i < 3; i++) {
            if (i == 1) {
                const int label_w = text_w(d, d->ui, "Break at:") + S(d, 6);
                place(d->break_at_label, x, y + (eh - lh) / 2, label_w, lh);
                x += label_w;
                place(d->break_at, x, y, S(d, 130), eh);
                x += S(d, 130) + S(d, 14);
            }
            w = btn_w(d, tools_text[i]);
            place(tools[i], x, y - S(d, 1), w, bh);
            x += w + gap;
        }
        w = btn_w(d, "Pop Out");
        place(d->popout_btn, x, y - S(d, 1), w, bh);
        y += bh + S(d, 6);
        if (status_own_line) {          /* too narrow for the whole stop reason beside the buttons */
            place(d->status, S(d, 8), y, client.right - S(d, 16), lh);
            y += lh + S(d, 4);
        }
    }

    /* The tab control is the strip alone: the pages sit on the window's own
     * background, which their captions are drawn on. */
    SetRect(&page, 0, 0, S(d, 400), S(d, 400));
    SendMessageA(d->tabs, TCM_ADJUSTRECT, FALSE, (LPARAM)&page);
    place(d->tabs, S(d, 8), y, client.right - S(d, 16), page.top);
    px = S(d, 12);
    py = y + page.top + S(d, 6);
    pw = client.right - 2 * px;

    /* the registers strip under the pages, on every tab */
    sh = strip_height(d, pw, &srows);
    ph = client.bottom - S(d, 6) - sh - S(d, 6) - py;
    if (pw < S(d, 300)) pw = S(d, 300);
    if (ph < S(d, 200)) ph = S(d, 200);
    layout_strip(d, px, py + ph + S(d, 6), pw);

    for (i = 0; i < TAB_COUNT; i++) {
        int cw = pw, ch = ph;
        if (d->popped[i]) {
            RECT pc;
            GetClientRect(d->popped[i], &pc);
            cw = pc.right - S(d, 16);
            ch = pc.bottom - S(d, 46);
            place(d->pg[i], S(d, 8), S(d, 38), cw, ch);
        } else {
            place(d->pg[i], px, py, pw, ph);
        }
        layout_page(d, i, cw > 1 ? cw : 1, ch > 1 ? ch : 1);
    }

    /* everything again: what the controls left behind where they were */
    RedrawWindow(d->hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
    for (i = 0; i < TAB_COUNT; i++)
        if (d->popped[i]) RedrawWindow(d->popped[i], NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

/* ---- peeling a tab off into a window of its own, and putting it back ------------------ */

static void goto_tab(debugger *d, int page)
{
    const int i = tab_index(d, page);
    if (d->popped[page]) {
        SetForegroundWindow(d->popped[page]);
        return;
    }
    if (i >= 0) {
        SendMessageA(d->tabs, TCM_SETCURSEL, (WPARAM)i, 0);
        show_page(d, page);
        layout(d);
        refresh_visible(d);
    }
}

static void dock_page(debugger *d, int page)
{
    TCITEMA item;
    char text[32];
    HWND pop = d->popped[page];
    int pos = 0, p, i;

    if (!pop) return;
    d->popped[page] = NULL;
    SetParent(d->pg[page], d->hwnd);
    for (p = 0; p < page; p++)   /* back where it was in the order */
        if (!d->popped[p]) pos++;
    for (i = d->ntabs; i > pos; i--) d->tab_page[i] = d->tab_page[i - 1];
    d->tab_page[pos] = page;
    d->ntabs++;
    memset(&item, 0, sizeof item);
    item.mask = TCIF_TEXT;
    tab_text(page, text, sizeof text);
    item.pszText = text;
    SendMessageA(d->tabs, TCM_INSERTITEMA, (WPARAM)pos, (LPARAM)&item);
    DestroyWindow(pop);
    SendMessageA(d->tabs, TCM_SETCURSEL, (WPARAM)pos, 0);
    show_page(d, page);
    SetWindowPos(d->tabs, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    layout(d);
    refresh_visible(d);
}

static void pop_out(debugger *d, int page)
{
    HWND pop, dock;
    HINSTANCE inst = (HINSTANCE)GetWindowLongPtrA(d->hwnd, GWLP_HINSTANCE);
    char title[96];
    int idx, i, next;

    if (page < 0 || page >= TAB_COUNT || d->popped[page] || d->ntabs <= 1) {
        if (d->ntabs <= 1) toast(d, "One tab always stays in the main window");
        return;
    }
    idx = tab_index(d, page);
    if (idx < 0) return;
    SendMessageA(d->tabs, TCM_DELETEITEM, (WPARAM)idx, 0);
    for (i = idx; i < d->ntabs - 1; i++) d->tab_page[i] = d->tab_page[i + 1];
    d->ntabs--;

    snprintf(title, sizeof title, "Atari 2600 Debugger - %s", k_tabs[page].label);
    pop = CreateWindowExA(0, POP_CLASS, title, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                          S(d, 900), S(d, 620), d->hwnd, NULL, inst, NULL);
    SetWindowLongPtrA(pop, GWLP_USERDATA, page);
    dock = CreateWindowExA(0, "BUTTON", "Dock", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, S(d, 8), S(d, 6),
                           btn_w(d, "Dock"), S(d, 26), pop, (HMENU)(INT_PTR)IDC_DOCK, inst, NULL);
    SendMessageA(dock, WM_SETFONT, (WPARAM)d->ui, TRUE);
    a2600_help_set_context(pop, k_tab_contexts[page], 1);
    SetParent(d->pg[page], pop);
    d->popped[page] = pop;
    /* the tab shown stays (when it was the popped one, its neighbour takes over) */
    next = d->page == page ? (idx < d->ntabs ? idx : d->ntabs - 1) : tab_index(d, d->page);
    if (next < 0) next = 0;
    SendMessageA(d->tabs, TCM_SETCURSEL, (WPARAM)next, 0);
    show_page(d, d->tab_page[next]);
    layout(d);
    ShowWindow(pop, SW_SHOW);
    refresh_visible(d);
}

/* A tab container: its controls' messages go to the debugger window. */
static LRESULT CALLBACK page_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    HWND main = (HWND)GetWindowLongPtrA(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_COMMAND:
    case WM_NOTIFY:
    case WM_DRAWITEM:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        if (main && g_dbg) return SendMessageA(main, msg, wp, lp);
        break;
    case WM_ERASEBKGND: {
        RECT c;
        GetClientRect(hwnd, &c);
        FillRect((HDC)wp, &c, GetSysColorBrush(COLOR_BTNFACE));
        return 1;
    }
    default: break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* The window a popped-out tab lives in. */
static LRESULT CALLBACK pop_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    const int page = (int)GetWindowLongPtrA(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_SIZE:
        if (g_dbg) layout(g_dbg);
        return 0;
    case WM_GETMINMAXINFO:
        ((MINMAXINFO *)lp)->ptMinTrackSize.x = 400;
        ((MINMAXINFO *)lp)->ptMinTrackSize.y = 300;
        return 0;
    case WM_COMMAND:
        if (g_dbg && LOWORD(wp) == IDC_DOCK) dock_page(g_dbg, page);
        return 0;
    case WM_MOUSEWHEEL:
        if (g_dbg) return SendMessageA(g_dbg->hwnd, msg, wp, lp);
        break;
    case WM_CLOSE:
        if (g_dbg) dock_page(g_dbg, page);
        else DestroyWindow(hwnd);
        return 0;
    default: break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* ---- the window ------------------------------------------------------------------- */

static void hide_window(debugger *d)
{
    KillTimer(d->hwnd, TIMER_REFRESH);
    if (a2600debug_is_stopped(d->dbg)) a2600debug_resume(d->dbg);    /* lets the machine run */
    ShowWindow(d->hwnd, SW_HIDE);
}

static void attach_and_refresh(debugger *d)
{
    a2600debug_stop(d->dbg);      /* so there is something to look at */
    d->was_stopped = 0;
    d->seen_gen = a2600debug_generation(d->dbg);
    SetTimer(d->hwnd, TIMER_REFRESH, 100, NULL);
    settle_and_sync(d, 1);
}

/* Stop/Run: the accent with white text while the machine is stopped (the
 * button that will start it), the stock look while it runs. */
static void draw_run_button(debugger *d, const DRAWITEMSTRUCT *di)
{
    char text[32];
    const int stopped = a2600debug_is_stopped(d->dbg);
    RECT r = di->rcItem;
    if (stopped) {
        FillRect(di->hDC, &r, a2600_accent_brush());
        FrameRect(di->hDC, &r, (HBRUSH)GetStockObject(GRAY_BRUSH));
        SetTextColor(di->hDC, A2600_ACCENT_TEXT);
    } else {
        DrawFrameControl(di->hDC, &r, DFC_BUTTON, DFCS_BUTTONPUSH | ((di->itemState & ODS_SELECTED) ? DFCS_PUSHED : 0));
        SetTextColor(di->hDC, GetSysColor(COLOR_BTNTEXT));
    }
    SetBkMode(di->hDC, TRANSPARENT);
    GetWindowTextA(di->hwndItem, text, sizeof text);
    SelectObject(di->hDC, d->ui);
    DrawTextA(di->hDC, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    if (di->itemState & ODS_FOCUS) {
        InflateRect(&r, -3, -3);
        DrawFocusRect(di->hDC, &r);
    }
}


static LRESULT on_notify(debugger *d, NMHDR *nh)
{
    if (nh->hwndFrom == d->tabs && nh->code == (UINT)TCN_SELCHANGE) {
        const int sel = (int)SendMessageA(d->tabs, TCM_GETCURSEL, 0, 0);
        if (sel >= 0 && sel < d->ntabs) {
            show_page(d, d->tab_page[sel]);
            refresh_visible(d);
        }
        return 0;
    }
    if (nh->hwndFrom == d->follow_pc && nh->code == (UINT)NM_CUSTOMDRAW)
        return a2600_accent_customdraw((const NMCUSTOMDRAW *)nh, following(d));
    if (nh->code == (UINT)NM_CUSTOMDRAW) {
        /* the Record toggles: the accent while recording */
        int k;
        for (k = 0; k < 2; k++)
            if (nh->hwndFrom == d->h_rec[k])
                return a2600_accent_customdraw((const NMCUSTOMDRAW *)nh,
                                               SendMessageA(d->h_rec[k], BM_GETCHECK, 0, 0) == BST_CHECKED);
    }
    if (nh->hwndFrom == d->sym_list) {
        if (nh->code == (UINT)NM_DBLCLK || nh->code == (UINT)NM_RETURN) on_symbol_activate(d);
        else if (nh->code == (UINT)LVN_KEYDOWN && ((const NMLVKEYDOWN *)nh)->wVKey == VK_RETURN) on_symbol_activate(d);
        return 0;
    }
    if (nh->hwndFrom != d->bp_list) return 0;
    if (nh->code == (UINT)LVN_ITEMCHANGED) {
        const NMLISTVIEW *nm = (const NMLISTVIEW *)nh;
        /* a row's box: its breakpoint on or off (the list follows after) */
        if (!d->updating && nm->iItem >= 0 && nm->iItem < d->nbps && (nm->uChanged & LVIF_STATE) &&
            ((nm->uNewState ^ nm->uOldState) & LVIS_STATEIMAGEMASK) && (nm->uOldState & LVIS_STATEIMAGEMASK)) {
            const int on = ((nm->uNewState & LVIS_STATEIMAGEMASK) >> 12) == 2;
            if (on != (d->bps[nm->iItem].enabled != 0)) {
                a2600debug_bp_enable(d->dbg, d->bps[nm->iItem].id, on);
                PostMessageA(d->hwnd, WM_DBG_SYNC, 0, 0);
            }
        }
    } else if (nh->code == (UINT)LVN_KEYDOWN) {
        /* after the list's own handling: removing rebuilds it */
        if (((const NMLVKEYDOWN *)nh)->wVKey == VK_DELETE)
            PostMessageA(d->hwnd, WM_COMMAND, MAKEWPARAM(IDC_BP_REMOVE, BN_CLICKED), 0);
    } else if (nh->code == (UINT)NM_DBLCLK) {
        /* an address breakpoint's row opens the disassembly there */
        const int i = bp_selected(d);
        if (i >= 0 && (d->bps[i].type & A2600DEBUG_BP_EXEC)) {
            char a[16];
            snprintf(a, sizeof a, "$%04X", d->bps[i].start);
            goto_tab(d, TAB_DISASM);
            SetWindowTextA(d->jump, a);
            on_jump(d);
        }
    }
    return 0;
}

static LRESULT CALLBACK dbg_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    debugger *d = g_dbg;
    if (msg == WM_GETMINMAXINFO) {
        const int dpi = d && d->dpi ? d->dpi : 96;
        ((MINMAXINFO *)lp)->ptMinTrackSize.x = MulDiv(860, dpi, 96);
        ((MINMAXINFO *)lp)->ptMinTrackSize.y = MulDiv(560, dpi, 96);
        return 0;
    }
    if (!d || d->hwnd != hwnd) return DefWindowProcA(hwnd, msg, wp, lp);

    switch (msg) {
    case WM_SIZE:
        layout(d);
        return 0;
    case WM_DPICHANGED: {
        const RECT *r = (const RECT *)lp;
        d->dpi = HIWORD(wp);
        make_fonts(d);       /* (also the controls of popped-out tabs) */
        size_columns(d);
        SetWindowPos(hwnd, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        layout(d);
        InvalidateRect(hwnd, NULL, TRUE);
        return 0;
    }
    case WM_TIMER:
        if (wp == TIMER_REFRESH) tick(d);
        return 0;
    case WM_DBG_ACCEPT:
        on_accept(d, (int)wp);
        return 0;
    case WM_DBG_TAB:
        complete_console(d);
        return 0;
    case WM_DBG_HISTORY:
        history_step(d, (int)wp);
        return 0;
    case WM_DBG_SYNC:
        resync(d);
        return 0;
    case WM_NOTIFY:
        return on_notify(d, (NMHDR *)lp);
    case WM_DRAWITEM:
        if (wp == IDC_RUN) {
            draw_run_button(d, (const DRAWITEMSTRUCT *)lp);
            return TRUE;
        }
        break;
    case WM_CTLCOLORSTATIC: {
        HWND h = (HWND)lp;
        HDC dc = (HDC)wp;
        if (in_list(h, d->views, d->nviews)) {
            /* read-only views on the window colour, as text views are */
            SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
            SetBkColor(dc, GetSysColor(COLOR_WINDOW));
            return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
        }
        if (h == d->bp_msg || h == d->wt_msg) {
            SetTextColor(dc, RGB(0xc0, 0x1c, 0x28));
            SetBkColor(dc, GetSysColor(COLOR_BTNFACE));
            return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
        }
        if (h == d->status && notice_showing(d)) {
            SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
            SetBkColor(dc, GetSysColor(COLOR_BTNFACE));
            return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
        }
        if (in_list(h, d->dim, d->ndim)) {
            SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
            SetBkColor(dc, GetSysColor(COLOR_BTNFACE));
            return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
        }
        break;
    }
    case WM_MOUSEWHEEL: {
        /* the disassembly scrolls under the pointer, focused or not */
        POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        RECT r;
        GetWindowRect(d->disasm, &r);
        if (page_visible(d, TAB_DISASM) && PtInRect(&r, p)) return SendMessageA(d->disasm, msg, wp, lp);
        GetWindowRect(d->mem_view, &r);
        if (page_visible(d, TAB_MEMORY) && PtInRect(&r, p)) return SendMessageA(d->mem_view, msg, wp, lp);
        break;
    }
    case WM_COMMAND: {
        const int id = LOWORD(wp);
        if (id == IDC_SYM_FILTER && HIWORD(wp) == EN_CHANGE) {
            refresh_symbols(d);
            return 0;
        }
        if (id >= IDC_CHK0 && id <= IDC_CHK_LAST && HIWORD(wp) == BN_CLICKED) {
            on_check(d, id - IDC_CHK0);
            return 0;
        }
        if (id >= IDC_SAVE_KIND0 && id <= IDC_SAVE_KIND_LAST) {
            on_save(d, id - IDC_SAVE_KIND0);
            return 0;
        }
        if (id >= IDC_SLOT_SAVE0 && id <= IDC_SLOT_SAVE_LAST) {
            char msg[64];
            a2600debug_state_save(d->dbg, id - IDC_SLOT_SAVE0);
            snprintf(msg, sizeof msg, "Saved state slot %d", id - IDC_SLOT_SAVE0);
            toast(d, msg);
            resync(d);
            return 0;
        }
        if (id >= IDC_SLOT_LOAD0 && id <= IDC_SLOT_LOAD_LAST) {
            char msg[64];
            a2600debug_state_load(d->dbg, id - IDC_SLOT_LOAD0);
            snprintf(msg, sizeof msg, "Loaded state slot %d", id - IDC_SLOT_LOAD0);
            toast(d, msg);
            resync(d);
            return 0;
        }
        switch (id) {
        case IDC_RUN: on_run(d); return 0;
        case IDC_STEP: a2600debug_step(d->dbg); settle_and_sync(d, 1); return 0;
        case IDC_OVER: a2600debug_step_over(d->dbg); settle_and_sync(d, 1); return 0;
        case IDC_OUT: a2600debug_step_out(d->dbg); settle_and_sync(d, 1); return 0;
        case IDC_SCAN: a2600debug_scanline(d->dbg, 1); settle_and_sync(d, 1); return 0;
        case IDC_FRAME: a2600debug_frame(d->dbg, 1); settle_and_sync(d, 1); return 0;
        case IDC_REWIND: a2600debug_rewind(d->dbg, 1); settle_and_sync(d, 1); return 0;
        case IDC_UNWIND: a2600debug_unwind(d->dbg, 1); settle_and_sync(d, 1); return 0;
        case IDC_BANK:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                d->disasm_bank = (int)SendMessageA(d->bank, CB_GETCURSEL, 0, 0) - 1;
                if (d->disasm_bank >= 0) {
                    SendMessageA(d->follow_pc, BM_SETCHECK, BST_UNCHECKED, 0);
                    InvalidateRect(d->follow_pc, NULL, FALSE);
                }
                d->disasm_first = 0;
                refresh_disasm(d);
            }
            return 0;
        case IDC_TIA_SET: on_tia_set(d); return 0;
        case IDC_CLEAR_BPS: on_clear_breakpoints(d); return 0;
        case IDC_CLEAR_SYMBOLS: on_clear_symbols(d); return 0;
        case IDC_RUN_TO: on_run_to(d); return 0;
        case IDC_TOGGLE_BP: on_toggle_bp(d); return 0;
        case IDC_HELP_BTN: a2600_help_for(d->hwnd); return 0;
        case IDC_SYMBOLS: on_symbols(d); return 0;
        case IDC_FOLLOW_PC: refresh_disasm(d); return 0;
        case IDC_POPOUT:
            pop_out(d, d->page);
            return 0;
        case IDC_SRCH_NEXT: on_search(d, 0); return 0;
        case IDC_SRCH_PREV: on_search(d, 1); return 0;
        case IDC_WT_ADD: on_watch_add(d); return 0;
        case IDC_WT_REMOVE: on_watch_remove(d); return 0;
        case IDC_WT_MEM: on_watch_memory(d); return 0;
        case IDC_WT_BRK: on_watch_break(d); return 0;
        case IDC_WT_LIST:
            if (HIWORD(wp) == LBN_DBLCLK) on_watch_memory(d);
            return 0;
        case IDC_H_REC0: on_record(d, 0); return 0;
        case IDC_H_REC1: on_record(d, 1); return 0;
        case IDC_H_CLEAR0: on_hist_clear(d, 0); return 0;
        case IDC_H_CLEAR1: on_hist_clear(d, 1); return 0;
        case IDC_H_FILTER0:
            if (HIWORD(wp) == CBN_SELCHANGE) on_hist_filter(d, 0);
            return 0;
        case IDC_H_FILTER1:
            if (HIWORD(wp) == CBN_SELCHANGE) on_hist_filter(d, 1);
            return 0;
        case IDC_BP_CLEAR_HITS:
            a2600debug_bp_clear_hits(d->dbg);
            d->bp_sig_ok = 0;
            resync(d);
            return 0;
        case IDC_BP_TYPE:
            if (HIWORD(wp) == CBN_SELCHANGE) on_bp_type(d);
            return 0;
        case IDC_BP_ADD: on_bp_add(d); return 0;
        case IDC_BP_REMOVE: on_bp_remove(d); return 0;
        case IDC_BP_CLEAR: on_clear_breakpoints(d); return 0;
        case IDC_FUJI_CLEAR:
            a2600debug_fujinet_trace_clear(d->dbg);
            refresh_fujinet(d, 1);
            return 0;
        case IDCANCEL: hide_window(d); return 0;
        default: break;
        }
        break;
    }
    case WM_CLOSE:
        /* Hide and let the machine run; F12 brings it back, stopped. */
        hide_window(d);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, TIMER_REFRESH);
        DeleteObject(d->ui);
        DeleteObject(d->ui_bold);
        DeleteObject(d->mono);
        DeleteObject(d->bp_on);
        DestroyAcceleratorTable(d->accel);
        free(d);
        g_dbg = NULL;
        return 0;
    default: break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* ---- entry points ----------------------------------------------------------------- */

static void register_class(HINSTANCE inst, const char *name, WNDPROC proc, HBRUSH background, UINT style)
{
    WNDCLASSEXA wc;
    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.style = style;
    wc.lpfnWndProc = proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = background;
    wc.lpszClassName = name;
    if (proc == dbg_proc || proc == pop_proc) wc.hIcon = LoadIconA(inst, MAKEINTRESOURCEA(IDI_APPICON));
    RegisterClassExA(&wc);
}

/* A2600_DEBUGGER_TAB: a tab number, or a tab's name or label (any case), or
 * the start of one, or of a word of a label ("memory", "map", "i/o"...). */
static int tab_from_env_var(const char *var)
{
    const char *tab = getenv(var);
    char *end;
    long n;
    size_t len;
    int i, pass;
    if (!tab || !*tab) return -1;
    if (_stricmp(tab, "io") == 0 || _stricmp(tab, "i/o") == 0) return TAB_CART;
    n = strtol(tab, &end, 10);
    if (end != tab && *end == '\0')
        return (n >= 0 && n < TAB_COUNT) ? (int)n : -1;
    len = strlen(tab);
    for (pass = 0; pass < 3; pass++)
        for (i = 0; i < TAB_COUNT; i++) {
            if (pass == 0 && _strnicmp(tab, k_tabs[i].name, len) == 0) return i;
            if (pass == 1 && _strnicmp(tab, k_tabs[i].label, len) == 0) return i;
            if (pass == 2) {
                const char *p = k_tabs[i].label;
                while ((p = strchr(p, ' ')) != NULL)
                    if (_strnicmp(tab, ++p, len) == 0) return i;
            }
        }
    return -1;
}

static int tab_from_env(void)
{
    return tab_from_env_var("A2600_DEBUGGER_TAB");
}

/* The env hooks for headless captures, once the window is up: a watch, a
 * break-on-change, the Memory tab's Go to, recording and a popped-out tab. */
static void env_hooks(debugger *d)
{
    const char *v;
    int tab;
    if ((v = getenv("A2600_DEBUGGER_WATCH")) != NULL && *v) {
        /* a comma-separated list */
        char list[512], *item, *save = NULL;
        snprintf(list, sizeof list, "%s", v);
        for (item = strtok_s(list, ",", &save); item; item = strtok_s(NULL, ",", &save)) {
            item = strip(item);
            if (*item) a2600watch_add(item);
        }
    }
    if ((v = getenv("A2600_DEBUGGER_WATCH_BREAK")) != NULL && *v) {
        /* a watch, and a break on change armed over it */
        const int index = a2600watch_add(v);
        uint16_t lo, hi;
        char err[96];
        if (index >= 0 && a2600watch_range(d->dbg, index, &lo, &hi)) break_on_change(d, lo, hi, err, sizeof err);
        else if (parse_range_ui(d, v, &lo, &hi, err, sizeof err)) break_on_change(d, lo, hi, err, sizeof err);
    }
    if ((v = getenv("A2600_DEBUGGER_MEM_GOTO")) != NULL && *v) {
        SetWindowTextA(d->mem_goto, v);
        on_mem_goto(d);
    }
    if ((v = getenv("A2600_DEBUGGER_HISTORY")) != NULL && *v && *v != '0') a2600debug_trace_enable(d->dbg, 1);
    if ((tab = tab_from_env_var("A2600_DEBUGGER_POPOUT")) >= 0) pop_out(d, tab);
    resync(d);
}

/* Symbols beside the cartridge (<rom>.sym/.lst), once, when the window
 * is first made; the engine does not look for them itself. */
static void load_cart_symbols(debugger *d)
{
    char msg[512];
    msg[0] = '\0';
    if (a2600debug_load_symbols(d->dbg, msg, sizeof msg) > 0 && msg[0]) console_output(d, msg);
}

void a2600_debugger_show(HWND parent, a2600session *session)
{
    static int registered;
    INITCOMMONCONTROLSEX icc;
    HINSTANCE inst;
    debugger *d;
    ACCEL accels[6];
    int tab, w, h;
    MONITORINFO mi;

    if (g_dbg) {
        d = g_dbg;
        ShowWindow(d->hwnd, IsIconic(d->hwnd) ? SW_RESTORE : SW_SHOW);
        SetForegroundWindow(d->hwnd);
        if (!IsWindowVisible(d->hwnd) || !a2600debug_is_stopped(d->dbg))
            attach_and_refresh(d);
        else
            resync(d);
        return;
    }

    icc.dwSize = sizeof icc;
    icc.dwICC = ICC_TAB_CLASSES | ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);
    inst = (HINSTANCE)GetWindowLongPtrA(parent, GWLP_HINSTANCE);
    if (!inst) inst = GetModuleHandleA(NULL);
    if (!registered) {
        register_class(inst, DBG_CLASS, dbg_proc, (HBRUSH)(COLOR_BTNFACE + 1), CS_HREDRAW | CS_VREDRAW);
        register_class(inst, DISASM_CLASS, disasm_proc, NULL, CS_DBLCLKS);
        register_class(inst, MEMVIEW_CLASS, memview_proc, NULL, 0);
        register_class(inst, PAGE_CLASS, page_proc, (HBRUSH)(COLOR_BTNFACE + 1), 0);
        register_class(inst, POP_CLASS, pop_proc, (HBRUSH)(COLOR_BTNFACE + 1), CS_HREDRAW | CS_VREDRAW);
        registered = 1;
    }

    d = calloc(1, sizeof *d);
    if (!d) return;
    d->session = session;
    d->dbg = a2600session_debugger(session);
    if (!d->dbg) {
        free(d);
        MessageBoxA(parent, "The debugger could not be created for this session.", "Debugger Unavailable",
                    MB_ICONERROR);
        return;
    }
    d->selected = -1;
    d->pc_line = -1;
    d->pc_listed = -1;
    d->disasm_bank = -1;
    d->fuji_count = -1;
    g_dbg = d;

    /* the start size, held inside the monitor's work area */
    d->dpi = window_dpi(parent);
    w = S(d, START_W);
    h = S(d, START_H);
    mi.cbSize = sizeof mi;
    if (GetMonitorInfoA(MonitorFromWindow(parent, MONITOR_DEFAULTTONEAREST), &mi)) {
        if (w > mi.rcWork.right - mi.rcWork.left - S(d, 40)) w = mi.rcWork.right - mi.rcWork.left - S(d, 40);
        if (h > mi.rcWork.bottom - mi.rcWork.top - S(d, 40)) h = mi.rcWork.bottom - mi.rcWork.top - S(d, 40);
    }
    /* owned by the main window: over it, and gone with it */
    d->hwnd = CreateWindowExA(0, DBG_CLASS, "Atari 2600 Debugger", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                              CW_USEDEFAULT, CW_USEDEFAULT, w, h, parent, NULL, inst, NULL);
    if (!d->hwnd) {
        free(d);
        g_dbg = NULL;
        return;
    }
    if (window_dpi(d->hwnd) != d->dpi) {
        /* placed on a monitor of another DPI than the parent's */
        const int was = d->dpi;
        d->dpi = window_dpi(d->hwnd);
        SetWindowPos(d->hwnd, NULL, 0, 0, MulDiv(w, d->dpi, was), MulDiv(h, d->dpi, was),
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    make_fonts(d);
    d->bp_on = CreateSolidBrush(RGB(0xe0, 0x1b, 0x24));

    build_controls(d);

    /* the tab body under every page: its repaints never cover a label or a
     * pane (the Win32 FujiNet-tab overlap of the 2026-11-01 notes) */
    SetWindowPos(d->tabs, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    /* F5/F7/F8/Shift+F8/F9/F12 work wherever the focus is in the
     * window. */
    accels[0].fVirt = FVIRTKEY;          accels[0].key = VK_F5;  accels[0].cmd = IDC_RUN;
    accels[1].fVirt = FVIRTKEY;          accels[1].key = VK_F7;  accels[1].cmd = IDC_STEP;
    accels[2].fVirt = FVIRTKEY;          accels[2].key = VK_F8;  accels[2].cmd = IDC_OVER;
    accels[3].fVirt = FVIRTKEY | FSHIFT; accels[3].key = VK_F8;  accels[3].cmd = IDC_OUT;
    accels[4].fVirt = FVIRTKEY;          accels[4].key = VK_F9;  accels[4].cmd = IDC_TOGGLE_BP;
    accels[5].fVirt = FVIRTKEY;          accels[5].key = VK_F12; accels[5].cmd = IDCANCEL;
    d->accel = CreateAcceleratorTableA(accels, 6);

    tab = tab_from_env();
    d->page = tab >= 0 ? tab : TAB_CONSOLE;
    SendMessageA(d->tabs, TCM_SETCURSEL, (WPARAM)d->page, 0);
    show_page(d, d->page);
    layout(d);
    load_cart_symbols(d);

    ShowWindow(d->hwnd, SW_SHOW);
    if (d->page == TAB_CONSOLE) SetFocus(d->con_in);
    attach_and_refresh(d);
    env_hooks(d);
}

int a2600_debugger_visible(void)
{
    return g_dbg && IsWindowVisible(g_dbg->hwnd);
}

void a2600_debugger_toggle(HWND parent, a2600session *session)
{
    if (a2600_debugger_visible()) hide_window(g_dbg);
    else a2600_debugger_show(parent, session);
}

int a2600_debugger_pretranslate(MSG *msg)
{
    if (!g_dbg || !g_dbg->accel) return 0;
    if (msg->hwnd != g_dbg->hwnd && !IsChild(g_dbg->hwnd, msg->hwnd)) return 0;
    /* F12 closes without waiting for the accelerator (Windows keeps F12 for
     * debuggers); its key-up goes with it */
    if (msg->message == WM_KEYDOWN && msg->wParam == VK_F12) {
        hide_window(g_dbg);
        return 1;
    }
    return TranslateAcceleratorA(g_dbg->hwnd, g_dbg->accel, msg) ? 1 : 0;
}
