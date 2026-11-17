/*
 * The Controllers window -- the Atari 2600's two controller ports drawn as a
 * player sees them: each port shows what is plugged into it (the CX40 joystick,
 * a pair of CX30 paddles, the CX20 driving controller or the CX50 keypad
 * controller) over the six-switch console with its wood strip. Everything is
 * drawn from the session's layout (a2600session_controls_layout), in the
 * layout's units, so every frontend shows the same controls in the same places.
 *
 * Mouse: a press on a button or key holds it; a press in the joystick's boot
 * tilts the stick toward the pointer (the cap follows, eight directions) and a
 * drag follows; a press on a paddle's knob turns it to the pointer's angle and
 * a drag follows (the scroll wheel nudges it); the driving controller's wheel
 * turns as the pointer circles it (a notch per 22.5 degrees, the wheel notches
 * it too); a console slide switch flips on a click (SELECT and RESET are held
 * while pressed, POWER power-cycles and STOP stops in the debugger on release).
 * Every press is the raw press and release (a GtkGestureDrag, not a click: a
 * click gives up when the pointer wobbles), because the machine samples its
 * ports once per frame. Losing the focus, hiding and the pointer leaving a
 * held button let go of whatever the mouse holds here.
 *
 * What the machine sees held -- keyboard, gamepads and this window together
 * (a2600session_buttons_held, switch_held, analog_get) -- is polled every frame
 * and painted in the held orange, so the window doubles as a gamepad tester.
 *
 * Keys typed with the window active go through the main window
 * (a2600_window_key), so the machine, F9 and the shortcuts behave the same
 * whichever window has the focus.
 *
 * FIXED SIZE, FLOATING: the drawing is one size for every pair of controllers
 * (the core's layout has the same extent for all of them), the labels ellipsize
 * rather than grow, and the window is not resizable, so neither Map mode, a game
 * swapping a controller nor a long gamepad name changes its size and tiling
 * compositors float it. It is hidden at startup (F9 or the menu shows it).
 * SINGLETON, hidden rather than destroyed, so a remap survives closing it. Its
 * timers run only while it is on screen.
 *
 * MAP MODE: the Map button arms a two-step rebind -- click a control to pick the
 * target (the four quarters of the joystick's boot are its four directions), or
 * choose one from the list, then press a keyboard key OR a gamepad button.
 * While armed the same gestures pick the target instead of pressing, every
 * control shows the key bound to it, and typed keys are not typed into the
 * machine; once a target is picked the other controls are greyed. Rebinding
 * steals the input from whatever held it. Escape cancels. Clear takes the picked
 * target's key and gamepad button away, Defaults puts every binding back.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "controllers_window.h"

#include "../help/help_window.h"
#include "../window.h"

#include <math.h>
#include <pango/pangocairo.h>
#include <string.h>

#define UNIT 24.0               /* pixels per layout unit */
#define MARGIN 14.0             /* the desk round the controllers, pixels */
#define MAX_PADS 8              /* pads listed (SDL opens few more) */
#define PAD_ROWS 2              /* pad rows the window reserves; more scroll */
#define PAD_ROW_H 34            /* one row's height, px */
#define PAD_GAP 4
#define WHEEL_NUDGE 2048        /* one scroll step turns a paddle knob by this much */
#define MAX_CTL 64
#define TICKS 36                /* the joystick ring's tick marks */
#define U(v) ((v) * UNIT)

/* the colours (DESIGN-atari.md): plastic black, fire red, the held orange */
#define COL_DESK_TOP 0x34302B
#define COL_DESK_BOT 0x211E1B
#define COL_DESK_EDGE 0x0E0C0A
#define COL_BLACK_TOP 0x262626
#define COL_BLACK_BOT 0x171717
#define COL_BLACK_HI 0x2E2E2E
#define COL_BLACK_LO 0x101010
#define COL_RED 0xE23B1F
#define COL_RED_HI 0xF05A38
#define COL_RED_DOWN 0xB02A14
#define COL_HELD 0xFFB04A
#define COL_SILVER 0xC9C9C9
#define COL_SILVER_SH 0x8E8E8E
#define COL_PIN 0xE58A1C
#define COL_LEGEND 0xE08A1F
#define COL_BLUE 0x8CCFEA

/* ---- singleton state ------------------------------------------------------ */

static GtkWidget *g_window;
static GtkWindow *g_parent;     /* the A2600Window: typed keys go through it */
static a2600session *g_session;
static GtkWidget *g_area;       /* the controllers' drawing area */

static const a2600_control *g_lay;   /* a2600session_controls_layout, for the types below */
static int g_n;
static float g_lw, g_lh;
static int g_type[2] = { -1, -1};    /* a2600_ctrl_type drawn on each port */

static unsigned g_held[2];      /* what the machine sees held: bit per a2600_action */
static int g_sw[A2600_SW_COUNT];     /* console switches: position, or held */
static int g_analog[2][3];      /* paddle A, paddle B, driving wheel positions */
static double g_wheel[2];       /* the driving wheel's angle, degrees */
static gboolean g_syncing;      /* a refresh, not the user, is moving a widget */

/* What the pointer is over / holds. */
typedef struct {
    int idx;                    /* layout index, or -1 */
    int target;                 /* what Map binds, or -1 (the stick: its quarter) */
} Hit;
static Hit g_hover = { -1, -1 };
static Hit g_press = { -1, -1 };
static double g_ptr_x, g_ptr_y; /* the pointer, for the scroll wheel */

/* What the mouse is dragging: the stick, a paddle's knob or the driving wheel. */
static struct {
    int kind;                   /* 0 none, 1 stick, 2 knob, 3 wheel */
    int port, paddle;
    double cenx, ceny, rad;     /* the dial, pixels */
    double lastang;             /* the wheel's last pointer angle */
    double carry;               /* degrees not yet a notch */
} g_drag;

/* Map mode: -2 idle, -1 armed and waiting for a target, >= 0 waiting for a
 * key or pad button to bind to that target. */
static int g_map_state = -2;
static GtkWidget *g_map_button, *g_clear_button, *g_map_hint, *g_show_bindings;
static GtkWidget *g_target_drop, *g_port_drop[2];
static GtkWidget *g_pad_box;
static GtkWidget *g_pad_eff[MAX_PADS];  /* each pad's "plays ..." */
static int g_pad_row_count;
static unsigned g_pad_generation = (unsigned)-1;
static guint g_capture_timer;
static guint g_swallow_code;    /* the key Map took: its repeats and release too */
static guint g_info_timer;

static void refresh_all(void);

/* "controllers_show_bindings", the same key the family's other windows use. */
static gboolean show_bindings_saved(void)
{
    return a2600session_get_int(g_session, "controllers_show_bindings", 0) > 0;
}

/* ---- small helpers ---------------------------------------------------------- */

static void rgb(cairo_t *cctx, guint32 col)
{
    cairo_set_source_rgb(cctx, ((col >> 16) & 0xFF) / 255.0, ((col >> 8) & 0xFF) / 255.0,
                         (col & 0xFF) / 255.0);
}

static void rgba(cairo_t *cctx, guint32 col, double alpha)
{
    cairo_set_source_rgba(cctx, ((col >> 16) & 0xFF) / 255.0, ((col >> 8) & 0xFF) / 255.0,
                          (col & 0xFF) / 255.0, alpha);
}

static void rrect(cairo_t *cctx, double xpos, double ypos, double wid, double hgt, double rad)
{
    if (rad > wid / 2) rad = wid / 2;
    if (rad > hgt / 2) rad = hgt / 2;
    cairo_new_sub_path(cctx);
    cairo_arc(cctx, xpos + wid - rad, ypos + rad, rad, -G_PI / 2, 0);
    cairo_arc(cctx, xpos + wid - rad, ypos + hgt - rad, rad, 0, G_PI / 2);
    cairo_arc(cctx, xpos + rad, ypos + hgt - rad, rad, G_PI / 2, G_PI);
    cairo_arc(cctx, xpos + rad, ypos + rad, rad, G_PI, 3 * G_PI / 2);
    cairo_close_path(cctx);
}

static void circle(cairo_t *cctx, double cenx, double ceny, double rad)
{
    cairo_new_sub_path(cctx);
    cairo_arc(cctx, cenx, ceny, rad, 0, 2 * G_PI);
}

static void stop_rgb(cairo_pattern_t *pat, double offset, guint32 col)
{
    cairo_pattern_add_color_stop_rgb(pat, offset, ((col >> 16) & 0xFF) / 255.0,
                                     ((col >> 8) & 0xFF) / 255.0, (col & 0xFF) / 255.0);
}

/* A vertical two-colour gradient over a box, set as the source. */
static void vgrad(cairo_t *cctx, double ypos, double hgt, guint32 top, guint32 bot)
{
    cairo_pattern_t *grad = cairo_pattern_create_linear(0, ypos, 0, ypos + hgt);
    stop_rgb(grad, 0, top);
    stop_rgb(grad, 1, bot);
    cairo_set_source(cctx, grad);
    cairo_pattern_destroy(grad);
}

/* Text in a box, in the current units (the drawing is scaled by UNIT, so a
 * size is in layout units). align -1 left, 0 centre, 1 right. */
static void txt(cairo_t *cctx, GtkWidget *widget, const char *text, double size, gboolean bold,
                gboolean italic, guint32 colour, double xpos, double ypos, double boxw,
                double boxh, int align)
{
    PangoLayout *lay = gtk_widget_create_pango_layout(widget, text);
    PangoContext *pctx = pango_layout_get_context(lay);
    cairo_font_options_t *fopt = cairo_font_options_create();
    PangoFontDescription *fdesc = pango_font_description_new();
    PangoRectangle logical;
    double offx;

    /* smooth at any scale: no pixel hinting on a drawing scaled by UNIT */
    cairo_font_options_set_hint_metrics(fopt, CAIRO_HINT_METRICS_OFF);
    cairo_font_options_set_hint_style(fopt, CAIRO_HINT_STYLE_NONE);
    pango_cairo_context_set_font_options(pctx, fopt);
    cairo_font_options_destroy(fopt);
    pango_font_description_set_family(fdesc, "Sans");
    pango_font_description_set_weight(fdesc, bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL);
    pango_font_description_set_style(fdesc, italic ? PANGO_STYLE_ITALIC : PANGO_STYLE_NORMAL);
    pango_font_description_set_absolute_size(fdesc, size * PANGO_SCALE);
    pango_layout_set_font_description(lay, fdesc);
    pango_cairo_update_layout(cctx, lay);
    pango_layout_get_extents(lay, NULL, &logical);
    offx = align < 0 ? 0 : align > 0 ? boxw - logical.width / (double)PANGO_SCALE
                                     : (boxw - logical.width / (double)PANGO_SCALE) / 2;
    rgb(cctx, colour);
    cairo_move_to(cctx, xpos + offx, ypos + (boxh - logical.height / (double)PANGO_SCALE) / 2);
    pango_cairo_show_layout(cctx, lay);
    cairo_new_path(cctx);
    pango_font_description_free(fdesc);
    g_object_unref(lay);
}

