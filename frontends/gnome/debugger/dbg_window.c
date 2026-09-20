/*
 * Debugger window (GTK4/libadwaita) over Stella's own debugger engine, via
 * core/include/a2600debug.h.
 *
 * Tabs mirror what Stella's DebuggerDialog offers: a Prompt (every one of
 * the parser's commands, with tab completion), CPU + zero-page RAM, the
 * disassembly with a breakpoint gutter and bank selector, the TIA (every
 * register, the object positions, the collision matrix, the audio channels
 * and the TIA output picture with the beam position), the RIOT (I/O ports,
 * timer and console switches), breakpoints and traps, and save states.
 * A toolbar carries the stepping controls; the family's keys apply (F5
 * run/stop, F7 step, F8 trace, Shift+F8 frame, F12 close).
 *
 * The window polls the engine's generation counter on a short timer and
 * refreshes only when something changed, so a stopped machine costs
 * nothing and a running one shows live values twice a second.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "dbg_window.h"

#include <stdlib.h>
#include <string.h>

#include "a2600debug.h"
#include "../window.h"

#define DISASM_WINDOW 48
#define FB_W A2600SESSION_FB_WIDTH
#define FB_H A2600SESSION_FB_MAX_HEIGHT

typedef struct {
    GtkWindow *win;
    a2600session *session;
    a2600debug *dbg;
    unsigned seen_generation;
    gboolean was_stopped;
    guint timer;

    GtkLabel *status;
    GtkButton *run_btn;

    /* prompt */
    GtkTextView *prompt_out;
    GtkEntry *prompt_in;
    GtkScrolledWindow *prompt_scroll;

    /* cpu + ram */
    GtkEntry *reg[6];         /* PC SP A X Y PS */
    GtkCheckButton *flag[7];  /* N V B D I Z C */
    GtkLabel *cycles;
    GtkTextView *ram_view;
    GtkEntry *ram_addr, *ram_val;

    /* disassembly */
    GtkTextView *disasm;
    GtkDropDown *bank;
    GtkCheckButton *follow_pc;
    GtkEntry *jump;
    int disasm_first;
    int disasm_total;
    int disasm_bank;    /* -1 = PC's */
    uint16_t line_addr[DISASM_WINDOW];
    int line_count;

    /* tia */
    GtkTextView *tia_text;
    GtkPicture *tia_pic;
    GtkCheckButton *tia_partial;
    GtkEntry *tia_reg, *tia_val;
    guint32 *tia_px;

    /* riot */
    GtkTextView *riot_text;

    /* breaks */
    GtkTextView *bp_view;
    GtkEntry *bp_entry;

    /* cart / states */
    GtkLabel *cart_info;
} DbgWin;

static DbgWin *g_win;

/* ---- helpers -------------------------------------------------------------- */

static void set_text(GtkTextView *view, const char *text)
{
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(view), text, -1);
}

static void append_text(GtkTextView *view, GtkScrolledWindow *scroll, const char *text)
{
    GtkTextBuffer *b = gtk_text_view_get_buffer(view);
    GtkTextIter end;
    GtkAdjustment *adj;
    gtk_text_buffer_get_end_iter(b, &end);
    gtk_text_buffer_insert(b, &end, text, -1);
    adj = gtk_scrolled_window_get_vadjustment(scroll);
    if (adj) gtk_adjustment_set_value(adj, gtk_adjustment_get_upper(adj));
}

/* Stella's prompt colours its output with control bytes; the text view
 * shows what remains. */
static void strip_control(char *s)
{
    char *d = s;
    for (; *s; s++)
        if ((unsigned char)*s >= 0x20 || *s == '\n' || *s == '\t') *d++ = *s;
    *d = '\0';
}

static int parse_num(const char *text, long *out)
{
    char *end;
    long v;
    while (*text == ' ') text++;
    if (*text == '$') v = strtol(text + 1, &end, 16);
    else if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) v = strtol(text + 2, &end, 16);
    else if (*text == '#') v = strtol(text + 1, &end, 10);
    else v = strtol(text, &end, 16);
    if (end == text) return 0;
    *out = v;
    return 1;
}

static GtkWidget *mono_view(GtkTextView **out, gboolean editable)
{
    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkWidget *view = gtk_text_view_new();
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(view), TRUE);
    gtk_text_view_set_editable(GTK_TEXT_VIEW(view), editable);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(view), editable);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(view), 6);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), view);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_widget_set_hexpand(scroll, TRUE);
    *out = GTK_TEXT_VIEW(view);
    return scroll;
}

static GtkWidget *labeled(const char *text, GtkWidget *child)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *l = gtk_label_new(text);
    gtk_widget_add_css_class(l, "dim-label");
    gtk_box_append(GTK_BOX(box), l);
    gtk_box_append(GTK_BOX(box), child);
    return box;
}

/* ---- refresh -------------------------------------------------------------- */

static void refresh_cpu(DbgWin *w)
{
    a2600debug_cpu c;
    char buf[64];
    int i;
    a2600debug_cpu_get(w->dbg, &c);
    g_snprintf(buf, sizeof buf, "%04X", c.pc); gtk_editable_set_text(GTK_EDITABLE(w->reg[0]), buf);
    g_snprintf(buf, sizeof buf, "%02X", c.sp); gtk_editable_set_text(GTK_EDITABLE(w->reg[1]), buf);
    g_snprintf(buf, sizeof buf, "%02X", c.a); gtk_editable_set_text(GTK_EDITABLE(w->reg[2]), buf);
    g_snprintf(buf, sizeof buf, "%02X", c.x); gtk_editable_set_text(GTK_EDITABLE(w->reg[3]), buf);
    g_snprintf(buf, sizeof buf, "%02X", c.y); gtk_editable_set_text(GTK_EDITABLE(w->reg[4]), buf);
    g_snprintf(buf, sizeof buf, "%02X", c.ps); gtk_editable_set_text(GTK_EDITABLE(w->reg[5]), buf);
    {
        const int flags[7] = { c.n, c.v, c.b, c.d, c.i, c.z, c.c };
        for (i = 0; i < 7; i++)
            gtk_check_button_set_active(w->flag[i], flags[i] != 0);
    }
    g_snprintf(buf, sizeof buf, "last instruction: %d cycles, total %llu",
               c.cycles, (unsigned long long)c.total_cycles);
    gtk_label_set_text(w->cycles, buf);
}

