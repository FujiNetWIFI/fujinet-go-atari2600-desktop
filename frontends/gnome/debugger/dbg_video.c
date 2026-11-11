/*
 * The debugger's Video tab (GTK4/cairo).
 *
 * Everything that decides what to show lives in the core (a2600vidtab.h): this file is
 * widgets. The picture is drawn from the controller's cached buffer (0xFFRRGGBB, drawn as
 * CAIRO_FORMAT_RGB24 with a nearest filter), so a mouse move redraws only the marks. Only the
 * section showing is filled in.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "dbg_video.h"

#include <stdlib.h>
#include <string.h>

#include "a2600debug.h"
#include "a2600vidtab.h"

#define NSWATCH A2600VID_O_COUNT   /* the objects that can draw a pixel */
#define MAX_OVERLAYS 16

typedef struct {
    a2600session *session;
    a2600vid_tab *tab;
    GtkDropDown *view, *source, *zoom;
    GtkCheckButton *ov[MAX_OVERLAYS];
    int nov;
    GtkLabel *status, *note;
    GtkDrawingArea *pic;
    GtkWidget *pic_scroll;
    int vp_w, vp_h;
    GtkNotebook *sections;
    GtkLabel *pixel_text, *pixel_note;
    GtkTextView *regs_text, *lines_text, *writes_text;
    char *regs_sig, *lines_sig, *writes_sig;
    GtkEntry *tia_reg, *tia_val;
    GtkDrawingArea *swatch_box[NSWATCH];
    GtkLabel *swatch_lbl[NSWATCH];
    GtkLabel *timing_text;
    GtkProgressBar *beam;
    gboolean updating, primed, was_mapped;
} Vid;

static GtkWidget *mono_label(void)
{
    GtkWidget *l = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_selectable(GTK_LABEL(l), TRUE);
    gtk_widget_add_css_class(l, "monospace");
    return l;
}

static GtkWidget *padded_box(void)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_start(box, 8);
    gtk_widget_set_margin_end(box, 8);
    gtk_widget_set_margin_top(box, 8);
    gtk_widget_set_margin_bottom(box, 8);
    return box;
}

static void set_rgb(cairo_t *cr, uint32_t rgb)
{
    cairo_set_source_rgb(cr, ((rgb >> 16) & 255) / 255.0, ((rgb >> 8) & 255) / 255.0, (rgb & 255) / 255.0);
}

/* ---- the picture ---------------------------------------------------------- */

static void draw_message(cairo_t *cr, int w, int h, const char *msg)
{
    cairo_text_extents_t ext;
    cairo_set_source_rgb(cr, 0.65, 0.65, 0.68);
    cairo_select_font_face(cr, "sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 14);
    cairo_text_extents(cr, msg, &ext);
    cairo_move_to(cr, (w - ext.width) / 2 - ext.x_bearing, (h - ext.height) / 2 - ext.y_bearing);
    cairo_show_text(cr, msg);
}

/* A pixel can be one device pixel wide: outline it from outside, dark under light. */
static void draw_mark(cairo_t *cr, const int r[4], double red, double green, double blue)
{
    cairo_set_line_width(cr, 3);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.85);
    cairo_rectangle(cr, r[0] - 1.5, r[1] - 1.5, r[2] + 3, r[3] + 3);
    cairo_stroke(cr);
    cairo_set_line_width(cr, 1);
    cairo_set_source_rgb(cr, red, green, blue);
    cairo_rectangle(cr, r[0] - 1.5, r[1] - 1.5, r[2] + 3, r[3] + 3);
    cairo_stroke(cr);
}