/* A host key's name as a small legend: the long ones shortened to fit. */
static void legend_name(guint32 keysym, char *dst, int dstsz)
{
    char name[32];
    a2600session_keysym_name(keysym, name, sizeof name);
    if (!strcmp(name, "Keypad Enter"))
        g_snprintf(dst, dstsz, "KP Ent");
    else if (g_str_has_prefix(name, "Keypad "))
        g_snprintf(dst, dstsz, "KP %s", name + 7);
    else if (g_str_has_prefix(name, "Left "))
        g_snprintf(dst, dstsz, "L %s", name + 5);
    else if (g_str_has_prefix(name, "Right "))
        g_snprintf(dst, dstsz, "R %s", name + 6);
    else if (!strcmp(name, "Backspace"))
        g_snprintf(dst, dstsz, "BkSp");
    else if (!strcmp(name, "Escape"))
        g_snprintf(dst, dstsz, "Esc");
    else
        g_snprintf(dst, dstsz, "%s", name);
}

static gboolean showing_bindings(void)
{
    return g_map_state != -2 ||
           gtk_check_button_get_active(GTK_CHECK_BUTTON(g_show_bindings));
}

/* The binding legend for a target, or "" when none or not shown. */
static void bound_text(int target, char *dst, int dstsz)
{
    a2600_binding bind;
    dst[0] = '\0';
    if (target < 0 || !showing_bindings()) return;
    bind = a2600session_binding_get(g_session, target);
    if (bind.keysym)
        legend_name(bind.keysym, dst, dstsz);
    else
        g_snprintf(dst, dstsz, "\xe2\x80\x94");
}

/* ---- geometry ---------------------------------------------------------------- */

static int area_width(void) { return (int)(2 * MARGIN + (g_lw > 0 ? g_lw : 30.6) * UNIT + 0.5); }
static int area_height(void) { return (int)(2 * MARGIN + (g_lh > 0 ? g_lh : 16.6) * UNIT + 0.5); }

static double part_x(const a2600_control *ctl) { return MARGIN + U(ctl->x); }
static double part_y(const a2600_control *ctl) { return MARGIN + U(ctl->y); }

/* Loads the layout for what is plugged into the two ports; TRUE when it changed. */
static gboolean reload_layout(void)
{
    const int left = a2600session_view_type(g_session, 0);
    const int right = a2600session_view_type(g_session, 1);
    if (left == g_type[0] && right == g_type[1] && g_lay)
        return FALSE;
    g_type[0] = left;
    g_type[1] = right;
    g_lay = a2600session_controls_layout(left, right, &g_n, &g_lw, &g_lh);
    if (g_n > MAX_CTL) g_n = MAX_CTL;
    return TRUE;
}

static int target_port(int target)
{
    return target >= 0 && target < 2 * A2600_ACT_PER_PORT ? target / A2600_ACT_PER_PORT : -1;
}

static int target_act(int target)
{
    return target >= 0 && target < 2 * A2600_ACT_PER_PORT ? target % A2600_ACT_PER_PORT : -1;
}

static gboolean is_sysact(int target)
{
    return target >= A2600_TARGET_SYSACT(0);
}

static gboolean is_switch(int target)
{
    return target >= A2600_TARGET_SWITCH(0) && target < A2600_TARGET_SYSACT(0);
}

/* The stick's target under a pointer: its four quarters are the four directions
 * (a diagonal belongs to the nearer axis). */
static int stick_target(const a2600_control *ctl, double xpos, double ypos)
{
    const double dx = xpos - (part_x(ctl) + U(ctl->w) / 2);
    const double dy = ypos - (part_y(ctl) + U(ctl->h) / 2);
    int act;
    if (fabs(dx) > fabs(dy))
        act = dx < 0 ? A2600_ACT_JOY_LEFT : A2600_ACT_JOY_RIGHT;
    else
        act = dy < 0 ? A2600_ACT_JOY_UP : A2600_ACT_JOY_DOWN;
    return A2600_TARGET_PORT(ctl->port, act);
}

static Hit hit_at(double xpos, double ypos)
{
    Hit hit = { -1, -1 };
    int idx;
    if (!g_lay) return hit;
    idx = a2600session_controls_hit(g_type[0], g_type[1], (float)((xpos - MARGIN) / UNIT),
                                    (float)((ypos - MARGIN) / UNIT));
    if (idx < 0 || idx >= g_n) return hit;
    hit.idx = idx;
    hit.target = g_lay[idx].target;
    if (g_lay[idx].kind == A2600_CTL_STICK)
        hit.target = stick_target(&g_lay[idx], xpos, ypos);
    return hit;
}

static gboolean same_hit(const Hit *aa, const Hit *bb)
{
    return aa->idx == bb->idx && aa->target == bb->target;
}

/* Is this part lit: the machine sees its target held (or it is the one pressed)? */
static gboolean part_lit(int idx)
{
    const a2600_control *ctl = &g_lay[idx];
    if (g_press.idx == idx && g_drag.kind == 0)
        return TRUE;
    if (ctl->target < 0) return FALSE;
    if (ctl->target < 2 * A2600_ACT_PER_PORT)
        return (g_held[target_port(ctl->target)] >> target_act(ctl->target)) & 1u;
    if (is_switch(ctl->target)) {
        const int sw = ctl->target - A2600_TARGET_SWITCH(0);
        return (sw == A2600_SW_SELECT || sw == A2600_SW_RESET) && g_sw[sw];
    }
    return FALSE;
}

/* Greyed while Map waits for a key for some other target. */
static gboolean dimmed(int idx)
{
    const a2600_control *ctl = &g_lay[idx];
    if (g_map_state < 0) return FALSE;
    if (ctl->kind == A2600_CTL_STICK) {
        const int base = A2600_TARGET_PORT(ctl->port, 0);
        return g_map_state < base + A2600_ACT_JOY_UP || g_map_state > base + A2600_ACT_JOY_RIGHT;
    }
    if (ctl->kind == A2600_CTL_BODY || ctl->kind == A2600_CTL_CAPTION) return FALSE;
    return ctl->target != g_map_state;
}

/* ---- drawing: the joystick ---------------------------------------------------- */

/* The case of the CX40 (DESIGN-atari.md section 2): a textured black square, the
 * fire button's teardrop recess, and the ring of tick marks round the boot. The
 * held directions light the ticks nearest them. */
static void draw_joy_case(cairo_t *cctx, GtkWidget *widget, int port)
{
    const unsigned held = g_held[port];
    const int dirx = (int)((held >> A2600_ACT_JOY_RIGHT) & 1u) - (int)((held >> A2600_ACT_JOY_LEFT) & 1u);
    const int diry = (int)((held >> A2600_ACT_JOY_DOWN) & 1u) - (int)((held >> A2600_ACT_JOY_UP) & 1u);
    const gboolean any = dirx || diry;
    const double want = any ? atan2((double)dirx, (double)-diry) * 180.0 / G_PI : 0;
    int tick;

    vgrad(cctx, 0, 8, COL_BLACK_TOP, COL_BLACK_BOT);
    rrect(cctx, 0, 0, 8, 8, 0.9);
    cairo_fill_preserve(cctx);
    rgb(cctx, 0x0B0B0B);
    cairo_set_line_width(cctx, 0.06);
    cairo_stroke(cctx);
    rgb(cctx, COL_BLACK_HI);
    rrect(cctx, 0.25, 0.25, 7.5, 7.5, 0.7);
    cairo_set_line_width(cctx, 0.07);
    cairo_stroke(cctx);

    /* the teardrop: the fire button's recess and the neck down to the ring */
    rgb(cctx, COL_BLACK_LO);
    cairo_set_line_width(cctx, 0.9);
    cairo_set_line_cap(cctx, CAIRO_LINE_CAP_BUTT);
    cairo_move_to(cctx, 1.7, 1.7);
    cairo_line_to(cctx, 4.5, 4.5);
    cairo_stroke(cctx);
    circle(cctx, 1.7, 1.7, 1.1);
    cairo_fill(cctx);

    /* the ring and its ticks */
    circle(cctx, 4.5, 4.5, 3.0);
    rgb(cctx, COL_BLACK_LO);
    cairo_fill_preserve(cctx);
    rgb(cctx, 0x050505);
    cairo_set_line_width(cctx, 0.05);
    cairo_stroke(cctx);
    cairo_set_line_cap(cctx, CAIRO_LINE_CAP_ROUND);
    for (tick = 0; tick < TICKS; tick++) {
        const double ang = tick * 360.0 / TICKS;
        double diff = fabs(fmod(ang - want + 540.0, 360.0) - 180.0);
        const gboolean lit = any && diff <= 25.0;
        const double sinv = sin(ang * G_PI / 180.0), cosv = -cos(ang * G_PI / 180.0);
        if (lit) rgb(cctx, COL_HELD); else rgb(cctx, 0x3A3A3A);
        cairo_set_line_width(cctx, lit ? 0.14 : 0.07);
        cairo_move_to(cctx, 4.5 + 2.55 * sinv, 4.5 + 2.55 * cosv);
        cairo_line_to(cctx, 4.5 + 2.85 * sinv, 4.5 + 2.85 * cosv);
        cairo_stroke(cctx);
    }
    txt(cctx, widget, "TOP", 0.3, TRUE, FALSE, 0x2E2E2E, 3.5, 1.6, 2.0, 0.4, 0);
}

/* The boot and the shaft's cap, which slides toward the held direction (up to
 * one unit; at a diagonal the same). The part's box is 4.2 square, centred on
 * the ring. */
static void draw_stick(cairo_t *cctx, const a2600_control *ctl)
{
    const unsigned held = g_held[ctl->port];
    const double dirx = (double)((held >> A2600_ACT_JOY_RIGHT) & 1u) - (double)((held >> A2600_ACT_JOY_LEFT) & 1u);
    const double diry = (double)((held >> A2600_ACT_JOY_DOWN) & 1u) - (double)((held >> A2600_ACT_JOY_UP) & 1u);
    const double len = sqrt(dirx * dirx + diry * diry);
    const double vecx = len > 0 ? dirx / len : 0, vecy = len > 0 ? diry / len : 0;
    const double cenx = ctl->w / 2, ceny = ctl->h / 2;
    int idx;

    /* the ribbed boot: concentric rings, each leaning further toward the tilt */
    static const double k_rad[3] = { 2.1, 1.7, 1.3 };
    static const guint32 k_col[3] = { 0x151515, 0x262626, 0x151515 };
    for (idx = 0; idx < 3; idx++) {
        circle(cctx, cenx + vecx * 0.35 * idx, ceny + vecy * 0.35 * idx, k_rad[idx]);
        rgb(cctx, k_col[idx]);
        cairo_fill_preserve(cctx);
        rgb(cctx, 0x0A0A0A);
        cairo_set_line_width(cctx, 0.04);
        cairo_stroke(cctx);
    }
    /* the octagonal cap */
    cairo_save(cctx);
    cairo_translate(cctx, cenx + vecx * 1.0, ceny + vecy * 1.0);
    cairo_new_path(cctx);
    for (idx = 0; idx < 8; idx++) {
        const double ang = (idx * 45.0 + 22.5) * G_PI / 180.0;
        if (idx == 0) cairo_move_to(cctx, 0.85 * sin(ang), -0.85 * cos(ang));
        else cairo_line_to(cctx, 0.85 * sin(ang), -0.85 * cos(ang));
    }
    cairo_close_path(cctx);
    rgb(cctx, 0x0C0C0C);
    cairo_fill_preserve(cctx);
    rgb(cctx, len > 0 ? COL_HELD : COL_BLACK_HI);
    cairo_set_line_width(cctx, len > 0 ? 0.12 : 0.1);
    cairo_stroke(cctx);
    cairo_restore(cctx);
}

/* The joystick's one red fire button, in its recess; the keypad's keys are round
 * buttons too (draw_key). */