static void refresh_ram(DbgWin *w)
{
    uint8_t ram[128];
    char text[2048];
    int len = 0, row, col;
    a2600debug_ram_get(w->dbg, ram);
    len += g_snprintf(text + len, sizeof text - len, "      0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F\n");
    for (row = 0; row < 8; row++) {
        len += g_snprintf(text + len, sizeof text - len, "$%02X: ", 0x80 + row * 16);
        for (col = 0; col < 16; col++)
            len += g_snprintf(text + len, sizeof text - len, "%02X ", ram[row * 16 + col]);
        len += g_snprintf(text + len, sizeof text - len, "\n");
    }
    set_text(w->ram_view, text);
}

static void refresh_disasm(DbgWin *w)
{
    static a2600debug_line lines[DISASM_WINDOW];
    static char text[DISASM_WINDOW * 96];
    int n, total, pc_line, i, len = 0;
    GtkTextBuffer *b = gtk_text_view_get_buffer(w->disasm);
    GtkTextIter s, e;

    /* find the PC first so a following window is centred on it */
    n = a2600debug_disassemble(w->dbg, w->disasm_bank, 0, lines, 1, &total, &pc_line);
    w->disasm_total = total;
    if (gtk_check_button_get_active(w->follow_pc) && pc_line >= 0)
        w->disasm_first = pc_line > DISASM_WINDOW / 3 ? pc_line - DISASM_WINDOW / 3 : 0;
    if (w->disasm_first > total - 1) w->disasm_first = total > 0 ? total - 1 : 0;
    if (w->disasm_first < 0) w->disasm_first = 0;

    n = a2600debug_disassemble(w->dbg, w->disasm_bank, w->disasm_first, lines,
                               DISASM_WINDOW, &total, &pc_line);
    w->line_count = n;
    for (i = 0; i < n; i++) {
        w->line_addr[i] = lines[i].address;
        len += g_snprintf(text + len, sizeof text - len, "%c%c %04X  %-10s %-14s %-22s %s\n",
                          lines[i].has_breakpoint ? '*' : ' ',
                          lines[i].is_pc ? '>' : ' ',
                          lines[i].address, lines[i].bytes,
                          lines[i].label, lines[i].disasm, lines[i].cycles);
    }
    if (n == 0)
        len += g_snprintf(text + len, sizeof text - len, "(no disassembly)\n");
    gtk_text_buffer_set_text(b, text, -1);

    /* the PC line in the accent colour */
    for (i = 0; i < n; i++) {
        if (!lines[i].is_pc) continue;
        gtk_text_buffer_get_iter_at_line(b, &s, i);
        gtk_text_buffer_get_iter_at_line(b, &e, i + 1);
        gtk_text_buffer_apply_tag_by_name(b, "pc", &s, &e);
    }
}

static void refresh_tia(DbgWin *w)
{
    a2600debug_tia t;
    char text[4096];
    int len = 0, h, i;
    static const char *const coll_names[15] = {
        "M0-P1", "M0-P0", "M1-P0", "M1-P1", "P0-PF", "P0-BL", "P1-PF", "P1-BL",
        "M0-PF", "M0-BL", "M1-PF", "M1-BL", "BL-PF", "P0-P1", "M0-M1" };

    a2600debug_tia_get(w->dbg, &t);
    len += g_snprintf(text + len, sizeof text - len,
        "Frame %d   scanline %d (last frame %d)   frame cycles %d (WSYNC %d)\n"
        "clocks this line %d   cycles this line %d   beam %d,%d\n"
        "VSYNC %d   VBLANK %d\n\n",
        t.frame_count, t.scanlines, t.scanlines_last, t.frame_cycles, t.wsync_cycles,
        t.clocks_this_line, t.cycles_this_line, t.beam_x, t.beam_y, t.vsync, t.vblank);
    len += g_snprintf(text + len, sizeof text - len,
        "Colours   COLUP0 %02X   COLUP1 %02X   COLUPF %02X   COLUBK %02X\n"
        "Players   GRP0 %02X  GRP1 %02X   NUSIZ0 %02X  NUSIZ1 %02X   REFP0 %d  REFP1 %d   VDELP0 %d  VDELP1 %d\n"
        "Missiles  ENAM0 %d  ENAM1 %d   RESMP0 %d  RESMP1 %d\n"
        "Ball      ENABL %d   VDELBL %d\n"
        "Playfield PF0 %02X  PF1 %02X  PF2 %02X   CTRLPF %02X   REF %d  SCORE %d  PRIORITY %d\n\n",
        t.colup0, t.colup1, t.colupf, t.colubk,
        t.grp0, t.grp1, t.nusiz0, t.nusiz1, t.refp0, t.refp1, t.vdelp0, t.vdelp1,
        t.enam0, t.enam1, t.resmp0, t.resmp1, t.enabl, t.vdelbl,
        t.pf0, t.pf1, t.pf2, t.ctrlpf, t.refpf, t.scorepf, t.pripf);
    len += g_snprintf(text + len, sizeof text - len,
        "Positions      P0 %3d   P1 %3d   M0 %3d   M1 %3d   BL %3d\n"
        "Motion (HM)    P0 %02X    P1 %02X    M0 %02X    M1 %02X    BL %02X\n\n",
        t.pos_p0, t.pos_p1, t.pos_m0, t.pos_m1, t.pos_bl,
        t.hm_p0, t.hm_p1, t.hm_m0, t.hm_m1, t.hm_bl);
    len += g_snprintf(text + len, sizeof text - len,
        "Audio     AUDC0 %02X  AUDF0 %02X  AUDV0 %02X  (%s)\n"
        "          AUDC1 %02X  AUDF1 %02X  AUDV1 %02X  (%s)\n\nCollisions:",
        t.audc0, t.audf0, t.audv0, t.aud_freq0, t.audc1, t.audf1, t.audv1, t.aud_freq1);
    for (i = 0; i < 15; i++)
        if (t.collisions & (1u << i))
            len += g_snprintf(text + len, sizeof text - len, " %s", coll_names[i]);
    if (!t.collisions) len += g_snprintf(text + len, sizeof text - len, " none");
    len += g_snprintf(text + len, sizeof text - len,
        "\n\nSet a register: name and value below (colup0, pf1, posp0, refp0, "
        "vsync ...); strobes: wsync rsync resp0 resp1 resm0 resm1 resbl hmove hmclr cxclr\n");
    set_text(w->tia_text, text);

    h = a2600debug_frame_snapshot(w->dbg, w->tia_px,
                                  gtk_check_button_get_active(w->tia_partial));
    if (h > 0) {
        GBytes *bytes;
        GdkTexture *tex;
        guint32 *bgra = g_new(guint32, FB_W * h);
        for (i = 0; i < FB_W * h; i++) bgra[i] = w->tia_px[i] | 0xFF000000u;
        bytes = g_bytes_new_take(bgra, (gsize)FB_W * h * 4);
        tex = gdk_memory_texture_new(FB_W, h, GDK_MEMORY_B8G8R8A8, bytes, FB_W * 4);
        gtk_picture_set_paintable(w->tia_pic, GDK_PAINTABLE(tex));
        g_object_unref(tex);
        g_bytes_unref(bytes);
    }
}