static void draw_pic(GtkDrawingArea *area, cairo_t *cr, int w, int h, gpointer ud)
{
    Vid *v = ud;
    int pw, ph, marks, hover[4] = { 0 }, pin[4] = { 0 };
    const uint32_t *px;
    (void)area;
    cairo_set_source_rgb(cr, 0.07, 0.07, 0.09);
    cairo_paint(cr);
    px = a2600vid_tab_picture(v->tab, &pw, &ph);
    if (!px) {
        const char *why = a2600vid_tab_why(v->tab);
        draw_message(cr, w, h, why && *why ? why : "Nothing to show");
        return;
    }
    {
        cairo_surface_t *s = cairo_image_surface_create_for_data((unsigned char *)px, CAIRO_FORMAT_RGB24,
                                                                 pw, ph, pw * 4);
        cairo_set_source_surface(cr, s, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
        cairo_paint(cr);
        cairo_surface_destroy(s);
    }
    marks = a2600vid_tab_marks(v->tab, hover, pin);
    if (marks & 1) draw_mark(cr, hover, 1, 1, 1);
    if (marks & 2) draw_mark(cr, pin, 1.0, 0.85, 0.1);
}

static void update_pixel_section(Vid *v)
{
    const a2600vid_pixel *p = a2600vid_tab_pixel(v->tab);
    gtk_label_set_text(v->pixel_text, p ? p->text
        : "Move over the picture to inspect a pixel.\nClick to pin it (click again to unpin);\n"
          "double-click to break on writes to it.");
    {   /* the swatch the inspected pixel uses is outlined */
        int index;
        for (index = 0; index < NSWATCH; index++)
            gtk_widget_queue_draw(GTK_WIDGET(v->swatch_box[index]));
    }
}

static void do_break_pixel(Vid *v)
{
    const a2600vid_pixel *p = a2600vid_tab_pixel(v->tab);
    int id = a2600vid_tab_break_on_pixel(v->tab);
    char buf[160];
    if (id >= 0 && p)
        g_snprintf(buf, sizeof buf, "Break on writes to the registers behind the pixel: "
                   "its colour, its graphics and its position strobe");
    else
        g_snprintf(buf, sizeof buf, "Nothing to break on (inspect a pixel of a row the TIA drew)");
    gtk_label_set_text(v->note, buf);
    gtk_label_set_text(v->pixel_note, buf);
}

static void on_motion(GtkEventControllerMotion *c, double x, double y, gpointer ud)
{
    Vid *v = ud;
    (void)c;
    if (a2600vid_tab_hover(v->tab, (int)x, (int)y)) {
        gtk_widget_queue_draw(GTK_WIDGET(v->pic));
        update_pixel_section(v);
    }
}

static void on_leave(GtkEventControllerMotion *c, gpointer ud)
{
    Vid *v = ud;
    (void)c;
    if (a2600vid_tab_hover(v->tab, -1, -1)) {
        gtk_widget_queue_draw(GTK_WIDGET(v->pic));
        update_pixel_section(v);
    }
}

static void on_click(GtkGestureClick *g, int n, double x, double y, gpointer ud)
{
    Vid *v = ud;
    (void)g;
    if (n == 1) {
        a2600vid_tab_pin(v->tab, (int)x, (int)y);
    } else if (n == 2) {
        /* the first click of the pair toggled the pin; the second restores it */
        a2600vid_tab_hover(v->tab, (int)x, (int)y);
        a2600vid_tab_pin(v->tab, (int)x, (int)y);
        do_break_pixel(v);
    }
    gtk_widget_queue_draw(GTK_WIDGET(v->pic));
    update_pixel_section(v);
}

/* ---- text tables ------------------------------------------------------------- */

/* Replace a text view's contents when they (or the highlighted rows) changed;
 * the scroll position stays put. hl[i] != 0 highlights line i. */
static void put_table(GtkTextView *tv, char **sigp, GString *text, const GString *hl)
{
    GString *sig = g_string_new(NULL);
    g_string_append_len(sig, hl->str, (gssize)hl->len);
    g_string_append_c(sig, '|');
    g_string_append_len(sig, text->str, (gssize)text->len);
    if (!*sigp || strcmp(*sigp, sig->str)) {
        GtkTextBuffer *b = gtk_text_view_get_buffer(tv);
        GtkAdjustment *adj = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(tv));
        double pos = adj ? gtk_adjustment_get_value(adj) : 0;
        char *clean = g_utf8_make_valid(text->str, -1);
        gsize i;
        gtk_text_buffer_set_text(b, clean, -1);
        g_free(clean);
        for (i = 0; i < hl->len; i++) {
            if (hl->str[i] == '1') {
                GtkTextIter a, e;
                gtk_text_buffer_get_iter_at_line(b, &a, (int)i);
                e = a;
                gtk_text_iter_forward_to_line_end(&e);
                gtk_text_buffer_apply_tag_by_name(b, "changed", &a, &e);
            }
        }
        if (adj) gtk_adjustment_set_value(adj, pos);
        g_free(*sigp);
        *sigp = g_strdup(sig->str);
    }
    g_string_free(sig, TRUE);
}

static void fill_regs(Vid *v)
{
    const a2600vid_reg *r;
    int n = a2600vid_tab_regs(v->tab, &r), i;
    GString *s = g_string_new(NULL), *hl = g_string_new(NULL);
    g_string_append(s, "Register   Value  Meaning\n");
    g_string_append_c(hl, '0');
    for (i = 0; i < n; i++) {
        g_string_append_printf(s, "%-10s %-6s %s\n", r[i].name, r[i].value, r[i].meaning);
        g_string_append_c(hl, r[i].age < A2600VID_REG_FADE ? '1' : '0');
    }
    if (!n) g_string_append(s, "The machine is not running.\n");
    put_table(v->regs_text, &v->regs_sig, s, hl);
    g_string_free(s, TRUE);
    g_string_free(hl, TRUE);
}