static void draw_fire(cairo_t *cctx, int idx)
{
    const a2600_control *ctl = &g_lay[idx];
    const double cenx = ctl->w / 2, ceny = ctl->h / 2;
    const gboolean lit = part_lit(idx);
    cairo_pattern_t *grad = cairo_pattern_create_radial(cenx - 0.25, ceny - 0.3, 0.05, cenx, ceny, 0.85);
    stop_rgb(grad, 0, lit ? COL_RED : COL_RED_HI);
    stop_rgb(grad, 1, lit ? COL_RED_DOWN : COL_RED);
    circle(cctx, cenx, ceny, 0.8);
    cairo_set_source(cctx, grad);
    cairo_fill(cctx);
    cairo_pattern_destroy(grad);
    if (lit) {
        circle(cctx, cenx, ceny, 0.9);
        rgb(cctx, COL_HELD);
        cairo_set_line_width(cctx, 0.12);
        cairo_stroke(cctx);
    }
}

/* ---- drawing: paddles and the driving controller ------------------------------- */

/* The wedge of a CX30 (section 3): a black body, narrower at the cable end, with
 * the label plate in the corner. The driving controller has the same body, its
 * plate reading "driving" over a racing car. */
static void draw_paddle_case(cairo_t *cctx, GtkWidget *widget, gboolean driving)
{
    static const double k_poly[6][2] = { { 1.0, 0.3 }, { 5.6, 0.3 }, { 7.0, 5.0 },
                                         { 6.2, 5.9 }, { 0.8, 5.9 }, { 0.0, 4.6 } };
    int idx;

    cairo_new_path(cctx);
    for (idx = 0; idx < 6; idx++) {
        if (idx == 0) cairo_move_to(cctx, k_poly[idx][0], k_poly[idx][1]);
        else cairo_line_to(cctx, k_poly[idx][0], k_poly[idx][1]);
    }
    cairo_close_path(cctx);
    vgrad(cctx, 0.3, 5.6, 0x252525, 0x181818);
    cairo_fill_preserve(cctx);
    /* rounded corners: the same outline stroked with a round join, then the bevel */
    cairo_set_line_join(cctx, CAIRO_LINE_JOIN_ROUND);
    cairo_set_line_width(cctx, 0.6);
    vgrad(cctx, 0.3, 5.6, 0x252525, 0x181818);
    cairo_stroke_preserve(cctx);
    rgb(cctx, COL_BLACK_HI);
    cairo_set_line_width(cctx, 0.06);
    cairo_stroke(cctx);

    /* the label plate, in the corner the knob leaves free */
    rrect(cctx, 4.5, 4.7, 2.1, 0.95, 0.15);
    rgb(cctx, COL_BLACK_LO);
    cairo_fill_preserve(cctx);
    rgb(cctx, 0x8A8A8A);
    cairo_set_line_width(cctx, 0.05);
    cairo_stroke(cctx);
    if (!driving) {
        txt(cctx, widget, "paddle", 0.4, FALSE, TRUE, 0xE6E6E6, 4.55, 4.7, 1.2, 0.95, -1);
        /* two crossed rackets and a ball */
        rgb(cctx, 0xDCCB78);
        cairo_set_line_width(cctx, 0.06);
        cairo_move_to(cctx, 5.85, 5.5); cairo_line_to(cctx, 6.25, 4.85); cairo_stroke(cctx);
        cairo_move_to(cctx, 6.25, 5.5); cairo_line_to(cctx, 5.85, 4.85); cairo_stroke(cctx);
        circle(cctx, 5.95, 4.9, 0.16); cairo_stroke(cctx);
        circle(cctx, 6.15, 4.9, 0.16); cairo_stroke(cctx);
        circle(cctx, 6.05, 5.55, 0.06); cairo_fill(cctx);
    } else {
        txt(cctx, widget, "driving", 0.36, FALSE, TRUE, 0xE6E6E6, 4.55, 4.7, 1.3, 0.95, -1);
        /* a small orange racing car */
        rrect(cctx, 5.85, 5.2, 0.75, 0.3, 0.12);
        rgb(cctx, 0xE8501A);
        cairo_fill(cctx);
        rrect(cctx, 6.1, 5.05, 0.32, 0.22, 0.08);
        cairo_fill(cctx);
        rgb(cctx, 0x050505);
        circle(cctx, 6.05, 5.52, 0.1); cairo_fill(cctx);
        circle(cctx, 6.45, 5.52, 0.1); cairo_fill(cctx);
    }
}

/* One degree of a knob's mark: a paddle's pot turns +-135 degrees across its
 * travel; the driving wheel turns endlessly. */
static double knob_angle(const a2600_control *ctl)
{
    if (ctl->kind == A2600_CTL_WHEEL)
        return g_wheel[ctl->port];
    return g_analog[ctl->port][ctl->paddle == 1 ? 1 : 0] / 32767.0 * A2600_KNOB_TRAVEL_DEG;
}

/* The ribbed knob (60 knurl ticks on its rim, a flat cap and a position mark
 * that turns with it); `box` is 4.4 square, the knob centred. */