static void refresh_riot(DbgWin *w)
{
    a2600debug_riot r;
    char text[1024];
    a2600debug_riot_get(w->dbg, &r);
    g_snprintf(text, sizeof text,
        "SWCHA  %02X   SWACNT %02X     left: %s   right: %s\n"
        "SWCHB  %02X   SWBCNT %02X\n"
        "INPT0-5  %02X %02X %02X %02X %02X %02X\n\n"
        "Timer  INTIM %02X   TIMINT %02X   clocks %d   divider %d\n\n"
        "Switches   Select %s   Reset %s   %s   Left difficulty %s   Right difficulty %s\n\n"
        "The switches can be flipped from the menu, the keypad window or the prompt "
        "(swchb $xx); the ports with joy0up, joy0fire ...",
        r.swcha, r.swacnt, r.dir_left, r.dir_right, r.swchb, r.swbcnt,
        r.inpt[0], r.inpt[1], r.inpt[2], r.inpt[3], r.inpt[4], r.inpt[5],
        r.intim, r.timint, r.tim_clocks, r.tim_divider,
        r.select ? "pressed" : "up", r.reset ? "pressed" : "up",
        r.color ? "Color" : "B&W", r.diff_left_a ? "A" : "B", r.diff_right_a ? "A" : "B");
    set_text(w->riot_text, text);
}

static void refresh_bps(DbgWin *w)
{
    uint32_t bps[64];
    char text[4096], cmd[8192];
    int n = a2600debug_breakpoint_list(w->dbg, bps, 64), i, len = 0;
    len += g_snprintf(text + len, sizeof text - len, "Breakpoints (%d):\n", n);
    for (i = 0; i < n; i++) {
        unsigned bank = bps[i] >> 16;
        if (bank == A2600DEBUG_ANY_BANK)
            len += g_snprintf(text + len, sizeof text - len, "  $%04X  any bank\n", bps[i] & 0xffff);
        else
            len += g_snprintf(text + len, sizeof text - len, "  $%04X  bank %u\n", bps[i] & 0xffff, bank);
    }
    a2600debug_command(w->dbg, "listTraps", cmd, sizeof cmd);
    strip_control(cmd);
    len += g_snprintf(text + len, sizeof text - len, "\nTraps:\n%s\n", cmd);
    a2600debug_command(w->dbg, "listBreaks", cmd, sizeof cmd);
    strip_control(cmd);
    len += g_snprintf(text + len, sizeof text - len, "\nConditional (breakIf):\n%s\n", cmd);
    set_text(w->bp_view, text);
}

static void refresh_status(DbgWin *w)
{
    char reason[160], info[256], text[512];
    int addr;
    gboolean stopped = a2600debug_is_stopped(w->dbg) != 0;
    a2600debug_stop_reason(w->dbg, reason, sizeof reason, &addr);
    a2600debug_cart_info(w->dbg, info, sizeof info);
    if (stopped)
        g_snprintf(text, sizeof text, "Stopped%s%s", reason[0] ? ": " : "", reason);
    else
        g_snprintf(text, sizeof text, "Running");
    gtk_label_set_text(w->status, text);
    gtk_label_set_text(w->cart_info, info);
    gtk_button_set_label(w->run_btn, stopped ? "Run (F5)" : "Stop (F5)");
    if (stopped) gtk_widget_add_css_class(GTK_WIDGET(w->run_btn), "a2600-accent");
    else gtk_widget_remove_css_class(GTK_WIDGET(w->run_btn), "a2600-accent");
}

static void refresh_all(DbgWin *w)
{
    refresh_status(w);
    refresh_cpu(w);
    refresh_ram(w);
    refresh_disasm(w);
    refresh_tia(w);
    refresh_riot(w);
    refresh_bps(w);
}