static void fill_lines(Vid *v)
{
    const a2600vid_line_row *r;
    int n = a2600vid_tab_lines(v->tab, &r), i;
    GString *s = g_string_new(NULL), *hl = g_string_new(NULL);
    g_string_append(s, "Row Line CP0 CP1 CPF CBK PF0 PF1 PF2 CTL GP0 GP1 NZ0 NZ1 En P0  P1  M0  M1  BL  Wr Notes\n");
    g_string_append_c(hl, '0');
    for (i = 0; i < n; i++) {
        g_string_append_printf(s, "%3d %4d  %02X  %02X  %02X  %02X  %02X  %02X  %02X  %02X  %02X  %02X  %02X  %02X %d%d%d "
                               "%3d %3d %3d %3d %3d %3d ", r[i].row, r[i].scanline,
                               r[i].colup0, r[i].colup1, r[i].colupf, r[i].colubk,
                               r[i].pf0, r[i].pf1, r[i].pf2, r[i].ctrlpf, r[i].grp0, r[i].grp1,
                               r[i].nusiz0, r[i].nusiz1, r[i].enables & 1, (r[i].enables >> 1) & 1,
                               (r[i].enables >> 2) & 1, r[i].pos[A2600VID_P0], r[i].pos[A2600VID_P1],
                               r[i].pos[A2600VID_M0], r[i].pos[A2600VID_M1], r[i].pos[A2600VID_BL],
                               r[i].writes);
        g_string_append_printf(s, "%s%s%s%s%s", r[i].flags & A2600VID_F_VBLANK ? "VBLANK " : "",
                               r[i].changed_mask & A2600VID_LC_COLOUR ? "colour " : "",
                               r[i].changed_mask & A2600VID_LC_PLAYFIELD ? "playfield " : "",
                               r[i].changed_mask & A2600VID_LC_GRAPHICS ? "graphics " : "",
                               r[i].changed_mask & A2600VID_LC_POSITION ? "position " : "");
        g_string_append_c(s, '\n');
        /* the first row is always listed; the registers moving is the news */
        g_string_append_c(hl, i > 0 && r[i].changed_mask ? '1' : '0');
    }
    if (!n) g_string_append(s, "The machine is not running.\n");
    put_table(v->lines_text, &v->lines_sig, s, hl);
    g_string_free(s, TRUE);
    g_string_free(hl, TRUE);
}

/* The writes the frame made, oldest first. */
static void fill_writes(Vid *v)
{
    const a2600vid_write *r;
    int n = a2600vid_tab_writes(v->tab, &r), i;
    GString *s = g_string_new(NULL), *hl = g_string_new(NULL);
    g_string_append(s, "Row Line Clock Register Value Old\n");
    g_string_append_c(hl, '0');
    for (i = 0; i < n; i++) {
        if (r[i].row >= 0)
            g_string_append_printf(s, "%3d %4d %5d %-8s   %02X  %02X\n", r[i].row, r[i].scanline,
                                   r[i].clock, r[i].name, r[i].value, r[i].old);
        else
            g_string_append_printf(s, "  -    - %5d %-8s   %02X  %02X\n", r[i].clock, r[i].name,
                                   r[i].value, r[i].old);
        g_string_append_c(hl, r[i].value != r[i].old ? '1' : '0');
    }
    if (!n) g_string_append(s, "No writes recorded (the machine is not running).\n");
    if (a2600vid_tab_writes_dropped(v->tab))
        g_string_append_printf(s, "(%d older writes were lost to the host's ring)\n",
                               a2600vid_tab_writes_dropped(v->tab));
    put_table(v->writes_text, &v->writes_sig, s, hl);
    g_string_free(s, TRUE);
    g_string_free(hl, TRUE);
}

/* ---- colours ------------------------------------------------------------------ */

static void draw_swatch(GtkDrawingArea *area, cairo_t *cr, int w, int h, gpointer ud)
{
    Vid *v = ud;
    const a2600vid_swatch *sw;
    int index = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(area), "idx"));
    (void)w; (void)h;
    if (a2600vid_tab_swatches(v->tab, &sw) <= index) return;
    set_rgb(cr, sw[index].rgb);
    cairo_rectangle(cr, 0, 0, 22, 22);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, 0.5, 0.5, 0.5, 0.8);
    cairo_set_line_width(cr, 1);
    cairo_rectangle(cr, 0.5, 0.5, 21, 21);
    cairo_stroke(cr);
    if (sw[index].inspected) {      /* the inspected pixel uses this colour */
        cairo_set_source_rgb(cr, 1, 0.85, 0.1);
        cairo_set_line_width(cr, 2);
        cairo_rectangle(cr, 1, 1, 20, 20);
        cairo_stroke(cr);
    }
}