static void draw_knob(cairo_t *cctx, int idx)
{
    const a2600_control *ctl = &g_lay[idx];
    const gboolean wheel = ctl->kind == A2600_CTL_WHEEL;
    const double cenx = ctl->w / 2, ceny = ctl->h / 2;
    const double ang = knob_angle(ctl) * G_PI / 180.0;
    const gboolean active = (g_drag.kind == 2 || g_drag.kind == 3) && g_drag.port == ctl->port &&
                            g_drag.paddle == ctl->paddle;
    const gboolean moved = !wheel && g_analog[ctl->port][ctl->paddle == 1 ? 1 : 0] != 0;
    int tick;

    circle(cctx, cenx, ceny, 2.2);
    rgb(cctx, 0x0E0E0E);
    cairo_fill_preserve(cctx);
    rgb(cctx, active || moved ? COL_HELD : 0x050505);
    cairo_set_line_width(cctx, active || moved ? 0.1 : 0.05);
    cairo_stroke(cctx);
    /* the knurl: the ticks turn with the knob */
    cairo_set_line_width(cctx, 0.05);
    rgb(cctx, 0x2A2A2A);
    for (tick = 0; tick < (wheel ? 24 : 60); tick++) {
        const double a2 = ang + tick * 2 * G_PI / (wheel ? 24 : 60);
        cairo_move_to(cctx, cenx + 2.0 * sin(a2), ceny - 2.0 * cos(a2));
        cairo_line_to(cctx, cenx + 2.2 * sin(a2), ceny - 2.2 * cos(a2));
    }
    cairo_stroke(cctx);
    circle(cctx, cenx, ceny, 1.75);
    rgb(cctx, 0x1A1A1A);
    cairo_fill(cctx);
    {
        cairo_pattern_t *grad = cairo_pattern_create_radial(cenx - 0.5, ceny - 0.6, 0.1, cenx, ceny, 1.65);
        stop_rgb(grad, 0, 0x2E2E2E);
        stop_rgb(grad, 1, 0x1E1E1E);
        circle(cctx, cenx, ceny, 1.65);
        cairo_set_source(cctx, grad);
        cairo_fill(cctx);
        cairo_pattern_destroy(grad);
    }
    /* the position mark, which the real knob does not have: a light line from the
     * middle toward where the knob points */
    rgb(cctx, 0xD8D8D8);
    cairo_set_line_cap(cctx, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_width(cctx, 0.13);
    cairo_move_to(cctx, cenx + 0.9 * sin(ang), ceny - 0.9 * cos(ang));
    cairo_line_to(cctx, cenx + 1.55 * sin(ang), ceny - 1.55 * cos(ang));
    cairo_stroke(cctx);
}

/* The fire button on a paddle's left edge. */
static void draw_edge_button(cairo_t *cctx, int idx)
{
    const a2600_control *ctl = &g_lay[idx];
    const gboolean lit = part_lit(idx);
    vgrad(cctx, 0, ctl->h, lit ? 0xC03018 : 0xE8492C, lit ? COL_RED_DOWN : 0xD93A22);
    rrect(cctx, 0, 0, ctl->w, ctl->h, 0.3);
    cairo_fill_preserve(cctx);
    rgb(cctx, lit ? COL_HELD : 0x7A1D0E);
    cairo_set_line_width(cctx, lit ? 0.12 : 0.06);
    cairo_stroke(cctx);
}

/* A turn arrow under a knob (a paddle's left/right, the wheel's CCW/CW): held, it
 * keeps turning. */
static void draw_turn(cairo_t *cctx, int idx)
{
    const a2600_control *ctl = &g_lay[idx];
    const gboolean right = ctl->label && (ctl->label[0] == '>' || !strcmp(ctl->label, "CW"));
    const gboolean lit = part_lit(idx);
    rgb(cctx, lit ? COL_HELD : 0x4A4A4A);
    cairo_new_path(cctx);
    if (right) {
        cairo_move_to(cctx, 0.1, 0.1);
        cairo_line_to(cctx, 0.75, ctl->h / 2);
        cairo_line_to(cctx, 0.1, ctl->h - 0.1);
    } else {
        cairo_move_to(cctx, ctl->w - 0.1, 0.1);
        cairo_line_to(cctx, ctl->w - 0.75, ctl->h / 2);
        cairo_line_to(cctx, ctl->w - 0.1, ctl->h - 0.1);
    }
    cairo_close_path(cctx);
    cairo_fill(cctx);
}

/* ---- drawing: the keypad -------------------------------------------------------- */

/* The CX50 (section 5): a black case with an inset face and its title; the twelve
 * keys are round buttons drawn by draw_key. */
static void draw_keypad_case(cairo_t *cctx, GtkWidget *widget)
{
    rrect(cctx, 0, 0, 5.6, 8.0, 0.6);
    rgb(cctx, 0x141414);
    cairo_fill_preserve(cctx);
    rgb(cctx, 0x080808);
    cairo_set_line_width(cctx, 0.05);
    cairo_stroke(cctx);
    rrect(cctx, 0.45, 0.45, 4.7, 7.1, 0.35);
    rgb(cctx, 0x1A1A1A);
    cairo_fill_preserve(cctx);
    rgb(cctx, 0xD7D7D7);
    cairo_set_line_width(cctx, 0.07);
    cairo_stroke(cctx);
    txt(cctx, widget, "Keyboard", 0.62, TRUE, FALSE, COL_BLUE, 0.6, 0.85, 4.4, 0.8, 0);
    txt(cctx, widget, "Controller", 0.62, TRUE, FALSE, COL_BLUE, 0.6, 1.5, 4.4, 0.8, 0);
}

/* One key of the keypad: a glossy black round button, its printed legend up and to
 * the left of it. */
static void draw_key(cairo_t *cctx, GtkWidget *widget, int idx)
{
    const a2600_control *ctl = &g_lay[idx];
    const double cenx = ctl->w / 2, ceny = ctl->h / 2;
    const gboolean lit = part_lit(idx);
    cairo_pattern_t *grad = cairo_pattern_create_radial(cenx - 0.15, ceny - 0.2, 0.05, cenx, ceny, 0.5);
    stop_rgb(grad, 0, lit ? 0x3A3A3A : 0x2C2C2C);
    stop_rgb(grad, 1, 0x080808);
    circle(cctx, cenx, ceny, 0.5);
    cairo_set_source(cctx, grad);
    cairo_fill_preserve(cctx);
    cairo_pattern_destroy(grad);
    rgb(cctx, lit ? COL_HELD : 0x000000);
    cairo_set_line_width(cctx, lit ? 0.12 : 0.04);
    cairo_stroke(cctx);
    txt(cctx, widget, ctl->label, 0.46, TRUE, FALSE, lit ? 0xFFFFFF : COL_BLUE,
        cenx - 0.95, ceny - 0.95, 0.6, 0.5, 0);
}

/* ---- drawing: the console ------------------------------------------------------- */

/* The three bars of the Atari fuji, in silver. */
static void draw_fuji(cairo_t *cctx, double xpos, double ypos, double size)
{
    int bar;
    rgb(cctx, 0xC7C7C7);
    for (bar = 0; bar < 3; bar++) {
        const double off = (bar - 1) * size * 0.34;
        cairo_move_to(cctx, xpos + off - size * 0.08, ypos + size);
        cairo_curve_to(cctx, xpos + off - size * 0.08, ypos + size * 0.5,
                       xpos + off * 0.6 - size * 0.05, ypos + size * 0.15, xpos + off * 0.2, ypos);
        cairo_curve_to(cctx, xpos + off * 0.6 + size * 0.05, ypos + size * 0.15,
                       xpos + off + size * 0.08, ypos + size * 0.5, xpos + off + size * 0.08, ypos + size);
        cairo_close_path(cctx);
    }
    cairo_fill(cctx);
}

static void draw_console_panel(cairo_t *cctx, GtkWidget *widget)
{
    vgrad(cctx, 0, 5.2, 0x1A1A1A, 0x101010);
    rrect(cctx, 0, 0, 24, 5.2, 0.4);
    cairo_fill_preserve(cctx);
    rgb(cctx, 0x050505);
    cairo_set_line_width(cctx, 0.06);
    cairo_stroke(cctx);
    rgb(cctx, COL_PIN);
    rrect(cctx, 0.25, 0.25, 23.5, 4.7, 0.3);
    cairo_set_line_width(cctx, 0.06);
    cairo_stroke(cctx);
    txt(cctx, widget, "video computer system", 0.78, FALSE, TRUE, 0xE6DFCF, 6, 0.3, 12, 0.9, 0);
    txt(cctx, widget, "TM", 0.28, FALSE, FALSE, 0xE6DFCF, 16.4, 0.3, 0.7, 0.4, -1);
}

static void draw_cart_slot(cairo_t *cctx, const a2600_control *ctl)
{
    int row;
    rrect(cctx, 0, 0, ctl->w, ctl->h, 0.15);
    rgb(cctx, 0x0A0A0A);
    cairo_fill_preserve(cctx);
    rgb(cctx, COL_BLACK_TOP);
    cairo_set_line_width(cctx, 0.12);
    cairo_stroke(cctx);
    rgba(cctx, 0x2A2A2A, 0.7);
    cairo_set_line_width(cctx, 0.05);
    for (row = 0; row < 3; row++) {
        cairo_move_to(cctx, 0.3, 0.6 + row * 0.55);
        cairo_line_to(cctx, ctl->w - 0.3, 0.6 + row * 0.55);
        cairo_stroke(cctx);
    }
}

/* The woodgrain front under the panel: a brown gradient with grain lines, and
 * the Atari name at its right end. */
static void draw_wood(cairo_t *cctx, GtkWidget *widget, const a2600_control *ctl)
{
    int line;
    vgrad(cctx, 0, ctl->h, 0x8A5A2B, 0x6E4521);
    rrect(cctx, 0, 0, ctl->w, ctl->h, 0.12);
    cairo_fill_preserve(cctx);
    cairo_save(cctx);
    cairo_clip(cctx);
    rgba(cctx, 0x4A2F16, 0.2);
    cairo_set_line_width(cctx, 0.04);
    for (line = 0; line < 16; line++) {
        const double ypos = 0.1 + line * 0.1 + (line % 3) * 0.012;
        cairo_move_to(cctx, 0, ypos);
        cairo_curve_to(cctx, 6, ypos + 0.12 * ((line % 4) - 1.5), 15, ypos - 0.1 * ((line % 3) - 1),
                       ctl->w, ypos + 0.05 * (line % 5));
        cairo_stroke(cctx);
    }
    cairo_restore(cctx);
    draw_fuji(cctx, 20.1, 0.35, 0.9);
    txt(cctx, widget, "ATARI", 0.62, TRUE, FALSE, 0xC7C7C7, 20.8, 0.3, 3.0, 1.0, -1);
}

/* A slide switch (section 6): a black slot, the silver toggle at its top or bottom,
 * the legend printed above and the two positions beside. A momentary one (game
 * select, game reset) rests up, tilts down while held and has a sawtooth arrow
 * that lights with it. */
static void draw_switch(cairo_t *cctx, GtkWidget *widget, int idx)
{
    static const struct {
        int target;
        const char *top, *bot;
        gboolean momentary;
    } k_kind[] = {
        { A2600_TARGET_SYSACT(A2600_SYSACT_REBOOT_CONFIG), "on", "off", FALSE },
        { A2600_TARGET_SWITCH(A2600_SW_COLOR_BW), "color", "b-w", FALSE },
        { A2600_TARGET_SWITCH(A2600_SW_LEFT_DIFF), "a", "b", FALSE },
        { A2600_TARGET_SWITCH(A2600_SW_RIGHT_DIFF), "a", "b", FALSE },
        { A2600_TARGET_SWITCH(A2600_SW_SELECT), NULL, NULL, TRUE },
        { A2600_TARGET_SWITCH(A2600_SW_RESET), NULL, NULL, TRUE },
    };
    const a2600_control *ctl = &g_lay[idx];
    const int sw = is_switch(ctl->target) ? ctl->target - A2600_TARGET_SWITCH(0) : -1;
    const gboolean power = is_sysact(ctl->target);
    gboolean up = TRUE, lit = FALSE;
    const char *top = NULL, *bot = NULL;
    cairo_pattern_t *grad;
    size_t kind;
    double tog;

    for (kind = 0; kind < G_N_ELEMENTS(k_kind); kind++) {
        if (k_kind[kind].target != ctl->target) continue;
        top = k_kind[kind].top;
        bot = k_kind[kind].bot;
        if (k_kind[kind].momentary) {
            lit = g_sw[sw] != 0;
            up = !lit;
        } else if (!power) {
            up = g_sw[sw] != 0;     /* 1 = colour / A: the toggle is UP */
        } else {
            up = g_press.idx != idx;
            lit = g_press.idx == idx;
        }
    }
    /* the printed legend above, in orange (red for power) */
    if (ctl->legend)
        txt(cctx, widget, ctl->legend, 0.4, FALSE, FALSE, power ? 0xC8341E : COL_LEGEND,
            ctl->w / 2 - 1.9, -0.7, 3.8, 0.5, 0);
    /* the slot and the toggle */
    rrect(cctx, ctl->w / 2 - 0.25, 0.35, 0.5, 1.5, 0.15);
    rgb(cctx, 0x0A0A0A);
    cairo_fill(cctx);
    tog = up ? 0.35 : 1.15;
    grad = cairo_pattern_create_linear(ctl->w / 2 - 0.3, 0, ctl->w / 2 + 0.3, 0);
    stop_rgb(grad, 0, COL_SILVER_SH);
    stop_rgb(grad, 0.4, 0xE6E6E6);
    stop_rgb(grad, 1, COL_SILVER_SH);
    rrect(cctx, ctl->w / 2 - 0.3, tog, 0.6, 0.7, 0.12);
    cairo_set_source(cctx, grad);
    cairo_fill_preserve(cctx);
    cairo_pattern_destroy(grad);
    rgb(cctx, lit || (g_hover.idx == idx && g_map_state != -2) ? COL_HELD : 0x555555);
    cairo_set_line_width(cctx, lit ? 0.1 : 0.04);
    cairo_stroke(cctx);
    /* the two positions, beside the slot */
    if (top) txt(cctx, widget, top, 0.4, FALSE, FALSE, COL_LEGEND, ctl->w / 2 + 0.4, 0.35, 1.2, 0.6, -1);
    if (bot) txt(cctx, widget, bot, 0.4, FALSE, FALSE, COL_LEGEND, ctl->w / 2 + 0.4, 1.25, 1.2, 0.6, -1);
    /* the sawtooth arrow under a momentary switch */
    if (!top && !bot) {
        double xpos = ctl->w / 2 - 0.35;
        int tooth;
        rgb(cctx, lit ? COL_HELD : COL_LEGEND);
        cairo_set_line_width(cctx, 0.08);
        cairo_move_to(cctx, xpos, 2.15);
        for (tooth = 0; tooth < 3; tooth++) {
            cairo_line_to(cctx, xpos + 0.14, 2.05);
            cairo_line_to(cctx, xpos + 0.14, 2.15);
            xpos += 0.14;
        }
        cairo_stroke(cctx);
    }
}

/* The STOP button on the wood strip: the session's Pause (stop in the debugger),
 * which the real console has no switch for. */
static void draw_stop(cairo_t *cctx, GtkWidget *widget, int idx)
{
    const a2600_control *ctl = &g_lay[idx];
    const gboolean lit = g_press.idx == idx;
    vgrad(cctx, 0, ctl->h, lit ? 0x3A2A1E : 0x2A2018, lit ? 0x24190F : 0x18110B);
    rrect(cctx, 0, 0, ctl->w, ctl->h, 0.22);
    cairo_fill_preserve(cctx);
    rgb(cctx, lit ? COL_HELD : 0xC7C7C7);
    cairo_set_line_width(cctx, lit ? 0.1 : 0.05);
    cairo_stroke(cctx);
    txt(cctx, widget, "STOP", 0.55, TRUE, FALSE, lit ? COL_HELD : 0xC7C7C7, 0, 0, ctl->w, ctl->h, 0);
}

/* ---- drawing: bindings and Map -------------------------------------------------- */

/* A part's key as a small dark label under it (Show bindings, Map mode). */
static void draw_binding(cairo_t *cctx, GtkWidget *widget, int idx, double xoff, double yoff,
                         int align)
{
    const a2600_control *ctl = &g_lay[idx];
    char text[40];
    double wid;
    bound_text(ctl->target, text, sizeof text);
    if (!text[0]) return;
    wid = 0.34 * (g_utf8_strlen(text, -1) + 1.2);
    if (wid < 0.8) wid = 0.8;
    {
        const double xpos = xoff + (align < 0 ? ctl->w - wid : align > 0 ? 0 : (ctl->w - wid) / 2);
        const gboolean picked = ctl->target == g_map_state;
        rrect(cctx, xpos, ctl->h + yoff, wid, 0.55, 0.14);
        rgba(cctx, 0x000000, 0.78);
        cairo_fill_preserve(cctx);
        rgb(cctx, picked ? COL_HELD : 0x555555);
        cairo_set_line_width(cctx, picked ? 0.08 : 0.03);
        cairo_stroke(cctx);
        txt(cctx, widget, text, 0.36, TRUE, FALSE, 0xF2F2F2, xpos, ctl->h + yoff, wid, 0.55, 0);
    }
}

/* The four quarters of the stick: its four direction keys round the ring. */
static void draw_stick_bindings(cairo_t *cctx, GtkWidget *widget, const a2600_control *ctl)
{
    static const struct { int act; double dx, dy; } k_dir[4] = {
        { A2600_ACT_JOY_UP, 0, -2.45 }, { A2600_ACT_JOY_DOWN, 0, 2.45 },
        { A2600_ACT_JOY_LEFT, -2.45, 0 }, { A2600_ACT_JOY_RIGHT, 2.45, 0 },
    };
    int dir;
    for (dir = 0; dir < 4; dir++) {
        const int target = A2600_TARGET_PORT(ctl->port, k_dir[dir].act);
        const gboolean picked = target == g_map_state;
        char text[40];
        double wid;
        bound_text(target, text, sizeof text);
        if (!text[0]) continue;
        wid = 0.34 * (g_utf8_strlen(text, -1) + 1.2);
        if (wid < 0.8) wid = 0.8;
        {
            const double xpos = ctl->w / 2 + k_dir[dir].dx - wid / 2;
            const double ypos = ctl->h / 2 + k_dir[dir].dy - 0.27;
            rrect(cctx, xpos, ypos, wid, 0.55, 0.14);
            rgba(cctx, 0x000000, 0.8);
            cairo_fill_preserve(cctx);
            rgb(cctx, picked ? COL_HELD : 0x555555);
            cairo_set_line_width(cctx, picked ? 0.08 : 0.03);
            cairo_stroke(cctx);
            txt(cctx, widget, text, 0.36, TRUE, FALSE, 0xF2F2F2, xpos, ypos, wid, 0.55, 0);
        }
    }
}

/* ---- drawing: the whole window ---------------------------------------------------- */

static void draw_part(cairo_t *cctx, GtkWidget *widget, int idx)
{
    const a2600_control *ctl = &g_lay[idx];

    cairo_save(cctx);
    cairo_translate(cctx, part_x(ctl), part_y(ctl));
    cairo_scale(cctx, UNIT, UNIT);
    switch (ctl->kind) {
    case A2600_CTL_CAPTION:
        txt(cctx, widget, ctl->label, 0.62, TRUE, FALSE, 0xD9D2C4, 0, 0, ctl->w, ctl->h, 0);
        break;
    case A2600_CTL_BODY:
        if (!strcmp(ctl->label, "Joystick")) draw_joy_case(cctx, widget, ctl->port);
        else if (!strcmp(ctl->label, "Paddle A") || !strcmp(ctl->label, "Paddle B"))
            draw_paddle_case(cctx, widget, FALSE);
        else if (!strcmp(ctl->label, "Driving")) draw_paddle_case(cctx, widget, TRUE);
        else if (!strcmp(ctl->label, "Keypad")) draw_keypad_case(cctx, widget);
        else if (!strcmp(ctl->label, "Console")) draw_console_panel(cctx, widget);
        else if (!strcmp(ctl->label, "Cartridge slot")) draw_cart_slot(cctx, ctl);
        else if (!strcmp(ctl->label, "Wood")) draw_wood(cctx, widget, ctl);
        break;
    case A2600_CTL_STICK:
        draw_stick(cctx, ctl);
        break;
    case A2600_CTL_ROUND_BUTTON:
        if (!strcmp(ctl->label, "FIRE")) draw_fire(cctx, idx);
        else draw_key(cctx, widget, idx);
        break;
    case A2600_CTL_KNOB:
    case A2600_CTL_WHEEL:
        draw_knob(cctx, idx);
        break;
    case A2600_CTL_TURN:
        draw_turn(cctx, idx);
        break;
    case A2600_CTL_BUTTON:
        if (ctl->port == -1 && !strcmp(ctl->label, "STOP")) draw_stop(cctx, widget, idx);
        else if (ctl->port == -1) draw_switch(cctx, widget, idx);
        else draw_edge_button(cctx, idx);
        break;
    case A2600_CTL_SWITCH:
        draw_switch(cctx, widget, idx);
        break;
    default:
        break;
    }
    cairo_restore(cctx);
}

/* The keys bound to the controls (Show bindings, Map mode), drawn after every
 * control so a label is never hidden under the next row of keys. */
static void draw_bindings_pass(cairo_t *cctx, GtkWidget *widget)
{
    int idx;
    if (!showing_bindings()) return;
    for (idx = 0; idx < g_n; idx++) {
        const a2600_control *ctl = &g_lay[idx];
        const gboolean dim = dimmed(idx);
        if (ctl->kind == A2600_CTL_CAPTION || ctl->kind == A2600_CTL_BODY ||
            ctl->kind == A2600_CTL_KNOB || ctl->kind == A2600_CTL_WHEEL)
            continue;
        if (dim) cairo_push_group(cctx);
        cairo_save(cctx);
        cairo_translate(cctx, part_x(ctl), part_y(ctl));
        cairo_scale(cctx, UNIT, UNIT);
        if (ctl->kind == A2600_CTL_STICK)
            draw_stick_bindings(cctx, widget, ctl);
        else if (ctl->kind == A2600_CTL_TURN)
            draw_binding(cctx, widget, idx, 0, 0.05, ctl->label[0] == '<' || !strcmp(ctl->label, "CCW") ? -1 : 1);
        else
            draw_binding(cctx, widget, idx, 0, ctl->port == -1 ? 0.0 : 0.05, 0);
        cairo_restore(cctx);
        if (dim) {
            cairo_pop_group_to_source(cctx);
            cairo_paint_with_alpha(cctx, 0.5);
        }
    }
}

/* The hover and the Map pick: an outline round the control under the pointer
 * (Map mode only: otherwise the control already shows what it does). */
static void draw_outline(cairo_t *cctx, int idx, gboolean picked)
{
    const a2600_control *ctl = &g_lay[idx];
    cairo_save(cctx);
    cairo_translate(cctx, part_x(ctl), part_y(ctl));
    cairo_scale(cctx, UNIT, UNIT);
    if (ctl->kind == A2600_CTL_STICK)
        circle(cctx, ctl->w / 2, ctl->h / 2, ctl->w / 2);
    else if (ctl->kind == A2600_CTL_ROUND_BUTTON)
        circle(cctx, ctl->w / 2, ctl->h / 2, ctl->w / 2);
    else
        rrect(cctx, -0.1, -0.1, ctl->w + 0.2, ctl->h + 0.2, 0.25);
    rgb(cctx, picked ? COL_HELD : 0xFFFFFF);
    cairo_set_line_width(cctx, picked ? 0.14 : 0.08);
    cairo_stroke(cctx);
    cairo_restore(cctx);
}

static void draw_area(GtkDrawingArea *area, cairo_t *cctx, int wid, int hgt, gpointer udata)
{
    GtkWidget *widget = GTK_WIDGET(area);
    cairo_pattern_t *grad = cairo_pattern_create_linear(0, 0, 0, hgt);
    int idx;
    (void)udata;

    /* the desk the controllers lie on */
    stop_rgb(grad, 0, COL_DESK_TOP);
    stop_rgb(grad, 1, COL_DESK_BOT);
    rrect(cctx, 0.5, 0.5, wid - 1, hgt - 1, 14);
    cairo_set_source(cctx, grad);
    cairo_fill_preserve(cctx);
    cairo_pattern_destroy(grad);
    rgb(cctx, COL_DESK_EDGE);
    cairo_set_line_width(cctx, 1);
    cairo_stroke(cctx);

    if (!g_lay)
        return;
    for (idx = 0; idx < g_n; idx++) {
        const gboolean dim = dimmed(idx);
        if (dim) cairo_push_group(cctx);
        draw_part(cctx, widget, idx);
        if (dim) {
            cairo_pop_group_to_source(cctx);
            cairo_paint_with_alpha(cctx, 0.38);
        }
    }
    draw_bindings_pass(cctx, widget);
    if (g_map_state != -2) {
        /* armed: the pickable control under the pointer, and the picked one */
        if (g_hover.idx >= 0 && g_hover.target >= 0 && g_hover.target != g_map_state)
            draw_outline(cctx, g_hover.idx, FALSE);
        for (idx = 0; idx < g_n; idx++) {
            if (g_map_state >= 0 && g_lay[idx].target == g_map_state &&
                g_lay[idx].kind != A2600_CTL_STICK)
                draw_outline(cctx, idx, TRUE);
        }
    }
}

/* ---- pointer ---------------------------------------------------------------------- */

/* Lets go of everything the mouse holds on the controllers. */
static void mouse_release(void)
{
    const gboolean any = g_drag.kind || g_press.idx >= 0;
    if (g_drag.kind == 1)
        a2600session_pointer_set(g_session, g_drag.port, 0);
    else if (g_drag.kind == 2)
        a2600session_pointer_analog(g_session, g_drag.port, g_drag.paddle, 0, 0);
    if (g_press.idx >= 0 && g_press.idx < g_n && !g_drag.kind) {
        const int target = g_lay[g_press.idx].target;
        if (target >= 0 && !is_sysact(target) && g_lay[g_press.idx].kind != A2600_CTL_SWITCH)
            a2600session_press(g_session, target, 0);
    }
    g_drag.kind = 0;
    g_press.idx = g_press.target = -1;
    if (any && g_area)
        gtk_widget_queue_draw(g_area);
}

static void release_all_holds(void)
{
    mouse_release();
    a2600session_release_all(g_session);
}

static void stop_capture(void)
{
    if (g_capture_timer) {
        g_source_remove(g_capture_timer);
        g_capture_timer = 0;
    }
    a2600session_gamepad_capture_cancel(g_session);
}

static void say_bound(const char *what, const char *stolen)
{
    char msg[260];
    if (stolen && stolen[0])
        g_snprintf(msg, sizeof msg, "Bound %s (was %s)", what, stolen);
    else
        g_snprintf(msg, sizeof msg, "Bound %s", what);
    gtk_label_set_text(GTK_LABEL(g_map_hint), msg);
}

static void set_map_state(int state);

static gboolean capture_tick(gpointer udata)
{
    int button;
    (void)udata;
    if (g_map_state < 0) { g_capture_timer = 0; return G_SOURCE_REMOVE; }
    if (a2600session_gamepad_capture_poll(g_session, &button)) {
        char stolen[128];
        stolen[0] = '\0';
        a2600session_binding_set_button(g_session, g_map_state, button, stolen, sizeof stolen);
        g_capture_timer = 0;
        set_map_state(-1);     /* stay armed: remapping several in a row is normal */
        say_bound(a2600_pad_button_name(button), stolen);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static void set_map_state(int state)
{
    g_map_state = state;
    gtk_widget_set_sensitive(g_clear_button, state >= 0);
    if (state == -2) {
        stop_capture();
        gtk_button_set_label(GTK_BUTTON(g_map_button), "Map");
        gtk_widget_remove_css_class(g_map_button, "a2600-accent");
        gtk_label_set_text(GTK_LABEL(g_map_hint),
                           "Hold a button, drag the stick or a knob, flip a switch. Map rebinds one.");
    } else if (state == -1) {
        stop_capture();
        gtk_button_set_label(GTK_BUTTON(g_map_button), "Done");
        gtk_widget_add_css_class(g_map_button, "a2600-accent");
        gtk_label_set_text(GTK_LABEL(g_map_hint),
                           "Click a control to remap it (a quarter of the stick is a direction), or pick one from the list");
    } else {
        char msg[200];
        mouse_release();
        g_snprintf(msg, sizeof msg, "Press a key or a gamepad button for %s (Esc cancels)",
                   a2600_target_name(state));
        gtk_label_set_text(GTK_LABEL(g_map_hint), msg);
        a2600session_gamepad_capture_begin(g_session);
        if (!g_capture_timer)
            g_capture_timer = g_timeout_add(50, capture_tick, NULL);
    }
    refresh_all();
}

static void run_sysaction(int sysact)
{
    if (g_parent)
        a2600_window_run_sysaction(A2600_WINDOW(g_parent), sysact);
    else
        a2600session_sysaction(g_session, sysact);
}

/* The stick under a drag: the pointer's offset from the ring's centre picks the
 * direction (eight sectors); the cap follows what the machine sees held. */
static void stick_follow(double xpos, double ypos)
{
    a2600session_pointer_set(g_session, g_drag.port,
        a2600session_stick_from_vector((float)(xpos - g_drag.cenx), (float)(ypos - g_drag.ceny),
                                       (float)g_drag.rad));
    gtk_widget_queue_draw(g_area);
}

/* The knob under a drag: the pointer's angle from the dial's centre. */
static void knob_follow(double xpos, double ypos)
{
    a2600session_pointer_analog(g_session, g_drag.port, g_drag.paddle,
        a2600session_knob_from_vector((float)(xpos - g_drag.cenx), (float)(ypos - g_drag.ceny)), 1);
    gtk_widget_queue_draw(g_area);
}

/* The driving wheel turns as the pointer circles it: a twist pulse per notch. */
static void wheel_follow(double xpos, double ypos)
{
    const double dx = xpos - g_drag.cenx, dy = ypos - g_drag.ceny;
    double ang, delta;
    if (dx * dx + dy * dy < (0.25 * g_drag.rad) * (0.25 * g_drag.rad))
        return;             /* too near the hub to have an angle */
    ang = a2600session_vector_angle((float)dx, (float)dy);
    delta = ang - g_drag.lastang;
    if (delta > 180) delta -= 360;
    if (delta < -180) delta += 360;
    g_drag.lastang = ang;
    g_drag.carry += delta;
    g_wheel[g_drag.port] += delta;
    while (g_drag.carry >= A2600_WHEEL_STEP_DEG) {
        a2600session_twist_pulse(g_session, g_drag.port, 1);
        g_drag.carry -= A2600_WHEEL_STEP_DEG;
    }
    while (g_drag.carry <= -A2600_WHEEL_STEP_DEG) {
        a2600session_twist_pulse(g_session, g_drag.port, -1);
        g_drag.carry += A2600_WHEEL_STEP_DEG;
    }
    gtk_widget_queue_draw(g_area);
}

static void drag_follow(double xpos, double ypos)
{
    if (g_drag.kind == 1) stick_follow(xpos, ypos);
    else if (g_drag.kind == 2) knob_follow(xpos, ypos);
    else if (g_drag.kind == 3) wheel_follow(xpos, ypos);
}

static void on_area_begin(GtkGestureDrag *gest, double xpos, double ypos, gpointer udata)
{
    const guint button = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gest));
    const Hit hit = hit_at(xpos, ypos);
    const a2600_control *ctl;
    (void)udata;

    if (button != GDK_BUTTON_PRIMARY || hit.idx < 0)
        return;
    ctl = &g_lay[hit.idx];
    if (g_map_state != -2) {
        /* armed: picks (or changes) the target; a knob, the wheel and the bodies are no target */
        if (hit.target >= 0)
            set_map_state(hit.target);
        return;
    }
    mouse_release();
    g_press = hit;
    switch (ctl->kind) {
    case A2600_CTL_STICK:
    case A2600_CTL_KNOB:
    case A2600_CTL_WHEEL:
        g_drag.kind = ctl->kind == A2600_CTL_STICK ? 1 : ctl->kind == A2600_CTL_KNOB ? 2 : 3;
        g_drag.port = ctl->port;
        g_drag.paddle = ctl->paddle;
        g_drag.cenx = part_x(ctl) + U(ctl->w) / 2;
        g_drag.ceny = part_y(ctl) + U(ctl->h) / 2;
        g_drag.rad = U(ctl->w) / 2;
        g_drag.carry = 0;
        g_drag.lastang = 0;
        if (g_drag.kind == 3) {
            const double dx = xpos - g_drag.cenx, dy = ypos - g_drag.ceny;
            g_drag.lastang = a2600session_vector_angle((float)dx, (float)dy);
        }
        if (g_drag.kind != 3)
            drag_follow(xpos, ypos);
        break;
    case A2600_CTL_SWITCH:
        /* a latching switch flips on the press */
        a2600session_press(g_session, ctl->target, 1);
        a2600session_press(g_session, ctl->target, 0);
        break;
    case A2600_CTL_BUTTON:
    case A2600_CTL_ROUND_BUTTON:
    case A2600_CTL_TURN:
        /* POWER and STOP are system actions: they fire on release, like a real
         * button, so pressing and dragging off does not reboot the console */
        if (ctl->target >= 0 && !is_sysact(ctl->target))
            a2600session_press(g_session, ctl->target, 1);
        break;
    default:
        g_press.idx = g_press.target = -1;
        return;
    }
    gtk_widget_queue_draw(g_area);
}

static void on_area_update(GtkGestureDrag *gest, double offx, double offy, gpointer udata)
{
    double startx, starty;
    (void)udata;
    if (!g_drag.kind) return;
    gtk_gesture_drag_get_start_point(gest, &startx, &starty);
    drag_follow(startx + offx, starty + offy);
}

static void on_area_end(GtkGestureDrag *gest, double offx, double offy, gpointer udata)
{
    const Hit was = g_press;
    double startx, starty;
    (void)udata;
    if (was.idx >= 0 && !g_drag.kind && was.target >= 0 && is_sysact(was.target)) {
        gtk_gesture_drag_get_start_point(gest, &startx, &starty);
        {
            const Hit now = hit_at(startx + offx, starty + offy);
            g_press.idx = -1;
            if (now.idx == was.idx)
                run_sysaction(was.target - A2600_TARGET_SYSACT(0));
        }
    }
    if (was.idx >= 0 || g_drag.kind)
        mouse_release();
}

static void on_area_cancel(GtkGesture *gest, GdkEventSequence *seq, gpointer udata)
{
    (void)gest; (void)seq; (void)udata;
    mouse_release();
}

/* Dragging off a held button or key lets it go. */
static void on_area_motion(GtkEventControllerMotion *ctrl, double xpos, double ypos, gpointer udata)
{
    const Hit hit = hit_at(xpos, ypos);
    (void)ctrl; (void)udata;
    g_ptr_x = xpos;
    g_ptr_y = ypos;
    if (g_press.idx >= 0 && !g_drag.kind && !same_hit(&hit, &g_press))
        mouse_release();
    if (!same_hit(&hit, &g_hover)) {
        g_hover = hit;
        gtk_widget_queue_draw(g_area);
    }
}

static void on_area_leave(GtkEventControllerMotion *ctrl, gpointer udata)
{
    (void)ctrl; (void)udata;
    if (g_press.idx >= 0 && !g_drag.kind)
        mouse_release();
    if (g_hover.idx >= 0) {
        g_hover.idx = g_hover.target = -1;
        gtk_widget_queue_draw(g_area);
    }
}

/* The wheel over a knob nudges it; over the driving wheel it notches it. */
static gboolean on_area_scroll(GtkEventControllerScroll *ctrl, double delx, double dely,
                               gpointer udata)
{
    Hit hit;
    const a2600_control *ctl;
    (void)ctrl; (void)delx; (void)udata;
    if (g_map_state != -2 || g_drag.kind)
        return FALSE;
    hit = hit_at(g_ptr_x, g_ptr_y);
    if (hit.idx < 0) return FALSE;
    ctl = &g_lay[hit.idx];
    if (ctl->kind == A2600_CTL_KNOB || (ctl->kind == A2600_CTL_TURN && ctl->paddle >= 0)) {
        const int pad = ctl->paddle == 1 ? 1 : 0;
        int val = a2600session_analog_get(g_session, ctl->port, pad) + (dely < 0 ? WHEEL_NUDGE : -WHEEL_NUDGE);
        if (val > 32767) val = 32767;
        if (val < -32767) val = -32767;
        a2600session_pointer_analog(g_session, ctl->port, pad, val, 1);
        a2600session_pointer_analog(g_session, ctl->port, pad, val, 0);   /* the position stays */
    } else if (ctl->kind == A2600_CTL_WHEEL || (ctl->kind == A2600_CTL_TURN && ctl->paddle < 0)) {
        a2600session_twist_pulse(g_session, ctl->port, dely < 0 ? 1 : -1);
        g_wheel[ctl->port] += dely < 0 ? A2600_WHEEL_STEP_DEG : -A2600_WHEEL_STEP_DEG;
    } else {
        return FALSE;
    }
    gtk_widget_queue_draw(g_area);
    return TRUE;
}

static void binding_tip(int target, char *text, int textsz)
{
    char key[32];
    const a2600_binding bind = a2600session_binding_get(g_session, target);
    if (bind.keysym) a2600session_keysym_name(bind.keysym, key, sizeof key);
    else g_snprintf(key, sizeof key, "none");
    g_snprintf(text, textsz, "%s\nKey: %s    Gamepad: %s", a2600_target_name(target), key,
               bind.button >= 0 ? a2600_pad_button_name(bind.button) : "none");
}

static gboolean on_area_tooltip(GtkWidget *widget, int xpos, int ypos, gboolean kbmode,
                                GtkTooltip *tip, gpointer udata)
{
    Hit hit;
    const a2600_control *ctl;
    char text[300];
    (void)widget; (void)udata;
    if (kbmode) return FALSE;
    hit = hit_at(xpos, ypos);
    if (hit.idx < 0) return FALSE;
    ctl = &g_lay[hit.idx];
    switch (ctl->kind) {
    case A2600_CTL_STICK:
        g_snprintf(text, sizeof text, "Press and drag to tilt the stick\nThis quarter: ");
        binding_tip(hit.target, text + strlen(text), (int)(sizeof text - strlen(text)));
        break;
    case A2600_CTL_KNOB:
        g_snprintf(text, sizeof text, "Drag to turn the knob, scroll to nudge it\n(Map: its turn arrows hold the keys)");
        break;
    case A2600_CTL_WHEEL:
        g_snprintf(text, sizeof text, "Drag round the wheel to turn it, or scroll\n(Map: its CCW / CW arrows hold the keys)");
        break;
    case A2600_CTL_TURN:
        g_snprintf(text, sizeof text, "Turns while held\n");
        binding_tip(hit.target, text + strlen(text), (int)(sizeof text - strlen(text)));
        break;
    case A2600_CTL_BUTTON:
    case A2600_CTL_ROUND_BUTTON:
    case A2600_CTL_SWITCH:
        if (hit.target < 0) return FALSE;
        if (hit.target == A2600_TARGET_SYSACT(A2600_SYSACT_REBOOT_CONFIG))
            g_snprintf(text, sizeof text, "Power: power-cycles the console back to the FujiNet CONFIG\n");
        else if (hit.target == A2600_TARGET_SYSACT(A2600_SYSACT_PAUSE))
            g_snprintf(text, sizeof text, "Stop: stops the machine in the debugger\n");
        else
            text[0] = '\0';
        binding_tip(hit.target, text + strlen(text), (int)(sizeof text - strlen(text)));
        break;
    default:
        return FALSE;
    }
    gtk_tooltip_set_text(tip, text);
    return TRUE;
}

/* ---- refresh ---------------------------------------------------------------------- */

static void refresh_all(void)
{
    if (g_area) gtk_widget_queue_draw(g_area);
}

/* Every frame while the window is on screen: what the machine sees held, painted
 * in the held orange, the knobs' and the stick's positions and the console
 * switches. Redraws only when something moved. */
static gboolean frame_tick(GtkWidget *widget, GdkFrameClock *clock, gpointer udata)
{
    gboolean changed = FALSE;
    int port, sw, pad;
    (void)clock; (void)udata;

    if (reload_layout()) {
        mouse_release();
        changed = TRUE;
    }
    for (port = 0; port < 2; port++) {
        const unsigned held = a2600session_buttons_held(g_session, port);
        if (held != g_held[port]) {
            g_held[port] = held;
            changed = TRUE;
        }
        for (pad = 0; pad < 2; pad++) {
            const int val = a2600session_analog_get(g_session, port, pad);
            if (val != g_analog[port][pad]) {
                g_analog[port][pad] = val;
                changed = TRUE;
            }
        }
        /* held CCW / CW keeps turning the driving wheel */
        if (((held >> A2600_ACT_DRIVE_CCW) & 1u) != ((held >> A2600_ACT_DRIVE_CW) & 1u)) {
            g_wheel[port] += ((held >> A2600_ACT_DRIVE_CW) & 1u) ? 2.5 : -2.5;
            changed = TRUE;
        }
    }
    for (sw = 0; sw < A2600_SW_COUNT; sw++) {
        const int val = a2600session_switch_held(g_session, sw);
        if (val != g_sw[sw]) {
            g_sw[sw] = val;
            changed = TRUE;
        }
    }
    if (changed)
        gtk_widget_queue_draw(widget);
    return G_SOURCE_CONTINUE;
}

/* ---- gamepads ------------------------------------------------------------- */

static void refresh_pad_ports(void)
{
    int idx;
    for (idx = 0; idx < g_pad_row_count; idx++) {
        const int eff = a2600session_gamepad_effective_port(g_session, idx);
        const char *text = eff == 0 ? "plays the left port" : eff == 1 ? "plays the right port"
                                                                      : "plays nothing";
        if (strcmp(gtk_label_get_text(GTK_LABEL(g_pad_eff[idx])), text) != 0)
            gtk_label_set_text(GTK_LABEL(g_pad_eff[idx]), text);
    }
}

static void on_assign_changed(GObject *obj, GParamSpec *pspec, gpointer udata)
{
    const int idx = GPOINTER_TO_INT(udata);
    const guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(obj));
    (void)pspec;
    if (g_syncing) return;
    /* 0 = automatic (connection order), then the left and the right port */
    a2600session_gamepad_assign(g_session, idx, sel == 0 ? -1 : (int)sel - 1);
    refresh_pad_ports();
}