static gboolean tick_refresh(gpointer ud)
{
    DbgWin *w = ud;
    unsigned gen = a2600debug_generation(w->dbg);
    gboolean stopped = a2600debug_is_stopped(w->dbg) != 0;
    static int running_ticks;
    if (!gtk_widget_get_visible(GTK_WIDGET(w->win))) return G_SOURCE_CONTINUE;
    if (gen != w->seen_generation || stopped != w->was_stopped) {
        w->seen_generation = gen;
        w->was_stopped = stopped;
        refresh_all(w);
    } else if (!stopped && ++running_ticks >= 5) {
        /* live values twice a second while the machine runs */
        running_ticks = 0;
        refresh_status(w);
        refresh_cpu(w);
        refresh_tia(w);
        refresh_riot(w);
    }
    return G_SOURCE_CONTINUE;
}

/* ---- handlers ------------------------------------------------------------- */

static void on_run(GtkButton *b, gpointer ud)
{
    DbgWin *w = ud;
    (void)b;
    if (a2600debug_is_stopped(w->dbg)) a2600debug_resume(w->dbg);
    else a2600debug_stop(w->dbg);
    refresh_all(w);
}

static void on_step(GtkButton *b, gpointer ud) { (void)b; a2600debug_step(((DbgWin *)ud)->dbg); refresh_all(ud); }
static void on_trace(GtkButton *b, gpointer ud) { (void)b; a2600debug_trace(((DbgWin *)ud)->dbg); refresh_all(ud); }
static void on_scanline(GtkButton *b, gpointer ud) { (void)b; a2600debug_scanline(((DbgWin *)ud)->dbg, 1); refresh_all(ud); }
static void on_frame(GtkButton *b, gpointer ud) { (void)b; a2600debug_frame(((DbgWin *)ud)->dbg, 1); refresh_all(ud); }
static void on_rewind(GtkButton *b, gpointer ud) { (void)b; a2600debug_rewind(((DbgWin *)ud)->dbg, 1); refresh_all(ud); }
static void on_unwind(GtkButton *b, gpointer ud) { (void)b; a2600debug_unwind(((DbgWin *)ud)->dbg, 1); refresh_all(ud); }

static void on_prompt(GtkEntry *entry, gpointer ud)
{
    DbgWin *w = ud;
    static char out[65536];
    const char *cmd = gtk_editable_get_text(GTK_EDITABLE(entry));
    char line[512];
    if (!cmd || !*cmd) return;
    g_snprintf(line, sizeof line, "> %s\n", cmd);
    append_text(w->prompt_out, w->prompt_scroll, line);
    a2600debug_command(w->dbg, cmd, out, sizeof out);
    strip_control(out);
    append_text(w->prompt_out, w->prompt_scroll, out);
    append_text(w->prompt_out, w->prompt_scroll, "\n");
    gtk_editable_set_text(GTK_EDITABLE(entry), "");
    refresh_all(w);
}

/* Tab completes the current word against the parser's and the debugger's
 * completions; several matches are listed instead. */
static gboolean on_prompt_key(GtkEventControllerKey *c, guint keyval, guint code,
                              GdkModifierType st, gpointer ud)
{
    DbgWin *w = ud;
    char comps[4096];
    const char *text;
    const char *word;
    int n;
    (void)c; (void)code; (void)st;
    if (keyval != GDK_KEY_Tab) return FALSE;
    text = gtk_editable_get_text(GTK_EDITABLE(w->prompt_in));
    word = strrchr(text, ' ');
    word = word ? word + 1 : text;
    n = a2600debug_completions(w->dbg, word, comps, sizeof comps);
    if (n == 1) {
        char merged[600];
        char *nl = strchr(comps, '\n');
        if (nl) *nl = '\0';
        g_snprintf(merged, sizeof merged, "%.*s%s ", (int)(word - text), text, comps);
        gtk_editable_set_text(GTK_EDITABLE(w->prompt_in), merged);
        gtk_editable_set_position(GTK_EDITABLE(w->prompt_in), -1);
    } else if (n > 1) {
        append_text(w->prompt_out, w->prompt_scroll, comps);
    }
    return TRUE;
}

static void on_reg_activate(GtkEntry *entry, gpointer ud)
{
    DbgWin *w = ud;
    int i = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(entry), "reg"));
    long v;
    static const int regs[6] = { A2600_REG_PC, A2600_REG_SP, A2600_REG_A, A2600_REG_X, A2600_REG_Y, A2600_REG_PS };
    if (parse_num(gtk_editable_get_text(GTK_EDITABLE(entry)), &v))
        a2600debug_cpu_set(w->dbg, regs[i], (int)v);
    refresh_all(w);
}

static void on_flag_toggled(GtkCheckButton *b, gpointer ud)
{
    DbgWin *w = ud;
    int i = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "flag"));
    static const int flags[7] = { A2600_FLAG_N, A2600_FLAG_V, A2600_FLAG_B, A2600_FLAG_D, A2600_FLAG_I, A2600_FLAG_Z, A2600_FLAG_C };
    if (!a2600debug_is_stopped(w->dbg)) return;
    a2600debug_cpu_set(w->dbg, flags[i], gtk_check_button_get_active(b));
}

static void on_ram_write(GtkEntry *entry, gpointer ud)
{
    DbgWin *w = ud;
    long a, v;
    (void)entry;
    if (parse_num(gtk_editable_get_text(GTK_EDITABLE(w->ram_addr)), &a)
        && parse_num(gtk_editable_get_text(GTK_EDITABLE(w->ram_val)), &v))
        a2600debug_write(w->dbg, (uint16_t)a, (uint8_t)v);
    refresh_all(w);
}