static void on_swatch_break(GtkButton *b, gpointer ud)
{
    Vid *v = ud;
    int index = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "idx"));
    const a2600vid_swatch *sw;
    int id = a2600vid_tab_break_on_swatch(v->tab, index), n = a2600vid_tab_swatches(v->tab, &sw);
    char buf[120];
    if (id >= 0 && index < n)
        g_snprintf(buf, sizeof buf, "Break on writes to $%02X (%s)", sw[index].address, sw[index].reg_name);
    else
        g_snprintf(buf, sizeof buf, "Could not add a breakpoint");
    gtk_label_set_text(v->note, buf);
}

static void fill_colours(Vid *v)
{
    const a2600vid_swatch *sw;
    int n = a2600vid_tab_swatches(v->tab, &sw), index;
    for (index = 0; index < NSWATCH; index++) {
        char buf[96];
        if (index < n)
            g_snprintf(buf, sizeof buf, "%-2s %-6s $%02X = %02X  #%06X  %ld px", sw[index].name,
                       sw[index].reg_name, sw[index].address, sw[index].value,
                       sw[index].rgb & 0xffffff, sw[index].uses);
        else
            g_snprintf(buf, sizeof buf, "-");
        gtk_label_set_text(v->swatch_lbl[index], buf);
        gtk_widget_queue_draw(GTK_WIDGET(v->swatch_box[index]));
    }
}

static void fill_timing(Vid *v)
{
    const char *const *lines;
    int n = a2600vid_tab_timing(v->tab, &lines), i;
    GString *s = g_string_new(NULL);
    for (i = 0; i < n; i++) {
        const char *tab = strchr(lines[i], '\t');
        if (tab) g_string_append_printf(s, "%-14.*s %s\n", (int)(tab - lines[i]), lines[i], tab + 1);
        else g_string_append_printf(s, "%s\n", lines[i]);
    }
    gtk_label_set_text(v->timing_text, s->len ? s->str : "The machine is not running.");
    g_string_free(s, TRUE);
    gtk_progress_bar_set_fraction(v->beam, a2600vid_tab_beam_permille(v->tab) / 1000.0);
}

/* ---- refresh ------------------------------------------------------------------- */

static void fill_section(Vid *v, int sec)
{
    switch (sec) {
    case A2600VID_S_REGISTERS: fill_regs(v); break;
    case A2600VID_S_COLOURS: fill_colours(v); break;
    case A2600VID_S_LINES: fill_lines(v); break;
    case A2600VID_S_WRITES: fill_writes(v); break;
    case A2600VID_S_TIMING: fill_timing(v); break;
    default: update_pixel_section(v); break;
    }
}

static void refresh_view(Vid *v, unsigned what)
{
    if (what & (A2600VID_TAB_PICTURE | A2600VID_TAB_OFF)) {
        int pw = 0, ph = 0;
        if (a2600vid_tab_picture(v->tab, &pw, &ph)) {
            gtk_drawing_area_set_content_width(v->pic, pw);
            gtk_drawing_area_set_content_height(v->pic, ph);
        } else {
            gtk_drawing_area_set_content_width(v->pic, v->vp_w > 4 ? v->vp_w : 480);
            gtk_drawing_area_set_content_height(v->pic, v->vp_h > 4 ? v->vp_h : 300);
        }
        gtk_widget_queue_draw(GTK_WIDGET(v->pic));
    }
    if (what & A2600VID_TAB_TABLES) {
        gtk_label_set_text(v->status, a2600vid_tab_status(v->tab));
        fill_section(v, gtk_notebook_get_current_page(v->sections));
    }
}

static void sync_viewport(Vid *v)
{
    int w = gtk_widget_get_width(v->pic_scroll), h = gtk_widget_get_height(v->pic_scroll);
    if (w > 4 && h > 4 && (w != v->vp_w || h != v->vp_h)) {
        v->vp_w = w;
        v->vp_h = h;
        a2600vid_tab_set_viewport(v->tab, w - 2, h - 2);
    }
}

void dbg_video_refresh(GtkWidget *page, gboolean force)
{
    Vid *v = g_object_get_data(G_OBJECT(page), "vid");
    gboolean mapped;
    unsigned what;
    if (!v) return;
    mapped = gtk_widget_get_mapped(page);
    if (mapped && !v->was_mapped) force = TRUE;      /* just shown */
    v->was_mapped = mapped;
    if (!v->primed) force = TRUE;
    v->primed = TRUE;
    sync_viewport(v);
    what = a2600vid_tab_refresh(v->tab, force);
    if (force) what |= A2600VID_TAB_PICTURE | A2600VID_TAB_TABLES;
    refresh_view(v, what);
}