/* Rebuilt whenever a pad comes or goes (the session's generation counter): one
 * row per pad, its name, what it plays and the port it is set to. The area is
 * PAD_ROWS rows tall whatever is plugged in (more pads scroll), so the window
 * never changes size. */
static void refresh_pads(void)
{
    static const char *const choices[] = { "Automatic", "Left port", "Right port", NULL };
    GtkWidget *child;
    int count, idx;

    g_syncing = TRUE;
    while ((child = gtk_widget_get_first_child(g_pad_box)) != NULL)
        gtk_box_remove(GTK_BOX(g_pad_box), child);
    g_pad_row_count = 0;

    count = a2600session_gamepad_count(g_session);
    if (count == 0) {
        GtkWidget *none = gtk_label_new("No gamepads connected - plug one in");
        gtk_widget_add_css_class(none, "dim-label");
        gtk_label_set_xalign(GTK_LABEL(none), 0);
        gtk_widget_set_size_request(none, -1, PAD_ROW_H);
        gtk_widget_set_tooltip_text(none, "A gamepad is picked up as it appears");
        gtk_box_append(GTK_BOX(g_pad_box), none);
        g_syncing = FALSE;
        return;
    }
    for (idx = 0; idx < count && idx < MAX_PADS; idx++) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *name, *drop, *eff;
        char text[128];
        const int assign = a2600session_gamepad_assignment(g_session, idx);

        a2600session_gamepad_name(g_session, idx, text, sizeof text);
        gtk_widget_set_size_request(row, -1, PAD_ROW_H);
        name = gtk_label_new(text);
        gtk_widget_set_hexpand(name, TRUE);
        gtk_label_set_xalign(GTK_LABEL(name), 0);
        gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(name), 1);
        gtk_widget_set_tooltip_text(name, text);
        eff = gtk_label_new("");
        gtk_widget_add_css_class(eff, "dim-label");
        drop = gtk_drop_down_new_from_strings(choices);
        gtk_drop_down_set_selected(GTK_DROP_DOWN(drop), assign < 0 || assign > 1 ? 0 : (guint)assign + 1);
        gtk_widget_set_focus_on_click(drop, FALSE);
        gtk_widget_set_valign(drop, GTK_ALIGN_CENTER);
        g_signal_connect(drop, "notify::selected", G_CALLBACK(on_assign_changed),
                         GINT_TO_POINTER(idx));
        gtk_box_append(GTK_BOX(row), name);
        gtk_box_append(GTK_BOX(row), eff);
        gtk_box_append(GTK_BOX(row), drop);
        gtk_box_append(GTK_BOX(g_pad_box), row);
        g_pad_eff[g_pad_row_count++] = eff;
    }
    g_syncing = FALSE;
    refresh_pad_ports();
}