static void on_disasm_click(GtkGestureClick *g, int n, double x, double y, gpointer ud)
{
    DbgWin *w = ud;
    GtkTextIter it;
    int bx, by, line, bank;
    (void)g; (void)n;
    gtk_text_view_window_to_buffer_coords(w->disasm, GTK_TEXT_WINDOW_WIDGET, (int)x, (int)y, &bx, &by);
    gtk_text_view_get_iter_at_location(w->disasm, &it, bx, by);
    line = gtk_text_iter_get_line(&it);
    if (line < 0 || line >= w->line_count) return;
    bank = w->disasm_bank < 0 ? a2600debug_current_bank(w->dbg) : w->disasm_bank;
    a2600debug_breakpoint_toggle(w->dbg, w->line_addr[line], bank);
    refresh_disasm(w);
    refresh_bps(w);
}

static void on_bank_changed(GObject *dd, GParamSpec *ps, gpointer ud)
{
    DbgWin *w = ud;
    int sel = (int)gtk_drop_down_get_selected(GTK_DROP_DOWN(dd));
    (void)ps;
    w->disasm_bank = sel - 1;   /* item 0 = the PC's bank */
    if (sel > 0) gtk_check_button_set_active(w->follow_pc, FALSE);
    w->disasm_first = 0;
    refresh_disasm(w);
}

static void on_jump(GtkEntry *entry, gpointer ud)
{
    DbgWin *w = ud;
    const char *text = gtk_editable_get_text(GTK_EDITABLE(entry));
    long a;
    int addr = a2600debug_label_address(w->dbg, text);
    if (addr < 0 && parse_num(text, &a)) addr = (int)a;
    if (addr < 0) return;
    /* the listing's lines are sorted by address: walk to it */
    {
        static a2600debug_line lines[256];
        int total, pc_line, first = 0, n, i;
        gtk_check_button_set_active(w->follow_pc, FALSE);
        for (;;) {
            n = a2600debug_disassemble(w->dbg, w->disasm_bank, first, lines, 256, &total, &pc_line);
            if (n == 0) break;
            for (i = 0; i < n; i++)
                if (lines[i].address >= addr) { w->disasm_first = first + i; refresh_disasm(w); return; }
            first += n;
            if (first >= total) break;
        }
    }
}

static void on_disasm_scroll(GtkEventControllerScroll *c, double dx, double dy, gpointer ud)
{
    DbgWin *w = ud;
    (void)c; (void)dx;
    gtk_check_button_set_active(w->follow_pc, FALSE);
    w->disasm_first += (int)(dy * 3);
    refresh_disasm(w);
}

static void on_tia_set(GtkEntry *entry, gpointer ud)
{
    DbgWin *w = ud;
    const char *reg = gtk_editable_get_text(GTK_EDITABLE(w->tia_reg));
    long v;
    (void)entry;
    if (a2600debug_tia_strobe(w->dbg, reg) == 0) { refresh_all(w); return; }
    if (parse_num(gtk_editable_get_text(GTK_EDITABLE(w->tia_val)), &v))
        a2600debug_tia_set(w->dbg, reg, (int)v);
    refresh_all(w);
}

static void on_bp_add(GtkEntry *entry, gpointer ud)
{
    DbgWin *w = ud;
    const char *text = gtk_editable_get_text(GTK_EDITABLE(entry));
    char cmd[300], out[2048];
    if (!text || !*text) return;
    /* an address or label toggles a breakpoint; anything else is a parser
     * command (breakIf {a==$ff}, trapWrite $80 ...) */
    {
        long a; int addr = a2600debug_label_address(w->dbg, text);
        if (addr < 0 && parse_num(text, &a) && strchr("$#0123456789abcdefABCDEF", text[0])) addr = (int)a;
        if (addr >= 0) {
            a2600debug_breakpoint_toggle(w->dbg, (uint16_t)addr, A2600DEBUG_ANY_BANK);
        } else {
            g_snprintf(cmd, sizeof cmd, "%s", text);
            a2600debug_command(w->dbg, cmd, out, sizeof out);
        }
    }
    gtk_editable_set_text(GTK_EDITABLE(entry), "");
    refresh_all(w);
}

static void on_bp_clear(GtkButton *b, gpointer ud)
{
    DbgWin *w = ud;
    char out[512];
    (void)b;
    a2600debug_breakpoint_clear(w->dbg);
    a2600debug_command(w->dbg, "clearTraps", out, sizeof out);
    refresh_all(w);
}

static void on_state_save(GtkButton *b, gpointer ud)
{
    DbgWin *w = ud;
    a2600debug_state_save(w->dbg, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "slot")));
    refresh_all(w);
}

static void on_state_load(GtkButton *b, gpointer ud)
{
    DbgWin *w = ud;
    a2600debug_state_load(w->dbg, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "slot")));
    refresh_all(w);
}

static void on_symbols(GtkButton *b, gpointer ud)
{
    DbgWin *w = ud;
    char msg[512], line[600];
    (void)b;
    a2600debug_load_symbols(w->dbg, msg, sizeof msg);
    g_snprintf(line, sizeof line, "%s\n", msg);
    append_text(w->prompt_out, w->prompt_scroll, line);
    refresh_all(w);
}

static void on_save_chosen(GObject *src, GAsyncResult *res, gpointer ud)
{
    DbgWin *w = ud;
    g_autoptr(GFile) file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(src), res, NULL);
    g_autofree char *path = NULL;
    const char *kind = g_object_get_data(src, "kind");
    char msg[512], line[600];
    if (!file) return;
    path = g_file_get_path(file);
    if (!path) return;
    a2600debug_save(w->dbg, kind, path, msg, sizeof msg);
    strip_control(msg);
    g_snprintf(line, sizeof line, "%s\n", msg);
    append_text(w->prompt_out, w->prompt_scroll, line);
}