/* The picture's room changed (window resized, split dragged): Fit follows it. */
static gboolean on_tick(GtkWidget *w, GdkFrameClock *fc, gpointer ud)
{
    Vid *v = ud;
    int pw = gtk_widget_get_width(w), ph = gtk_widget_get_height(w);
    (void)fc;
    if (pw > 4 && ph > 4 && (pw != v->vp_w || ph != v->vp_h)) {
        sync_viewport(v);
        if (v->primed && a2600vid_tab_zoom(v->tab) == 0)
            refresh_view(v, a2600vid_tab_refresh(v->tab, TRUE) | A2600VID_TAB_PICTURE);
    }
    return G_SOURCE_CONTINUE;
}

/* ---- controls ------------------------------------------------------------------- */

static void refresh_all(Vid *v)
{
    refresh_view(v, a2600vid_tab_refresh(v->tab, TRUE) | A2600VID_TAB_PICTURE | A2600VID_TAB_TABLES);
}

static void on_view(GObject *o, GParamSpec *p, gpointer ud)
{
    Vid *v = ud;
    (void)o; (void)p;
    if (v->updating) return;
    a2600vid_tab_set_view(v->tab, (int)gtk_drop_down_get_selected(v->view));
    refresh_all(v);
}

static void on_source(GObject *o, GParamSpec *p, gpointer ud)
{
    Vid *v = ud;
    (void)o; (void)p;
    if (v->updating) return;
    a2600vid_tab_set_source(v->tab, (int)gtk_drop_down_get_selected(v->source));
    refresh_all(v);
}

static void on_zoom(GObject *o, GParamSpec *p, gpointer ud)
{
    Vid *v = ud;
    int n;
    const int *z = a2600vid_zooms(&n);
    guint i = gtk_drop_down_get_selected(v->zoom);
    (void)o; (void)p;
    if (v->updating || (int)i >= n) return;
    sync_viewport(v);
    a2600vid_tab_set_zoom(v->tab, z[i]);
    refresh_all(v);
}

static void on_overlay(GtkCheckButton *b, gpointer ud)
{
    Vid *v = ud;
    unsigned m = 0;
    int n, i;
    const a2600vid_overlay_info *d = a2600vid_overlays(&n);
    (void)b;
    if (v->updating) return;
    for (i = 0; i < n && i < v->nov; i++)
        if (gtk_check_button_get_active(v->ov[i])) m |= d[i].bit;
    a2600vid_tab_set_overlays(v->tab, m);
    refresh_all(v);
}

static void on_unpin(GtkButton *b, gpointer ud)
{
    Vid *v = ud;
    (void)b;
    a2600vid_tab_unpin(v->tab);
    gtk_widget_queue_draw(GTK_WIDGET(v->pic));
    update_pixel_section(v);
}

static void on_break(GtkButton *b, gpointer ud)
{
    (void)b;
    do_break_pixel(ud);
}

/* A TIA register by name (colup0, pf1, posp0 ...) gets the value; a strobe's
 * name (wsync, resp0, hmove ...) fires it. */
static void on_tia_set(GtkEntry *entry, gpointer ud)
{
    Vid *v = ud;
    a2600debug *dbg = a2600session_debugger(v->session);
    const char *reg = gtk_editable_get_text(GTK_EDITABLE(v->tia_reg));
    const char *val = gtk_editable_get_text(GTK_EDITABLE(v->tia_val));
    char *end;
    long number;
    (void)entry;
    if (!*reg) return;
    if (a2600debug_tia_strobe(dbg, reg) != 0) {
        while (*val == ' ') val++;
        number = *val == '$' ? strtol(val + 1, &end, 16) : strtol(val, &end, 16);
        if (end == val || (*val == '$' && end == val + 1) ||
            a2600debug_tia_set(dbg, reg, (int)number) != 0) {
            gtk_label_set_text(v->note, "Not a TIA register or strobe, or no value (hex)");
            return;
        }
        gtk_label_set_text(v->note, "Register set");
    } else {
        gtk_label_set_text(v->note, "Strobe fired");
    }
    refresh_all(v);
}

static void on_section(GtkNotebook *nb, GtkWidget *page, guint n, gpointer ud)
{
    Vid *v = ud;
    (void)nb; (void)page;
    fill_section(v, (int)n);
}

static void on_destroy(GtkWidget *w, gpointer ud)
{
    Vid *v = ud;
    (void)w;
    a2600vid_tab_free(v->tab);
    g_free(v->regs_sig);
    g_free(v->lines_sig);
    g_free(v->writes_sig);
    g_free(v);
}