/* Keeps the port-type choices in step with the session (the Preferences page and
 * a settings file move them too). */
static void sync_widgets(void)
{
    int port;
    g_syncing = TRUE;
    for (port = 0; port < 2; port++) {
        const guint want = (guint)a2600session_port_type(g_session, port);
        if (gtk_drop_down_get_selected(GTK_DROP_DOWN(g_port_drop[port])) != want)
            gtk_drop_down_set_selected(GTK_DROP_DOWN(g_port_drop[port]), want);
    }
    g_syncing = FALSE;
}

/* The pad rows (hot plug, automatic assignment changes) and the port types. */
static gboolean info_tick(gpointer udata)
{
    unsigned gen;
    (void)udata;
    if (!g_window || !gtk_widget_get_visible(g_window)) return G_SOURCE_CONTINUE;

    sync_widgets();
    gen = a2600session_gamepad_generation(g_session);
    if (gen != g_pad_generation) {
        g_pad_generation = gen;
        refresh_pads();
    } else {
        refresh_pad_ports();
    }
    return G_SOURCE_CONTINUE;
}

/* ---- keyboard ------------------------------------------------------------- */

gboolean a2600_controllers_map_key(guint keyval, guint code, GdkModifierType mods, gboolean down)
{
    guint32 keysym;
    char name[32], stolen[128];

    if (!g_window || g_map_state == -2)
        return FALSE;
    if (keyval == GDK_KEY_F9 || keyval == GDK_KEY_F11 || keyval == GDK_KEY_F12 ||
        (mods & GDK_CONTROL_MASK))
        return FALSE;
    if (g_swallow_code && code == g_swallow_code) {
        if (!down) g_swallow_code = 0;
        return TRUE;
    }
    if (!down)
        return TRUE;
    if (keyval == GDK_KEY_Escape) {
        g_swallow_code = code;
        set_map_state(g_map_state >= 0 ? -1 : -2);
        return TRUE;
    }
    if (g_map_state < 0)
        return TRUE;              /* armed, no target yet: typing is not typed */
    keysym = code >= 8 ? a2600session_keysym_from_evdev(code - 8) : 0;
    if (!keysym) keysym = keyval;
    if (a2600session_key_reserved(keysym)) {
        gtk_label_set_text(GTK_LABEL(g_map_hint), "F9, F11 and F12 belong to the app: they cannot be bound");
        return TRUE;
    }
    stolen[0] = '\0';
    a2600session_binding_set_key(g_session, g_map_state, keysym, stolen, sizeof stolen);
    a2600session_keysym_name(keysym, name, sizeof name);
    g_swallow_code = code;
    set_map_state(-1);            /* stay armed: remapping several in a row is normal */
    say_bound(name, stolen);
    return TRUE;
}