static void on_save(GtkButton *b, gpointer ud)
{
    DbgWin *w = ud;
    GtkFileDialog *dlg = gtk_file_dialog_new();
    const char *kind = g_object_get_data(G_OBJECT(b), "kind");
    char title[64];
    g_snprintf(title, sizeof title, "Save %s", (const char *)g_object_get_data(G_OBJECT(b), "title"));
    gtk_file_dialog_set_title(dlg, title);
    g_object_set_data(G_OBJECT(dlg), "kind", (gpointer)kind);
    gtk_file_dialog_save(dlg, w->win, NULL, on_save_chosen, w);
    g_object_unref(dlg);
}

static gboolean on_key(GtkEventControllerKey *c, guint keyval, guint code,
                       GdkModifierType st, gpointer ud)
{
    DbgWin *w = ud;
    (void)c; (void)code;
    switch (keyval) {
    case GDK_KEY_F5: on_run(NULL, w); return TRUE;
    case GDK_KEY_F7: on_step(NULL, w); return TRUE;
    case GDK_KEY_F8:
        if (st & GDK_SHIFT_MASK) on_frame(NULL, w); else on_trace(NULL, w);
        return TRUE;
    case GDK_KEY_F12: gtk_widget_set_visible(GTK_WIDGET(w->win), FALSE); return TRUE;
    default: return FALSE;
    }
}

static gboolean on_close(GtkWindow *win, gpointer ud)
{
    (void)ud;
    gtk_widget_set_visible(GTK_WIDGET(win), FALSE);
    return TRUE;
}

/* ---- construction --------------------------------------------------------- */

static GtkWidget *build_toolbar(DbgWin *w)
{
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *b;
    gtk_widget_set_margin_start(bar, 8);
    gtk_widget_set_margin_end(bar, 8);
    gtk_widget_set_margin_top(bar, 6);
    gtk_widget_set_margin_bottom(bar, 6);
#define TB(label, cb) do { b = gtk_button_new_with_label(label); \
    g_signal_connect(b, "clicked", G_CALLBACK(cb), w); gtk_box_append(GTK_BOX(bar), b); } while (0)
    w->run_btn = GTK_BUTTON(gtk_button_new_with_label("Stop (F5)"));
    g_signal_connect(w->run_btn, "clicked", G_CALLBACK(on_run), w);
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(w->run_btn));
    TB("Step (F7)", on_step);
    TB("Trace (F8)", on_trace);
    TB("Scan+1", on_scanline);
    TB("Frame+1 (\xe2\x87\xa7""F8)", on_frame);
    TB("Rewind", on_rewind);
    TB("Unwind", on_unwind);
#undef TB
    w->status = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(w->status), "dim-label");
    gtk_widget_set_hexpand(GTK_WIDGET(w->status), TRUE);
    gtk_label_set_xalign(w->status, 1.0);
    gtk_label_set_ellipsize(w->status, PANGO_ELLIPSIZE_START);
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(w->status));
    return bar;
}

static GtkWidget *build_prompt(DbgWin *w)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *scroll = mono_view(&w->prompt_out, FALSE);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *b;
    GtkEventController *keys;
    w->prompt_scroll = GTK_SCROLLED_WINDOW(scroll);
    gtk_text_view_set_wrap_mode(w->prompt_out, GTK_WRAP_WORD_CHAR);
    w->prompt_in = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(w->prompt_in, "Stella debugger command (help, break, breakIf, trap, watch, frame, tia, ...) \xe2\x80\x94 Tab completes");
    gtk_widget_set_hexpand(GTK_WIDGET(w->prompt_in), TRUE);
    g_signal_connect(w->prompt_in, "activate", G_CALLBACK(on_prompt), w);
    keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_prompt_key), w);
    gtk_widget_add_controller(GTK_WIDGET(w->prompt_in), keys);
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(w->prompt_in));
    b = gtk_button_new_with_label("Load symbols");
    g_signal_connect(b, "clicked", G_CALLBACK(on_symbols), w);
    gtk_box_append(GTK_BOX(row), b);
    gtk_box_append(GTK_BOX(box), scroll);
    gtk_box_append(GTK_BOX(box), row);
    set_text(w->prompt_out, "Stella debugger prompt. Type 'help' for every command.\n");
    return box;
}

static GtkWidget *build_cpu(DbgWin *w)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *regs = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *flags = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *ram = mono_view(&w->ram_view, FALSE);
    GtkWidget *edit = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    static const char *const names[6] = { "PC", "SP", "A", "X", "Y", "PS" };
    static const char *const fnames[7] = { "N", "V", "B", "D", "I", "Z", "C" };
    int i;
    for (i = 0; i < 6; i++) {
        w->reg[i] = GTK_ENTRY(gtk_entry_new());
        gtk_entry_set_max_length(w->reg[i], 4);
        gtk_editable_set_width_chars(GTK_EDITABLE(w->reg[i]), 5);
        g_object_set_data(G_OBJECT(w->reg[i]), "reg", GINT_TO_POINTER(i));
        g_signal_connect(w->reg[i], "activate", G_CALLBACK(on_reg_activate), w);
        gtk_box_append(GTK_BOX(regs), labeled(names[i], GTK_WIDGET(w->reg[i])));
    }
    for (i = 0; i < 7; i++) {
        w->flag[i] = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(fnames[i]));
        g_object_set_data(G_OBJECT(w->flag[i]), "flag", GINT_TO_POINTER(i));
        g_signal_connect(w->flag[i], "toggled", G_CALLBACK(on_flag_toggled), w);
        gtk_box_append(GTK_BOX(flags), GTK_WIDGET(w->flag[i]));
    }
    w->cycles = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(w->cycles), "dim-label");
    gtk_box_append(GTK_BOX(flags), GTK_WIDGET(w->cycles));

    w->ram_addr = GTK_ENTRY(gtk_entry_new());
    gtk_editable_set_width_chars(GTK_EDITABLE(w->ram_addr), 6);
    gtk_entry_set_placeholder_text(w->ram_addr, "$80");
    w->ram_val = GTK_ENTRY(gtk_entry_new());
    gtk_editable_set_width_chars(GTK_EDITABLE(w->ram_val), 4);
    gtk_entry_set_placeholder_text(w->ram_val, "$00");
    g_signal_connect(w->ram_val, "activate", G_CALLBACK(on_ram_write), w);
    gtk_box_append(GTK_BOX(edit), labeled("Write address", GTK_WIDGET(w->ram_addr)));
    gtk_box_append(GTK_BOX(edit), labeled("value", GTK_WIDGET(w->ram_val)));

    gtk_box_append(GTK_BOX(box), regs);
    gtk_box_append(GTK_BOX(box), flags);
    gtk_box_append(GTK_BOX(box), gtk_label_new("Zero-page RAM ($80-$FF)"));
    gtk_box_append(GTK_BOX(box), ram);
    gtk_box_append(GTK_BOX(box), edit);
    return box;
}