GtkWidget *dbg_video_new(a2600session *session)
{
    Vid *v = g_new0(Vid, 1);
    GtkWidget *root, *left, *bar, *right, *box, *btns, *b1, *b2, *scroll, *ovbtn, *pop, *ovbox, *grid, *brk;
    GtkEventController *motion;
    GtkGesture *click;
    GtkTextBuffer *tb;
    const a2600vid_overlay_info *od;
    const int *zv;
    const char *views[A2600VID_V_COUNT + 1], *sources[A2600VID_D_COUNT + 1], *zlabels[24];
    char zbuf[24][8];
    int n, nz, i;

    v->session = session;
    v->tab = a2600vid_tab_new(session);

    root = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    g_object_set_data(G_OBJECT(root), "vid", v);
    g_signal_connect(root, "destroy", G_CALLBACK(on_destroy), v);

    /* ---- left: toolbar, status, picture */
    left = padded_box();
    bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    for (i = 0; i < A2600VID_V_COUNT; i++) views[i] = a2600vid_view_name(i);
    views[A2600VID_V_COUNT] = NULL;
    v->view = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(views));
    gtk_widget_set_tooltip_text(GTK_WIDGET(v->view),
        "Display: the last finished frame, as the TIA drew it.\n"
        "Decoded: each pixel tinted by the object that drew it (see Source).\n"
        "Beam: this frame above the beam, redrawn from the registers; the last one dimmed below.\n"
        "Values: the raw colour bytes as false colours.");
    for (i = 0; i < A2600VID_D_COUNT; i++) sources[i] = a2600vid_source_name(i);
    sources[A2600VID_D_COUNT] = NULL;
    v->source = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(sources));
    gtk_widget_set_tooltip_text(GTK_WIDGET(v->source),
        "What Decoded shows: each pixel tinted by what drew it, or the 128 TIA colours.");
    zv = a2600vid_zooms(&nz);
    if (nz > 23) nz = 23;
    for (i = 0; i < nz; i++) {
        if (zv[i] == 0) g_strlcpy(zbuf[i], "Fit", sizeof zbuf[i]);
        else g_snprintf(zbuf[i], sizeof zbuf[i], "%d×", zv[i]);
        zlabels[i] = zbuf[i];
    }
    zlabels[nz] = NULL;
    v->zoom = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(zlabels));
    gtk_box_append(GTK_BOX(bar), gtk_label_new("View"));
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(v->view));
    gtk_box_append(GTK_BOX(bar), gtk_label_new("Source"));
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(v->source));
    gtk_box_append(GTK_BOX(bar), gtk_label_new("Zoom"));
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(v->zoom));

    od = a2600vid_overlays(&n);
    if (n > MAX_OVERLAYS) n = MAX_OVERLAYS;
    v->nov = n;
    ovbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    for (i = 0; i < n; i++) {
        v->ov[i] = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(od[i].label));
        g_signal_connect(v->ov[i], "toggled", G_CALLBACK(on_overlay), v);
        gtk_box_append(GTK_BOX(ovbox), GTK_WIDGET(v->ov[i]));
    }
    pop = gtk_popover_new();
    gtk_popover_set_child(GTK_POPOVER(pop), ovbox);
    ovbtn = gtk_menu_button_new();
    gtk_menu_button_set_label(GTK_MENU_BUTTON(ovbtn), "Overlays");
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(ovbtn), pop);
    gtk_box_append(GTK_BOX(bar), ovbtn);

    brk = gtk_button_new_with_label("Break on writes");
    gtk_widget_set_tooltip_text(brk, "Break when the CPU writes the registers behind the inspected "
                                     "pixel (its colour, graphics and position)");
    g_signal_connect(brk, "clicked", G_CALLBACK(on_break), v);
    gtk_box_append(GTK_BOX(bar), brk);
    gtk_box_append(GTK_BOX(left), bar);

    v->status = GTK_LABEL(mono_label());
    gtk_label_set_selectable(v->status, FALSE);
    gtk_box_append(GTK_BOX(left), GTK_WIDGET(v->status));

    v->pic = GTK_DRAWING_AREA(gtk_drawing_area_new());
    gtk_drawing_area_set_draw_func(v->pic, draw_pic, v, NULL);
    gtk_drawing_area_set_content_width(v->pic, 480);
    gtk_drawing_area_set_content_height(v->pic, 300);
    gtk_widget_set_halign(GTK_WIDGET(v->pic), GTK_ALIGN_START);
    gtk_widget_set_valign(GTK_WIDGET(v->pic), GTK_ALIGN_START);
    motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "motion", G_CALLBACK(on_motion), v);
    g_signal_connect(motion, "leave", G_CALLBACK(on_leave), v);
    gtk_widget_add_controller(GTK_WIDGET(v->pic), motion);
    click = gtk_gesture_click_new();
    g_signal_connect(click, "pressed", G_CALLBACK(on_click), v);
    gtk_widget_add_controller(GTK_WIDGET(v->pic), GTK_EVENT_CONTROLLER(click));
    scroll = gtk_scrolled_window_new();
    v->pic_scroll = scroll;
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(v->pic));
    gtk_widget_set_hexpand(scroll, TRUE);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_widget_add_tick_callback(scroll, on_tick, v, NULL);
    gtk_box_append(GTK_BOX(left), scroll);

    v->note = GTK_LABEL(gtk_label_new(""));
    gtk_label_set_xalign(v->note, 0);
    gtk_label_set_wrap(v->note, TRUE);
    gtk_box_append(GTK_BOX(left), GTK_WIDGET(v->note));

    /* ---- right: the six sections, in a2600vid_section_name order */
    right = gtk_notebook_new();
    v->sections = GTK_NOTEBOOK(right);
    gtk_widget_set_size_request(right, 520, -1);

    /* Pixel */
    box = padded_box();
    v->pixel_text = GTK_LABEL(mono_label());
    gtk_label_set_wrap(v->pixel_text, TRUE);
    gtk_label_set_wrap_mode(v->pixel_text, PANGO_WRAP_WORD_CHAR);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(v->pixel_text));
    v->pixel_note = GTK_LABEL(gtk_label_new(""));
    gtk_label_set_xalign(v->pixel_note, 0);
    gtk_label_set_wrap(v->pixel_note, TRUE);
    gtk_label_set_wrap_mode(v->pixel_note, PANGO_WRAP_WORD_CHAR);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(v->pixel_note));
    btns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    b1 = gtk_button_new_with_label("Break on writes");
    b2 = gtk_button_new_with_label("Unpin");
    g_signal_connect(b1, "clicked", G_CALLBACK(on_break), v);
    g_signal_connect(b2, "clicked", G_CALLBACK(on_unpin), v);
    gtk_box_append(GTK_BOX(btns), b1);
    gtk_box_append(GTK_BOX(btns), b2);
    gtk_box_append(GTK_BOX(box), btns);
    scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), box);
    gtk_notebook_append_page(v->sections, scroll, gtk_label_new(a2600vid_section_name(A2600VID_S_PIXEL)));

    /* Registers */
    v->regs_text = GTK_TEXT_VIEW(gtk_text_view_new());
    gtk_text_view_set_editable(v->regs_text, FALSE);
    gtk_text_view_set_cursor_visible(v->regs_text, FALSE);
    gtk_text_view_set_monospace(v->regs_text, TRUE);
    gtk_text_view_set_wrap_mode(v->regs_text, GTK_WRAP_WORD_CHAR);
    tb = gtk_text_view_get_buffer(v->regs_text);
    gtk_text_buffer_create_tag(tb, "changed", "background", "#E4002B", "foreground", "#ffffff",
                               "weight", 700, NULL);
    scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(v->regs_text));
    gtk_widget_set_vexpand(scroll, TRUE);
    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_append(GTK_BOX(box), scroll);
    {   /* the TIA's registers and strobes, by name, as Stella's TIA tab sets them */
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6), *go;
        v->tia_reg = GTK_ENTRY(gtk_entry_new());
        gtk_editable_set_width_chars(GTK_EDITABLE(v->tia_reg), 9);
        gtk_entry_set_placeholder_text(v->tia_reg, "colup0 / wsync");
        v->tia_val = GTK_ENTRY(gtk_entry_new());
        gtk_editable_set_width_chars(GTK_EDITABLE(v->tia_val), 5);
        gtk_entry_set_placeholder_text(v->tia_val, "$1E");
        go = gtk_button_new_with_label("Set / Strobe");
        gtk_widget_set_tooltip_text(go, "A register name and a hex value sets it (colup0, pf1, posp0, "
            "refp0, vsync ...); a strobe's name alone fires it (wsync rsync resp0 resp1 resm0 resm1 "
            "resbl hmove hmclr cxclr)");
        g_signal_connect(v->tia_reg, "activate", G_CALLBACK(on_tia_set), v);
        g_signal_connect(v->tia_val, "activate", G_CALLBACK(on_tia_set), v);
        g_signal_connect(go, "clicked", G_CALLBACK(on_tia_set), v);
        gtk_box_append(GTK_BOX(row), gtk_label_new("TIA"));
        gtk_box_append(GTK_BOX(row), GTK_WIDGET(v->tia_reg));
        gtk_box_append(GTK_BOX(row), GTK_WIDGET(v->tia_val));
        gtk_box_append(GTK_BOX(row), go);
        gtk_widget_set_margin_start(row, 6);
        gtk_widget_set_margin_end(row, 6);
        gtk_widget_set_margin_bottom(row, 6);
        gtk_box_append(GTK_BOX(box), row);
    }
    gtk_notebook_append_page(v->sections, box, gtk_label_new(a2600vid_section_name(A2600VID_S_REGISTERS)));

    /* Colours: the seven objects, each with the colour register it is drawn from */
    box = padded_box();
    grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 3);
    for (i = 0; i < NSWATCH; i++) {
        GtkWidget *bk = gtk_button_new_with_label("Break");
        const int column = 0, row = i;
        v->swatch_box[i] = GTK_DRAWING_AREA(gtk_drawing_area_new());
        gtk_drawing_area_set_content_width(v->swatch_box[i], 22);
        gtk_drawing_area_set_content_height(v->swatch_box[i], 22);
        g_object_set_data(G_OBJECT(v->swatch_box[i]), "idx", GINT_TO_POINTER(i));
        gtk_drawing_area_set_draw_func(v->swatch_box[i], draw_swatch, v, NULL);
        v->swatch_lbl[i] = GTK_LABEL(mono_label());
        gtk_label_set_selectable(v->swatch_lbl[i], FALSE);
        g_object_set_data(G_OBJECT(bk), "idx", GINT_TO_POINTER(i));
        gtk_widget_add_css_class(bk, "flat");
        gtk_widget_set_tooltip_text(bk, "Break when the program writes this colour register");
        g_signal_connect(bk, "clicked", G_CALLBACK(on_swatch_break), v);
        gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(v->swatch_box[i]), column, row, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(v->swatch_lbl[i]), column + 1, row, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), bk, column + 2, row, 1, 1);
    }
    gtk_box_append(GTK_BOX(box), grid);
    scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), box);
    gtk_notebook_append_page(v->sections, scroll, gtk_label_new(a2600vid_section_name(A2600VID_S_COLOURS)));

    /* Lines */
    v->lines_text = GTK_TEXT_VIEW(gtk_text_view_new());
    gtk_text_view_set_editable(v->lines_text, FALSE);
    gtk_text_view_set_cursor_visible(v->lines_text, FALSE);
    gtk_text_view_set_monospace(v->lines_text, TRUE);
    tb = gtk_text_view_get_buffer(v->lines_text);
    gtk_text_buffer_create_tag(tb, "changed", "background", "rgba(228,0,43,0.25)", NULL);
    scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(v->lines_text));
    gtk_notebook_append_page(v->sections, scroll, gtk_label_new(a2600vid_section_name(A2600VID_S_LINES)));

    /* Writes */
    v->writes_text = GTK_TEXT_VIEW(gtk_text_view_new());
    gtk_text_view_set_editable(v->writes_text, FALSE);
    gtk_text_view_set_cursor_visible(v->writes_text, FALSE);
    gtk_text_view_set_monospace(v->writes_text, TRUE);
    tb = gtk_text_view_get_buffer(v->writes_text);
    gtk_text_buffer_create_tag(tb, "changed", "background", "rgba(228,0,43,0.25)", NULL);
    scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(v->writes_text));
    gtk_notebook_append_page(v->sections, scroll, gtk_label_new(a2600vid_section_name(A2600VID_S_WRITES)));

    /* Timing */
    box = padded_box();
    v->beam = GTK_PROGRESS_BAR(gtk_progress_bar_new());
    gtk_progress_bar_set_text(v->beam, "beam");
    gtk_progress_bar_set_show_text(v->beam, TRUE);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(v->beam));
    v->timing_text = GTK_LABEL(mono_label());
    gtk_label_set_selectable(v->timing_text, FALSE);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(v->timing_text));
    gtk_notebook_append_page(v->sections, box, gtk_label_new(a2600vid_section_name(A2600VID_S_TIMING)));

    gtk_paned_set_start_child(GTK_PANED(root), left);
    gtk_paned_set_end_child(GTK_PANED(root), right);
    gtk_paned_set_resize_start_child(GTK_PANED(root), TRUE);
    gtk_paned_set_shrink_start_child(GTK_PANED(root), FALSE);
    gtk_paned_set_resize_end_child(GTK_PANED(root), FALSE);
    gtk_paned_set_shrink_end_child(GTK_PANED(root), FALSE);

    /* start from the controller's state (the A2600_DEBUGGER_VIDEO_* variables) */
    v->updating = TRUE;
    gtk_drop_down_set_selected(v->view, (guint)a2600vid_tab_view(v->tab));
    gtk_drop_down_set_selected(v->source, (guint)a2600vid_tab_source(v->tab));
    for (i = 0; i < nz; i++)
        if (zv[i] == a2600vid_tab_zoom(v->tab)) gtk_drop_down_set_selected(v->zoom, (guint)i);
    for (i = 0; i < v->nov; i++)
        gtk_check_button_set_active(v->ov[i], (a2600vid_tab_overlays(v->tab) & od[i].bit) != 0);
    gtk_notebook_set_current_page(v->sections, a2600vid_tab_env_section(v->tab));
    v->updating = FALSE;
    update_pixel_section(v);
    g_signal_connect(v->view, "notify::selected", G_CALLBACK(on_view), v);
    g_signal_connect(v->source, "notify::selected", G_CALLBACK(on_source), v);
    g_signal_connect(v->zoom, "notify::selected", G_CALLBACK(on_zoom), v);
    g_signal_connect(v->sections, "switch-page", G_CALLBACK(on_section), v);
    return root;
}