static gboolean on_key(guint keyval, guint code, GdkModifierType mods, gboolean down)
{
    /* an open popover (a drop-down's list) has the arrows and Return */
    GtkWidget *focus = gtk_root_get_focus(GTK_ROOT(g_window));
    if (focus && gtk_widget_get_ancestor(focus, GTK_TYPE_POPOVER))
        return FALSE;
    /* Map first, then exactly what the main window does with it. */
    if (!g_parent) return a2600_controllers_map_key(keyval, code, mods, down);
    return a2600_window_key(A2600_WINDOW(g_parent), keyval, code, mods, down);
}

static gboolean on_key_pressed(GtkEventControllerKey *ctrl, guint keyval,
                               guint code, GdkModifierType mods, gpointer udata)
{
    (void)ctrl; (void)udata;
    return on_key(keyval, code, mods, TRUE);
}

static void on_key_released(GtkEventControllerKey *ctrl, guint keyval,
                            guint code, GdkModifierType mods, gpointer udata)
{
    (void)ctrl; (void)udata;
    on_key(keyval, code, mods, FALSE);
}

/* Keys typed here went through the main window, so a key held into a focus
 * change must be let go there too (and the mouse's, here). */
static void on_focus_leave(GtkEventControllerFocus *ctrl, gpointer udata)
{
    (void)ctrl; (void)udata;
    mouse_release();
    if (g_parent)
        a2600_window_release_keys(A2600_WINDOW(g_parent));
    g_swallow_code = 0;     /* its release went elsewhere */
}

static void on_active_changed(GObject *obj, GParamSpec *pspec, gpointer udata)
{
    (void)pspec; (void)udata;
    if (!gtk_window_is_active(GTK_WINDOW(obj)))
        mouse_release();
}

/* ---- the window ----------------------------------------------------------- */

static void on_map_clicked(GtkButton *button, gpointer udata)
{
    (void)button; (void)udata;
    set_map_state(g_map_state == -2 ? -1 : -2);
}

static void on_defaults_clicked(GtkButton *button, gpointer udata)
{
    (void)button; (void)udata;
    a2600session_bindings_reset(g_session);
    gtk_label_set_text(GTK_LABEL(g_map_hint), "Bindings back to their defaults");
    refresh_all();
}

/* Takes the picked control's key and gamepad button away. */
static void on_clear_clicked(GtkButton *button, gpointer udata)
{
    (void)button; (void)udata;
    if (g_map_state < 0) return;
    a2600session_binding_clear(g_session, g_map_state);
    gtk_label_set_text(GTK_LABEL(g_map_hint),
                       "Cleared. Press a key or a gamepad button to bind it again");
    refresh_all();
}

static void on_show_bindings(GtkCheckButton *button, gpointer udata)
{
    (void)udata;
    a2600session_set_int(g_session, "controllers_show_bindings",
                         gtk_check_button_get_active(button) ? 1 : 0);
    refresh_all();
}

/* "Choose a control...": any of the 59 targets, for the ones the picture cannot
 * show (the knobs' DEC / INC keys under their arrows are tiny). */
static void on_target_picked(GObject *obj, GParamSpec *pspec, gpointer udata)
{
    const guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(obj));
    (void)pspec; (void)udata;
    if (g_syncing || sel == 0) return;
    g_syncing = TRUE;
    gtk_drop_down_set_selected(GTK_DROP_DOWN(obj), 0);
    g_syncing = FALSE;
    set_map_state((int)sel - 1);
}

static void on_port_type_changed(GObject *obj, GParamSpec *pspec, gpointer udata)
{
    const int port = GPOINTER_TO_INT(udata);
    (void)pspec;
    if (g_syncing) return;
    mouse_release();
    a2600session_set_port_type(g_session, port, (int)gtk_drop_down_get_selected(GTK_DROP_DOWN(obj)));
    refresh_all();
}

static void hide_window(void)
{
    set_map_state(-2);
    release_all_holds();
    g_swallow_code = 0;
    gtk_widget_set_visible(g_window, FALSE);
}

static gboolean on_close(GtkWindow *window, gpointer udata)
{
    (void)window; (void)udata;
    hide_window();
    return TRUE;
}