static GtkWidget *build_disasm(DbgWin *w)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *scroll = mono_view(&w->disasm, FALSE);
    GtkStringList *banks = gtk_string_list_new(NULL);
    GtkGesture *click = gtk_gesture_click_new();
    GtkEventController *scrollc = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
    GtkTextBuffer *b = gtk_text_view_get_buffer(w->disasm);
    char rgb[16];
    int i, n = a2600debug_bank_count(w->dbg);

    g_snprintf(rgb, sizeof rgb, "#%06x", A2600SESSION_ACCENT_RGB);
    gtk_text_buffer_create_tag(b, "pc", "background", rgb, "foreground", "#000000", NULL);

    gtk_string_list_append(banks, "PC's bank");
    for (i = 0; i < n; i++) {
        char s[16];
        g_snprintf(s, sizeof s, "Bank %d", i);
        gtk_string_list_append(banks, s);
    }
    w->bank = GTK_DROP_DOWN(gtk_drop_down_new(G_LIST_MODEL(banks), NULL));
    g_signal_connect(w->bank, "notify::selected", G_CALLBACK(on_bank_changed), w);
    w->follow_pc = GTK_CHECK_BUTTON(gtk_check_button_new_with_label("Follow PC"));
    gtk_check_button_set_active(w->follow_pc, TRUE);
    w->jump = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(w->jump, "address or label");
    gtk_editable_set_width_chars(GTK_EDITABLE(w->jump), 14);
    g_signal_connect(w->jump, "activate", G_CALLBACK(on_jump), w);
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(w->bank));
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(w->follow_pc));
    gtk_box_append(GTK_BOX(row), labeled("Jump to", GTK_WIDGET(w->jump)));
    gtk_box_append(GTK_BOX(row), gtk_label_new("Click a line to toggle its breakpoint; scroll to browse"));

    g_signal_connect(click, "pressed", G_CALLBACK(on_disasm_click), w);
    gtk_widget_add_controller(GTK_WIDGET(w->disasm), GTK_EVENT_CONTROLLER(click));
    g_signal_connect(scrollc, "scroll", G_CALLBACK(on_disasm_scroll), w);
    gtk_widget_add_controller(GTK_WIDGET(w->disasm), scrollc);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);

    gtk_box_append(GTK_BOX(box), row);
    gtk_box_append(GTK_BOX(box), scroll);
    w->disasm_bank = -1;
    return box;
}

static GtkWidget *build_tia(DbgWin *w)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *text = mono_view(&w->tia_text, FALSE);
    GtkWidget *edit = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

    gtk_text_view_set_wrap_mode(w->tia_text, GTK_WRAP_WORD_CHAR);
    w->tia_reg = GTK_ENTRY(gtk_entry_new());
    gtk_editable_set_width_chars(GTK_EDITABLE(w->tia_reg), 8);
    gtk_entry_set_placeholder_text(w->tia_reg, "colup0");
    w->tia_val = GTK_ENTRY(gtk_entry_new());
    gtk_editable_set_width_chars(GTK_EDITABLE(w->tia_val), 5);
    gtk_entry_set_placeholder_text(w->tia_val, "$1E");
    g_signal_connect(w->tia_val, "activate", G_CALLBACK(on_tia_set), w);
    g_signal_connect(w->tia_reg, "activate", G_CALLBACK(on_tia_set), w);
    gtk_box_append(GTK_BOX(edit), labeled("Register / strobe", GTK_WIDGET(w->tia_reg)));
    gtk_box_append(GTK_BOX(edit), labeled("value", GTK_WIDGET(w->tia_val)));
    gtk_box_append(GTK_BOX(left), text);
    gtk_box_append(GTK_BOX(left), edit);
    gtk_widget_set_hexpand(left, TRUE);

    w->tia_pic = GTK_PICTURE(gtk_picture_new());
    gtk_picture_set_content_fit(w->tia_pic, GTK_CONTENT_FIT_FILL);
    gtk_widget_set_size_request(GTK_WIDGET(w->tia_pic), 320, 240);
    gtk_widget_set_vexpand(GTK_WIDGET(w->tia_pic), TRUE);
    w->tia_partial = GTK_CHECK_BUTTON(gtk_check_button_new_with_label("Frame in progress (to the beam)"));
    gtk_box_append(GTK_BOX(right), GTK_WIDGET(w->tia_pic));
    gtk_box_append(GTK_BOX(right), GTK_WIDGET(w->tia_partial));

    gtk_box_append(GTK_BOX(box), left);
    gtk_box_append(GTK_BOX(box), right);
    w->tia_px = g_new0(guint32, FB_W * FB_H);
    return box;
}

static GtkWidget *build_riot(DbgWin *w)
{
    GtkWidget *text = mono_view(&w->riot_text, FALSE);
    gtk_text_view_set_wrap_mode(w->riot_text, GTK_WRAP_WORD_CHAR);
    return text;
}