/* The pad line's timer runs only while the window is on screen: closing only
 * hides it, and a hidden window has nobody to update. (The per-frame tick
 * stops by itself while the drawing is unmapped.) */
static void on_window_map(GtkWidget *widget, gpointer udata)
{
    (void)widget; (void)udata;
    if (!g_info_timer) g_info_timer = g_timeout_add(250, info_tick, NULL);
    info_tick(NULL);
}

static void on_window_unmap(GtkWidget *widget, gpointer udata)
{
    (void)widget; (void)udata;
    if (g_info_timer) g_source_remove(g_info_timer);
    g_info_timer = 0;
}

static void on_destroy(GtkWidget *widget, gpointer udata)
{
    (void)widget; (void)udata;
    if (g_capture_timer) g_source_remove(g_capture_timer);
    if (g_info_timer) g_source_remove(g_info_timer);
    g_capture_timer = g_info_timer = 0;
    g_area = NULL;
    g_window = NULL;
    g_pad_box = NULL;
    g_pad_row_count = 0;
    g_map_state = -2;
    g_hover.idx = g_press.idx = -1;
    g_drag.kind = 0;
}

static void build_window(GtkWindow *parent)
{
    GtkWidget *root, *portrow, *maprow, *toolbar, *header, *defaults, *padscroll, *label;
    GtkEventController *keys, *focus, *motion, *scroll;
    GtkGesture *drag;
    GtkStringList *targets;
    int port, target;

    a2600_install_accent_css();
    g_lay = NULL;
    g_type[0] = g_type[1] = -1;
    reload_layout();
    memset(g_held, 0, sizeof g_held);
    memset(g_sw, 0, sizeof g_sw);
    memset(g_analog, 0, sizeof g_analog);
    memset(g_wheel, 0, sizeof g_wheel);
    g_hover.idx = g_press.idx = -1;
    g_drag.kind = 0;

    g_window = adw_window_new();
    gtk_window_set_title(GTK_WINDOW(g_window), "Controllers");
    if (parent) {
        gtk_window_set_transient_for(GTK_WINDOW(g_window), parent);
        /* Same app-id as the main window, so the shell groups and icons it. */
        gtk_window_set_application(GTK_WINDOW(g_window), gtk_window_get_application(parent));
    }
    gtk_window_set_destroy_with_parent(GTK_WINDOW(g_window), TRUE);
    /* min == max: Wayland compositors float it */
    gtk_window_set_resizable(GTK_WINDOW(g_window), FALSE);
    g_signal_connect(g_window, "close-request", G_CALLBACK(on_close), NULL);
    g_signal_connect(g_window, "destroy", G_CALLBACK(on_destroy), NULL);
    g_signal_connect(g_window, "notify::is-active", G_CALLBACK(on_active_changed), NULL);

    /* the controllers */
    g_area = gtk_drawing_area_new();
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(g_area), area_width());
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(g_area), area_height());
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(g_area), draw_area, NULL, NULL);
    gtk_widget_set_halign(g_area, GTK_ALIGN_CENTER);
    /* Not focusable: clicking a control must not steal focus. */
    gtk_widget_set_focusable(g_area, FALSE);
    gtk_widget_set_has_tooltip(g_area, TRUE);
    g_signal_connect(g_area, "query-tooltip", G_CALLBACK(on_area_tooltip), NULL);
    drag = gtk_gesture_drag_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), 0);
    g_signal_connect(drag, "drag-begin", G_CALLBACK(on_area_begin), NULL);
    g_signal_connect(drag, "drag-update", G_CALLBACK(on_area_update), NULL);
    g_signal_connect(drag, "drag-end", G_CALLBACK(on_area_end), NULL);
    g_signal_connect(drag, "cancel", G_CALLBACK(on_area_cancel), NULL);
    gtk_widget_add_controller(g_area, GTK_EVENT_CONTROLLER(drag));
    motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "motion", G_CALLBACK(on_area_motion), NULL);
    g_signal_connect(motion, "leave", G_CALLBACK(on_area_leave), NULL);
    gtk_widget_add_controller(g_area, motion);
    scroll = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
    g_signal_connect(scroll, "scroll", G_CALLBACK(on_area_scroll), NULL);
    gtk_widget_add_controller(g_area, scroll);
    gtk_widget_add_tick_callback(g_area, frame_tick, NULL, NULL);

    root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_top(root, 12);
    gtk_widget_set_margin_bottom(root, 12);
    gtk_widget_set_margin_start(root, 12);
    gtk_widget_set_margin_end(root, 12);
    gtk_widget_set_size_request(root, area_width(), -1);
    gtk_box_append(GTK_BOX(root), g_area);

    /* what is plugged into each port (Auto follows the cartridge) */
    portrow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    for (port = 0; port < 2; port++) {
        GtkStringList *types = gtk_string_list_new(NULL);
        int type;
        for (type = 0; a2600_ctrl_type_name(type); type++)
            gtk_string_list_append(types, a2600_ctrl_type_name(type));
        label = gtk_label_new(port ? "Right port" : "Left port");
        gtk_widget_add_css_class(label, "dim-label");
        if (port) gtk_widget_set_margin_start(label, 16);
        gtk_box_append(GTK_BOX(portrow), label);
        g_port_drop[port] = gtk_drop_down_new(G_LIST_MODEL(types), NULL);
        gtk_widget_set_focus_on_click(g_port_drop[port], FALSE);
        gtk_widget_set_tooltip_text(g_port_drop[port],
            "What is plugged into this port. Auto follows the cartridge; any change applies at once");
        g_signal_connect(g_port_drop[port], "notify::selected", G_CALLBACK(on_port_type_changed),
                         GINT_TO_POINTER(port));
        gtk_box_append(GTK_BOX(portrow), g_port_drop[port]);
    }
    gtk_box_append(GTK_BOX(root), portrow);

    /* the pad rows: a fixed-height area, so the window never resizes */
    g_pad_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, PAD_GAP);
    padscroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(padscroll), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(padscroll), FALSE);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(padscroll),
        PAD_ROWS * PAD_ROW_H + (PAD_ROWS - 1) * PAD_GAP);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(padscroll),
        PAD_ROWS * PAD_ROW_H + (PAD_ROWS - 1) * PAD_GAP);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(padscroll), g_pad_box);
    gtk_box_append(GTK_BOX(root), padscroll);
    g_pad_generation = a2600session_gamepad_generation(g_session);
    refresh_pads();

    maprow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    g_map_button = gtk_button_new_with_label("Map");
    gtk_widget_set_size_request(g_map_button, 72, -1);
    gtk_widget_set_focus_on_click(g_map_button, FALSE);
    g_signal_connect(g_map_button, "clicked", G_CALLBACK(on_map_clicked), NULL);
    g_clear_button = gtk_button_new_with_label("Clear");
    gtk_widget_set_focus_on_click(g_clear_button, FALSE);
    gtk_widget_set_tooltip_text(g_clear_button, "Take the picked control's key and gamepad button away");
    g_signal_connect(g_clear_button, "clicked", G_CALLBACK(on_clear_clicked), NULL);
    defaults = gtk_button_new_with_label("Defaults");
    gtk_widget_set_focus_on_click(defaults, FALSE);
    g_signal_connect(defaults, "clicked", G_CALLBACK(on_defaults_clicked), NULL);
    g_show_bindings = gtk_check_button_new_with_label("Show bindings");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(g_show_bindings), show_bindings_saved());
    gtk_widget_set_focus_on_click(g_show_bindings, FALSE);
    g_signal_connect(g_show_bindings, "toggled", G_CALLBACK(on_show_bindings), NULL);
    targets = gtk_string_list_new(NULL);
    gtk_string_list_append(targets, "Remap a control from the list...");
    for (target = 0; target < A2600_TARGET_COUNT; target++)
        gtk_string_list_append(targets, a2600_target_name(target));
    g_target_drop = gtk_drop_down_new(G_LIST_MODEL(targets), NULL);
    gtk_widget_set_focus_on_click(g_target_drop, FALSE);
    gtk_widget_set_hexpand(g_target_drop, TRUE);
    gtk_widget_set_tooltip_text(g_target_drop, "Any of the 59 controls: pick one, then press its new key or gamepad button");
    g_signal_connect(g_target_drop, "notify::selected", G_CALLBACK(on_target_picked), NULL);
    gtk_box_append(GTK_BOX(maprow), g_map_button);
    gtk_box_append(GTK_BOX(maprow), g_clear_button);
    gtk_box_append(GTK_BOX(maprow), defaults);
    gtk_box_append(GTK_BOX(maprow), g_show_bindings);
    gtk_box_append(GTK_BOX(maprow), g_target_drop);
    gtk_box_append(GTK_BOX(root), maprow);

    g_map_hint = gtk_label_new("");
    gtk_widget_add_css_class(g_map_hint, "dim-label");
    gtk_widget_set_hexpand(g_map_hint, TRUE);
    gtk_label_set_xalign(GTK_LABEL(g_map_hint), 0);
    /* a long hint wraps onto a second line rather than clip its "Esc cancels"
     * or widen the window; two lines are always reserved, so the window never
     * changes height either */
    gtk_label_set_wrap(GTK_LABEL(g_map_hint), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(g_map_hint), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_lines(GTK_LABEL(g_map_hint), 2);
    gtk_label_set_ellipsize(GTK_LABEL(g_map_hint), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(g_map_hint), 1);
    gtk_label_set_yalign(GTK_LABEL(g_map_hint), 0);
    {
        PangoLayout *twolines = gtk_widget_create_pango_layout(g_map_hint, "A\nA");
        int linesw, linesh;
        pango_layout_get_pixel_size(twolines, &linesw, &linesh);
        g_object_unref(twolines);
        gtk_widget_set_size_request(g_map_hint, -1, linesh);
    }
    gtk_box_append(GTK_BOX(root), g_map_hint);

    header = adw_header_bar_new();
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(header),
                                    adw_window_title_new("Controllers", "Atari 2600"));
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), a2600_help_button("controllers"));
    a2600_help_set_context(GTK_WINDOW(g_window), "controllers");
    toolbar = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), root);
    adw_window_set_content(ADW_WINDOW(g_window), toolbar);

    keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key_pressed), NULL);
    g_signal_connect(keys, "key-released", G_CALLBACK(on_key_released), NULL);
    gtk_widget_add_controller(g_window, keys);

    focus = gtk_event_controller_focus_new();
    g_signal_connect(focus, "leave", G_CALLBACK(on_focus_leave), NULL);
    gtk_widget_add_controller(g_window, focus);

    set_map_state(-2);
    sync_widgets();
    g_signal_connect(g_window, "map", G_CALLBACK(on_window_map), NULL);
    g_signal_connect(g_window, "unmap", G_CALLBACK(on_window_unmap), NULL);
}

static void ensure_window(GtkWindow *parent, a2600session *session)
{
    g_session = session;
    if (parent)
        g_parent = parent;
    if (!g_window)
        build_window(g_parent);
}

void a2600_controllers_show(GtkWindow *parent, a2600session *session)
{
    ensure_window(parent, session);
    gtk_window_present(GTK_WINDOW(g_window));
}

void a2600_controllers_toggle(GtkWindow *parent, a2600session *session)
{
    ensure_window(parent, session);
    if (gtk_widget_get_visible(g_window))
        hide_window();
    else
        gtk_window_present(GTK_WINDOW(g_window));
}

gboolean a2600_controllers_visible(void)
{
    return g_window && gtk_widget_get_visible(g_window);
}