static GtkWidget *build_breaks(DbgWin *w)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *view = mono_view(&w->bp_view, FALSE);
    GtkWidget *b;
    w->bp_entry = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(w->bp_entry, "address/label to toggle, or breakIf {..}, trap $80, trapWrite $80 $ff, watch a ...");
    gtk_widget_set_hexpand(GTK_WIDGET(w->bp_entry), TRUE);
    g_signal_connect(w->bp_entry, "activate", G_CALLBACK(on_bp_add), w);
    b = gtk_button_new_with_label("Clear all");
    g_signal_connect(b, "clicked", G_CALLBACK(on_bp_clear), w);
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(w->bp_entry));
    gtk_box_append(GTK_BOX(row), b);
    gtk_box_append(GTK_BOX(box), row);
    gtk_box_append(GTK_BOX(box), view);
    return box;
}

static GtkWidget *build_states(DbgWin *w)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *grid = gtk_grid_new();
    GtkWidget *files = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    int i;
    static const struct { const char *kind, *title; } saves[] = {
        { "dis", "disassembly" }, { "rom", "ROM (patched)" }, { "access", "access counters" },
        { "ses", "session" }, { "snap", "TIA snapshot" } };
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 6);
    for (i = 0; i < 10; i++) {
        char s[16];
        GtkWidget *sb, *lb;
        g_snprintf(s, sizeof s, "Save %d", i);
        sb = gtk_button_new_with_label(s);
        g_object_set_data(G_OBJECT(sb), "slot", GINT_TO_POINTER(i));
        g_signal_connect(sb, "clicked", G_CALLBACK(on_state_save), w);
        g_snprintf(s, sizeof s, "Load %d", i);
        lb = gtk_button_new_with_label(s);
        g_object_set_data(G_OBJECT(lb), "slot", GINT_TO_POINTER(i));
        g_signal_connect(lb, "clicked", G_CALLBACK(on_state_load), w);
        gtk_grid_attach(GTK_GRID(grid), sb, i % 5, (i / 5) * 2, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), lb, i % 5, (i / 5) * 2 + 1, 1, 1);
    }
    for (i = 0; i < 5; i++) {
        GtkWidget *b = gtk_button_new_with_label(saves[i].title);
        g_object_set_data(G_OBJECT(b), "kind", (gpointer)saves[i].kind);
        g_object_set_data(G_OBJECT(b), "title", (gpointer)saves[i].title);
        g_signal_connect(b, "clicked", G_CALLBACK(on_save), w);
        gtk_box_append(GTK_BOX(files), b);
    }
    gtk_box_append(GTK_BOX(box), gtk_label_new("Emulator states (Stella's saveState / loadState slots)"));
    gtk_box_append(GTK_BOX(box), grid);
    gtk_box_append(GTK_BOX(box), gtk_label_new("Save to a file"));
    gtk_box_append(GTK_BOX(box), files);
    w->cart_info = GTK_LABEL(gtk_label_new(""));
    gtk_label_set_wrap(w->cart_info, TRUE);
    gtk_widget_add_css_class(GTK_WIDGET(w->cart_info), "dim-label");
    gtk_box_append(GTK_BOX(box), gtk_label_new("Cartridge"));
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(w->cart_info));
    return box;
}

static void on_destroy(GtkWidget *widget, gpointer ud)
{
    DbgWin *w = ud;
    (void)widget;
    if (w->timer) g_source_remove(w->timer);
    g_free(w->tia_px);
    if (g_win == w) g_win = NULL;
    g_free(w);
}

void a2600_debugger_show(GtkWindow *parent, a2600session *session)
{
    DbgWin *w;
    GtkWidget *toolbar, *header, *root, *notebook;
    GtkEventController *keys;

    if (g_win) {
        gtk_window_present(g_win->win);
        a2600debug_stop(g_win->dbg);
        refresh_all(g_win);
        return;
    }
    w = g_new0(DbgWin, 1);
    w->session = session;
    w->dbg = a2600session_debugger(session);
    g_win = w;

    w->win = GTK_WINDOW(adw_window_new());
    gtk_window_set_title(w->win, "Debugger");
    gtk_window_set_default_size(w->win, 1100, 760);
    gtk_window_set_transient_for(w->win, parent);
    gtk_window_set_destroy_with_parent(w->win, TRUE);
    g_signal_connect(w->win, "close-request", G_CALLBACK(on_close), w);
    g_signal_connect(w->win, "destroy", G_CALLBACK(on_destroy), w);

    root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(root), build_toolbar(w));
    notebook = gtk_notebook_new();
    gtk_widget_set_vexpand(notebook, TRUE);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), build_prompt(w), gtk_label_new("Prompt"));
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), build_cpu(w), gtk_label_new("CPU & RAM"));
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), build_disasm(w), gtk_label_new("Disassembly"));
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), build_tia(w), gtk_label_new("TIA"));
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), build_riot(w), gtk_label_new("I/O"));
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), build_breaks(w), gtk_label_new("Breaks & Traps"));
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), build_states(w), gtk_label_new("States & Cart"));
    gtk_box_append(GTK_BOX(root), notebook);
    {
        const char *tab = g_getenv("A2600_DEBUGGER_TAB");
        if (tab && *tab) gtk_notebook_set_current_page(GTK_NOTEBOOK(notebook), atoi(tab));
    }

    header = adw_header_bar_new();
    toolbar = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), root);
    adw_window_set_content(ADW_WINDOW(w->win), toolbar);

    keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key), w);
    gtk_widget_add_controller(GTK_WIDGET(w->win), keys);

    a2600debug_stop(w->dbg);
    refresh_all(w);
    w->seen_generation = a2600debug_generation(w->dbg);
    w->was_stopped = TRUE;
    w->timer = g_timeout_add(100, tick_refresh, w);
    gtk_window_present(w->win);
}
