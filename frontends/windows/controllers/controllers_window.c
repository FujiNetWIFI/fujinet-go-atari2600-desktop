/*
 * The Win32 Controllers window (F9): what is plugged into the Atari 2600's two
 * ports drawn as a player sees it, from the session's layout
 * (a2600session_controls_layout), so every frontend shows the same parts.
 *
 *   JOYSTICK  the CX40: a black textured square with a red fire button in a
 *             recessed teardrop at the top left, a ribbed rubber boot in a
 *             ring of tick marks and an octagonal shaft cap. The cap follows
 *             the pointer while you drag it, and what drives the machine
 *             (keys, a gamepad) the rest of the time.
 *   PADDLES   the CX30 pair: a black wedge each with a knurled knob you turn,
 *             a red button on the left edge and two turn arrows.
 *   DRIVING   the CX20: the paddle's body with an endless wheel.
 *   KEYPAD    the CX50: twelve round keys with light-blue legends.
 *
 * Under them the six-switch console (POWER, TV TYPE, the two difficulty
 * switches, GAME SELECT and GAME RESET), a STOP button on the wood strip, the
 * gamepads with the port each drives, and Map mode. Each port follows what the
 * session says is attached (a2600session_view_type); the window never changes
 * size when a game swaps a controller.
 *
 * MOUSE: a press on a button, a key, a turn arrow or a switch holds it; a press
 * on the stick follows the pointer (a2600session_stick_from_vector) and lets go
 * on release; a press on a knob turns it to the pointer's angle and the mouse
 * wheel nudges it; the driving wheel gives one twist pulse per notch of drag.
 * Presses are the raw button-down and button-up, never a click: a game reads
 * its controllers once a frame. The mouse is captured on press; letting go
 * anywhere, losing the capture, hiding or deactivating the window releases
 * whatever the pointer holds. POWER (a power cycle) and STOP fire on release
 * over the part, like a real button.
 *
 * What the machine sees held -- keyboard, gamepads and this window together
 * (a2600session_buttons_held, a2600session_analog_get, a2600session_switch_held)
 * -- is polled every 40 ms and painted in orange (#FFB04A), so the window
 * doubles as a gamepad tester.
 *
 * KEYS: a key typed in this window goes to the main window's key path, so it
 * plays exactly as typed there; only Map mode keeps keys for itself.
 *
 * MAP MODE: Map arms a two-step rebind -- click a part to pick its target (the
 * stick: the side of the ring), or pick any of the targets from the list; then
 * press a host key OR a gamepad button; Esc cancels. While armed the parts show
 * what drives them and typing does not reach the machine; rebinding steals the
 * key from whatever held it. Clear takes the picked target's key and gamepad
 * button away; Show bindings prints the same legends outside Map mode. F9, F11
 * and F12 cannot be bound. The paddle knobs and the driving wheel follow the
 * analog sticks (Settings), so they are not targets; their turn arrows are.
 *
 * FIXED SIZE: a caption-only tool window with no thick frame and no maximize
 * box, laid out for both ports, two gamepad rows and the Map row.
 *
 * The art is drawn at twice the size into a memory bitmap and shrunk with
 * HALFTONE (GDI has no anti-aliasing of its own); the text goes on after at the
 * window's own size.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <math.h>
#include "controllers_window.h"

/* GET_X_LPARAM / GET_Y_LPARAM live here, not in windows.h. */
#include <windowsx.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../help/help_window.h"
#include "../key_forward.h"
#include "../resource.h"

#define PANEL_CLASS   "FujiNetGoAtari2600Controllers"
#define PANEL_STYLE   (WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN)
#define PANEL_EXSTYLE WS_EX_TOOLWINDOW

/* Sizes at 96 dpi; SCL() scales them to the window's. */
#define UNIT96      25      /* pixels per layout unit */
#define MARGIN      12
#define CASE_PAD    16
#define BRAND_H     18      /* the strip along the top of the mat */
#define HEAD_H      22
#define NAME_H      22      /* a gamepad row: its name line ... */
#define SEG_H       24      /* ... and its Automatic / Left / Right line */
#define ROW_GAP      6
#define PAD_ROWS     2      /* two ports: two pads is the usual most */
#define SEGMENTS     3      /* Automatic, Left port, Right port */
#define BTN_H       28
#define MAP_W       56
#define CLEAR_W     56
#define DEFAULTS_W  74
#define SHOW_W     118
#define HELP_W      60
#define PI_DEG 3.14159265358979323846

#define IDT_CAPTURE 1
#define IDT_HELD    2
#define IDT_PADS    3

#define HX(v) RGB(((v) >> 16) & 0xFF, ((v) >> 8) & 0xFF, (v) & 0xFF)

/* The buttons under the mat: the Map row. */
enum { SB_MAP, SB_CLEAR, SB_DEFAULTS, SB_SHOW, SB_HELP, SB_COUNT };

typedef struct {
    unsigned generation;
    int views[2], count, assign[PAD_ROWS], effective[PAD_ROWS];
} pads_state;

typedef struct {
    unsigned held[2];                       /* bit per a2600_action, merged, per port */
    int analog[2][2];                       /* a paddle's position, per port, per paddle */
    int wheel[2];                           /* the driving wheel's analog position */
    unsigned char sw[A2600_SW_COUNT];       /* latched position, or held now for SELECT / RESET */
} held_state;

static HWND g_panel, g_parent, g_combo;
static a2600session *g_session;
static int g_dpi = 96;
static int g_view[2];                       /* a2600_ctrl_type drawn on each port */

/* Map mode: -2 idle, -1 armed and waiting for a target, >= 0 waiting for a
 * key or gamepad button to bind to that target. */
static int g_map_state = -2;
static int g_show_bindings;
static WPARAM g_swallow_vkey;               /* the key Map took: its repeats and release too */
static char g_hint[300];
static int g_hover_part = -1;               /* the part under the idle pointer: its bindings are the hint */
static int g_tracking;

/* The controllers' layout (units), and where it sits in the client. */
static const a2600_control *g_layout;
static int g_numlayout;
static float g_layw, g_layh;
static double g_unit, g_orgx, g_orgy;

/* What the pointer holds. */
static int g_mouse_part = -1;               /* a pressed button / key / arrow / switch (layout index) */
static int g_mouse_stick = -1;              /* a stick drag: the port */
static int g_mouse_knob = -1;               /* a knob drag: the layout index */
static int g_mouse_wheel = -1;              /* a wheel drag: the layout index */
static int g_mouse_action = -1;             /* a pressed POWER or STOP (fires on release) */
static double g_stick_dx, g_stick_dy;       /* the dragged cap's offset, -1..1 of the reach */
static float g_wheel_last, g_wheel_acc;     /* the drag's last angle, and degrees not yet a pulse */
static float g_wheel_deg[2];                /* where each port's driving wheel is drawn */

/* What was last painted as held. */
static held_state g_shown;
static pads_state g_pads_shown;

static int g_total_w, g_total_h;
static RECT g_case_rc, g_brand_rc, g_gp_rc, g_gp_head_rc, g_gp_row_rc[PAD_ROWS];
static RECT g_btn_rc[SB_COUNT], g_combo_rc, g_hint_rc;

static HFONT g_font, g_bold, g_legend, g_tiny, g_brand, g_caption, g_title, g_logo, g_key;

static void refresh_held(void);
static void refresh_pads(void);

static int SCL(int val) { return a2600_scale(val, g_dpi); }

/* ---- fonts ------------------------------------------------------------------ */

static void free_fonts(void)
{
    HFONT *const slots[] = { &g_font, &g_bold, &g_legend, &g_tiny, &g_brand, &g_caption, &g_title, &g_logo, &g_key };
    size_t idx;
    for (idx = 0; idx < sizeof slots / sizeof slots[0]; idx++)
        if (*slots[idx]) { DeleteObject(*slots[idx]); *slots[idx] = NULL; }
}

static HFONT italic_of(HFONT base)
{
    LOGFONTA face;
    if (!base || !GetObjectA(base, sizeof face, &face)) return NULL;
    face.lfItalic = TRUE;
    return CreateFontIndirectA(&face);
}

static void make_fonts(void)
{
    HFONT plain;
    free_fonts();
    g_font = a2600_make_font(g_dpi, 0, FW_NORMAL, NULL);
    g_bold = a2600_make_font(g_dpi, 0, FW_BOLD, NULL);
    g_legend = a2600_make_font(g_dpi, 9, FW_BOLD, NULL);
    g_tiny = a2600_make_font(g_dpi, 9, FW_NORMAL, NULL);
    g_brand = a2600_make_font(g_dpi, 12, FW_BOLD, NULL);
    g_caption = a2600_make_font(g_dpi, 12, FW_BOLD, NULL);
    g_logo = a2600_make_font(g_dpi, 15, FW_BOLD, NULL);
    g_key = a2600_make_font(g_dpi, 11, FW_BOLD, NULL);
    plain = a2600_make_font(g_dpi, 13, FW_NORMAL, NULL);
    g_title = italic_of(plain);
    if (plain) DeleteObject(plain);
}

/* ---- text -------------------------------------------------------------------- */

/* UTF-8 (key names, gamepad names) through DrawTextW. */
static int text_out(HDC hdc, const char *utf8, RECT *rect, UINT fmt)
{
    wchar_t wide[256];
    if (!utf8 || !*utf8) return 0;
    if (MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, (int)(sizeof wide / sizeof wide[0])) <= 0) return 0;
    return DrawTextW(hdc, wide, -1, rect, fmt | DT_NOPREFIX);
}

static void text_line(HDC hdc, const char *utf8, const RECT *rect, UINT fmt)
{
    RECT copy = *rect;
    text_out(hdc, utf8, &copy, fmt | DT_SINGLELINE | DT_VCENTER);
}

/* The colours live here and only here; the geometry is the core's. From the
 * photos of the real controllers and console (DESIGN-atari.md). */
#define COL_HELD       HX(A2600SESSION_ACCENT_RGB)      /* the orange of a held part: the design's #FFB04A family */
#define COL_HELD_RING  HX(0xFFB04A)
#define COL_ON_HELD    HX(0x1C1C1C)
#define COL_MAT_TOP    HX(0x8E897C)             /* the desk: light enough that black plastic reads */
#define COL_MAT_BOT    HX(0x6B675C)
#define COL_MAT_EDGE   HX(0x3D3A33)
#define COL_BRAND_INK  HX(0xF6F2E4)
#define COL_SHADOW     HX(0x403D36)
#define COL_CAPTION    HX(0xFBF8EC)
#define COL_DIM_TO     HX(0x7C776B)             /* unpicked parts fade toward the mat */
#define COL_PLASTIC    HX(0x1C1C1C)
#define COL_PLASTIC_HI HX(0x2A2A2A)
#define COL_PLASTIC_LO HX(0x101010)
#define COL_FIRE       HX(0xE23B1F)
#define COL_FIRE_HI    HX(0xF05A38)
#define COL_FIRE_DOWN  HX(0xB02A14)
#define COL_ORANGE     HX(0xE58A1C)
#define COL_PANEL      HX(0x151515)
#define NOFILL         ((COLORREF)0xFFFFFFFFu)

/* ---- art: GDI at a supersampled scale ------------------------------------------- */

/* One art pass draws the client rectangle with its top-left at (g_art_x,
 * g_art_y) into a bitmap g_art_k times the size and g_art_rows rows tall. */
static int g_art_k = 1, g_art_x, g_art_y, g_art_rows;

static int art_xpos(double val) { return (int)floor((val - g_art_x) * g_art_k + 0.5); }
static int art_ypos(double val) { return (int)floor((val - g_art_y) * g_art_k + 0.5); }
static int art_len(double val) { return (int)floor(val * g_art_k + 0.5); }

static COLORREF mix_color(COLORREF start, COLORREF finish, double frac)
{
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    return RGB((int)(GetRValue(start) + (GetRValue(finish) - GetRValue(start)) * frac + 0.5),
               (int)(GetGValue(start) + (GetGValue(finish) - GetGValue(start)) * frac + 0.5),
               (int)(GetBValue(start) + (GetBValue(finish) - GetBValue(start)) * frac + 0.5));
}

/* An unpicked part in Map mode: 40% of its own colours over the mat's. */
static COLORREF tone(COLORREF col, int dim)
{
    return dim ? mix_color(col, COL_DIM_TO, 0.6) : col;
}

static POINT apt(double xpos, double ypos)
{
    POINT pnt;
    pnt.x = art_xpos(xpos);
    pnt.y = art_ypos(ypos);
    return pnt;
}

static void poly(HDC hdc, const POINT *pnts, int num, COLORREF fill, COLORREF line, double width)
{
    HBRUSH brush = fill == NOFILL ? (HBRUSH)GetStockObject(NULL_BRUSH) : CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, width > 0 ? art_len(width) : 0, line);
    HGDIOBJ oldb = SelectObject(hdc, brush);
    HGDIOBJ oldp = SelectObject(hdc, width > 0 ? (HGDIOBJ)pen : GetStockObject(NULL_PEN));
    Polygon(hdc, pnts, num);
    SelectObject(hdc, oldp);
    SelectObject(hdc, oldb);
    DeleteObject(pen);
    if (fill != NOFILL) DeleteObject(brush);
}

static void ellipse(HDC hdc, double ctrx, double ctry, double radx, double rady, COLORREF fill, COLORREF line,
                    double width)
{
    HBRUSH brush = fill == NOFILL ? (HBRUSH)GetStockObject(NULL_BRUSH) : CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, width > 0 ? art_len(width) : 0, line);
    HGDIOBJ oldb = SelectObject(hdc, brush);
    HGDIOBJ oldp = SelectObject(hdc, width > 0 ? (HGDIOBJ)pen : GetStockObject(NULL_PEN));
    Ellipse(hdc, art_xpos(ctrx - radx), art_ypos(ctry - rady), art_xpos(ctrx + radx), art_ypos(ctry + rady));
    SelectObject(hdc, oldp);
    SelectObject(hdc, oldb);
    DeleteObject(pen);
    if (fill != NOFILL) DeleteObject(brush);
}

static void rrect(HDC hdc, double xpos, double ypos, double width, double height, double radius, COLORREF fill,
                  COLORREF line, double linew)
{
    HBRUSH brush = fill == NOFILL ? (HBRUSH)GetStockObject(NULL_BRUSH) : CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, linew > 0 ? art_len(linew) : 0, line);
    HGDIOBJ oldb = SelectObject(hdc, brush);
    HGDIOBJ oldp = SelectObject(hdc, linew > 0 ? (HGDIOBJ)pen : GetStockObject(NULL_PEN));
    RoundRect(hdc, art_xpos(xpos), art_ypos(ypos), art_xpos(xpos + width), art_ypos(ypos + height),
              art_len(radius * 2), art_len(radius * 2));
    SelectObject(hdc, oldp);
    SelectObject(hdc, oldb);
    DeleteObject(pen);
    if (fill != NOFILL) DeleteObject(brush);
}

static void line_seg(HDC hdc, double x0, double y0, double x1, double y1, COLORREF col, double width)
{
    HPEN pen = CreatePen(PS_SOLID, art_len(width) > 0 ? art_len(width) : 1, col);
    HGDIOBJ oldp = SelectObject(hdc, pen);
    MoveToEx(hdc, art_xpos(x0), art_ypos(y0), NULL);
    LineTo(hdc, art_xpos(x1), art_ypos(y1));
    SelectObject(hdc, oldp);
    DeleteObject(pen);
}

/* A rounded rectangle filled with a vertical gradient, then outlined. */
static void grad_rrect(HDC hdc, double xpos, double ypos, double width, double height, double radius,
                       COLORREF top, COLORREF bottom, COLORREF edge, double edgew)
{
    const int left = art_xpos(xpos), right = art_xpos(xpos + width);
    const int first = art_ypos(ypos), last = art_ypos(ypos + height);
    HRGN clip = CreateRoundRectRgn(left, first, right + 1, last + 1, art_len(radius * 2), art_len(radius * 2));
    int row;
    HGDIOBJ oldb;
    SelectClipRgn(hdc, clip);
    oldb = SelectObject(hdc, GetStockObject(DC_BRUSH));
    for (row = first < 0 ? 0 : first; row <= last && row < g_art_rows; row++) {
        SetDCBrushColor(hdc, mix_color(top, bottom, last > first ? (double)(row - first) / (last - first) : 0));
        PatBlt(hdc, left, row, right - left + 1, 1, PATCOPY);
    }
    SelectObject(hdc, oldb);
    SelectClipRgn(hdc, NULL);
    DeleteObject(clip);
    if (edgew > 0) rrect(hdc, xpos, ypos, width, height, radius, NOFILL, edge, edgew);
}

/* The pixel rectangle of a layout part. */
static void part_box(const a2600_control *ctl, double *xpos, double *ypos, double *width, double *height)
{
    *xpos = g_orgx + (double)ctl->x * g_unit;
    *ypos = g_orgy + (double)ctl->y * g_unit;
    *width = (double)ctl->w * g_unit;
    *height = (double)ctl->h * g_unit;
}

static void part_rect(const a2600_control *ctl, RECT *rect)
{
    double xpos, ypos, width, height;
    part_box(ctl, &xpos, &ypos, &width, &height);
    SetRect(rect, (int)floor(xpos), (int)floor(ypos), (int)ceil(xpos + width), (int)ceil(ypos + height));
}

static int binding_mode(void)
{
    return g_map_state != -2 || g_show_bindings;
}

/* ---- which parts are lit ------------------------------------------------------- */

static int part_index(const a2600_control *ctl)
{
    return (int)(ctl - g_layout);
}

static int pressable(const a2600_control *ctl)
{
    return ctl->kind != A2600_CTL_BODY && ctl->kind != A2600_CTL_CAPTION;
}

/* The port a target belongs to, or -1 for the console's. */
static int target_port(int target)
{
    return target >= 0 && target < 2 * A2600_ACT_PER_PORT ? target / A2600_ACT_PER_PORT : -1;
}

/* Whether the machine sees a part held (from any source). */
static int part_held(const a2600_control *ctl)
{
    const int port = ctl->port;
    if (ctl->kind == A2600_CTL_STICK)
        return (g_shown.held[port] & ((1u << A2600_ACT_JOY_UP) | (1u << A2600_ACT_JOY_DOWN) |
                                      (1u << A2600_ACT_JOY_LEFT) | (1u << A2600_ACT_JOY_RIGHT))) != 0 ||
               g_mouse_stick == port;
    if (ctl->kind == A2600_CTL_KNOB) return g_mouse_knob == part_index(ctl);     /* the index mark shows where it is */
    if (ctl->kind == A2600_CTL_WHEEL) return g_mouse_wheel == part_index(ctl);
    if (ctl->target < 0) return 0;
    if (port >= 0 && ctl->target >= port * A2600_ACT_PER_PORT && ctl->target < (port + 1) * A2600_ACT_PER_PORT)
        return (g_shown.held[port] >> (ctl->target - port * A2600_ACT_PER_PORT)) & 1u;
    if (ctl->target >= A2600_TARGET_SWITCH(0) && ctl->target < A2600_TARGET_SYSACT(0)) {
        const int sw = ctl->target - A2600_TARGET_SWITCH(0);
        if (ctl->kind == A2600_CTL_BUTTON) return g_shown.sw[sw] != 0;
        return g_mouse_part == part_index(ctl);
    }
    return g_mouse_action == part_index(ctl);
}

/* Whether a part is shown lit: what the machine sees, in orange. In Map mode
 * the picked target is the one lit. */
static int part_lit(const a2600_control *ctl)
{
    if (g_map_state != -2) {
        if (g_map_state < 0) return 0;
        switch (ctl->kind) {
        case A2600_CTL_BUTTON:
        case A2600_CTL_ROUND_BUTTON:
        case A2600_CTL_TURN:
        case A2600_CTL_SWITCH: return ctl->target == g_map_state;
        case A2600_CTL_STICK:
            return target_port(g_map_state) == ctl->port && g_map_state % A2600_ACT_PER_PORT <= A2600_ACT_JOY_RIGHT;
        default: return 0;
        }
    }
    if (ctl->kind == A2600_CTL_SWITCH) return g_mouse_part == part_index(ctl);
    return part_held(ctl) != 0;
}

/* Map mode with a target picked: every other part is drawn at 40%. */
static int part_dim(const a2600_control *ctl)
{
    if (g_map_state < 0) return 0;
    if (!pressable(ctl)) return 0;
    return !part_lit(ctl);
}

/* A device's case in Map mode with a target picked elsewhere. */
static int body_dim(int port)
{
    return g_map_state >= 0 && target_port(g_map_state) != port;
}

/* ---- the CX40 joystick ----------------------------------------------------------- */

/* The cap's offset from rest, -1..1 of the reach: the pointer while dragging,
 * else what is held (diagonals normalised so the cap stays inside the boot). */
static void stick_offset(int port, double *offx, double *offy)
{
    int dirx, diry;
    if (g_mouse_stick == port) { *offx = g_stick_dx; *offy = g_stick_dy; return; }
    dirx = (int)((g_shown.held[port] >> A2600_ACT_JOY_RIGHT) & 1u) - (int)((g_shown.held[port] >> A2600_ACT_JOY_LEFT) & 1u);
    diry = (int)((g_shown.held[port] >> A2600_ACT_JOY_DOWN) & 1u) - (int)((g_shown.held[port] >> A2600_ACT_JOY_UP) & 1u);
    *offx = dirx;
    *offy = diry;
    if (dirx && diry) { *offx *= 0.7071; *offy *= 0.7071; }
}

/* The angle (degrees clockwise from straight up) the held directions point
 * at, if any. */
static int held_angle(int port, double *ang)
{
    const unsigned held = g_shown.held[port];
    const int dirx = (int)((held >> A2600_ACT_JOY_RIGHT) & 1u) - (int)((held >> A2600_ACT_JOY_LEFT) & 1u);
    const int diry = (int)((held >> A2600_ACT_JOY_DOWN) & 1u) - (int)((held >> A2600_ACT_JOY_UP) & 1u);
    if (!dirx && !diry) return 0;
    *ang = atan2((double)dirx, (double)-diry) * 180.0 / PI_DEG;
    return 1;
}

static double angle_between(double one, double two)
{
    double delta = fmod(one - two, 360.0);
    if (delta > 180) delta -= 360;
    if (delta < -180) delta += 360;
    return fabs(delta);
}

/* The case, the fire button's recess, the ring of ticks: 8.0 x 8.0 u. */
static void art_joy_body(HDC hdc, const a2600_control *ctl)
{
    double bx, by, bw, bh, ang = 0;
    const double u = g_unit;
    const int dim = body_dim(ctl->port), have = held_angle(ctl->port, &ang);
    int idx;
    part_box(ctl, &bx, &by, &bw, &bh);

    rrect(hdc, bx + u * 0.12, by + u * 0.2, bw, bh, u * 0.9, COL_SHADOW, COL_SHADOW, 0);
    rrect(hdc, bx, by, bw, bh, u * 0.9, tone(COL_PLASTIC, dim), tone(COL_PLASTIC_LO, dim), 1.2);
    rrect(hdc, bx + u * 0.25, by + u * 0.25, bw - u * 0.5, bh - u * 0.5, u * 0.7, NOFILL, tone(COL_PLASTIC_HI, dim), 1.0);
    /* the teardrop: the button's recess and its neck to the ring */
    {
        const double fromx = bx + u * 1.7, fromy = by + u * 1.7, tox = bx + u * 4.5, toy = by + u * 4.5;
        const double dirx = (tox - fromx) / hypot(tox - fromx, toy - fromy), diry = (toy - fromy) / hypot(tox - fromx, toy - fromy);
        const double half = u * 0.45;
        POINT neck[4];
        neck[0] = apt(fromx - diry * half, fromy + dirx * half);
        neck[1] = apt(tox - diry * half, toy + dirx * half);
        neck[2] = apt(tox + diry * half, toy - dirx * half);
        neck[3] = apt(fromx + diry * half, fromy - dirx * half);
        poly(hdc, neck, 4, tone(COL_PLASTIC_LO, dim), tone(COL_PLASTIC_LO, dim), 0);
        ellipse(hdc, fromx, fromy, u * 1.1, u * 1.1, tone(COL_PLASTIC_LO, dim), tone(COL_PLASTIC_LO, dim), 0);
    }
    ellipse(hdc, bx + u * 4.5, by + u * 4.5, u * 3.0, u * 3.0, tone(COL_PLASTIC_LO, dim), tone(HX(0x070707), dim), 1.0);
    for (idx = 0; idx < 36; idx++) {    /* the ring's tick marks; a held direction lights its sector */
        const double deg = idx * 10.0, rad = deg * PI_DEG / 180.0;
        const int lit = have && angle_between(deg, ang) <= 22.6;
        line_seg(hdc, bx + u * 4.5 + u * 2.52 * sin(rad), by + u * 4.5 - u * 2.52 * cos(rad),
                 bx + u * 4.5 + u * 2.86 * sin(rad), by + u * 4.5 - u * 2.86 * cos(rad),
                 lit ? COL_HELD_RING : tone(HX(0x3A3A3A), dim), lit ? u * 0.11 : u * 0.07);
    }
}

/* The ribbed boot and the octagonal shaft cap, tilted toward what is held. */
static void art_stick(HDC hdc, const a2600_control *ctl)
{
    double bx, by, bw, bh, offx, offy, capx, capy;
    const double u = g_unit;
    const int dim = part_dim(ctl), lit = part_lit(ctl);
    POINT oct[8];
    int idx;
    part_box(ctl, &bx, &by, &bw, &bh);
    stick_offset(ctl->port, &offx, &offy);
    {
        const double len = sqrt(offx * offx + offy * offy);
        if (len > 1.0) { offx /= len; offy /= len; }
    }
    capx = bx + bw / 2 + offx * u * 1.0;
    capy = by + bh / 2 + offy * u * 1.0;
    /* the boot's rings squash toward the tilt for depth */
    ellipse(hdc, bx + bw / 2 + offx * u * 0.08, by + bh / 2 + offy * u * 0.08, u * 2.1, u * 2.1, tone(HX(0x151515), dim),
            tone(HX(0x0A0A0A), dim), 1.0);
    ellipse(hdc, bx + bw / 2 + offx * u * 0.2, by + bh / 2 + offy * u * 0.2, u * 1.7, u * 1.7, tone(HX(0x262626), dim),
            tone(HX(0x151515), dim), 1.0);
    ellipse(hdc, bx + bw / 2 + offx * u * 0.35, by + bh / 2 + offy * u * 0.35, u * 1.3, u * 1.3, tone(HX(0x151515), dim),
            tone(HX(0x262626), dim), 1.0);
    for (idx = 0; idx < 8; idx++) {     /* the octagonal shaft cap */
        const double rad = (idx * 45.0 + 22.5) * PI_DEG / 180.0;
        oct[idx] = apt(capx + u * 0.9 * sin(rad), capy - u * 0.9 * cos(rad));
    }
    poly(hdc, oct, 8, tone(HX(0x0C0C0C), dim), tone(COL_PLASTIC_HI, dim), u * 0.1);
    ellipse(hdc, capx - u * 0.15, capy - u * 0.18, u * 0.28, u * 0.2, tone(HX(0x232323), dim), tone(HX(0x232323), dim), 0);
    if (lit && g_map_state == -2)
        ellipse(hdc, capx, capy, u * 1.02, u * 1.02, NOFILL, COL_HELD_RING, u * 0.1);
}

/* The red fire button: a glossy dome in its recess. */
static void art_fire(HDC hdc, const a2600_control *ctl)
{
    double bx, by, bw, bh;
    const double u = g_unit;
    const int dim = part_dim(ctl), lit = part_lit(ctl), pressed = lit && g_map_state == -2;
    part_box(ctl, &bx, &by, &bw, &bh);
    ellipse(hdc, bx + bw / 2, by + bh / 2, u * 0.8, u * 0.8, tone(pressed ? COL_FIRE_DOWN : COL_FIRE, dim),
            tone(COL_FIRE_DOWN, dim), 1.0);
    if (!pressed)
        ellipse(hdc, bx + bw / 2 - u * 0.25, by + bh / 2 - u * 0.28, u * 0.3, u * 0.22, tone(COL_FIRE_HI, dim),
                tone(COL_FIRE_HI, dim), 0);
    if (lit)
        ellipse(hdc, bx + bw / 2, by + bh / 2, u * 1.0, u * 1.0, NOFILL, COL_HELD_RING, u * 0.1);
}

/* ---- the CX30 paddle and CX20 driving controller --------------------------------- */

/* The wedge body, its label plate with the rackets or the racing car: 7.0 x
 * 6.0 u. */
static void art_paddle_body(HDC hdc, const a2600_control *ctl, int driving)
{
    static const double shape[6][2] = { { 1.0, 0.3 }, { 5.6, 0.3 }, { 7.0, 5.0 }, { 6.2, 5.9 }, { 0.8, 5.9 }, { 0.0, 4.6 } };
    double bx, by, bw, bh;
    const double u = g_unit;
    const int dim = body_dim(ctl->port);
    POINT pts[6], shadow[6];
    int idx;
    part_box(ctl, &bx, &by, &bw, &bh);
    for (idx = 0; idx < 6; idx++) {
        pts[idx] = apt(bx + shape[idx][0] * u, by + shape[idx][1] * u);
        shadow[idx] = apt(bx + shape[idx][0] * u + u * 0.12, by + shape[idx][1] * u + u * 0.2);
    }
    poly(hdc, shadow, 6, COL_SHADOW, COL_SHADOW, 0);
    poly(hdc, pts, 6, tone(HX(0x1F1F1F), dim), tone(HX(0x2E2E2E), dim), u * 0.12);
    /* the label plate */
    rrect(hdc, bx + u * 3.6, by + u * 4.3, u * 2.8, u * 1.3, u * 0.15, tone(COL_PLASTIC_LO, dim), tone(HX(0x8A8A8A), dim), 1.0);
    /* the knob covers the plate's left end, so the picture sits where it stays visible */
    if (driving) {      /* the racing car, in orange */
        const COLORREF car = tone(HX(0xE8501A), dim);
        rrect(hdc, bx + u * 4.0, by + u * 5.0, u * 0.7, u * 0.24, u * 0.08, car, car, 0);
        rrect(hdc, bx + u * 4.2, by + u * 4.88, u * 0.32, u * 0.16, u * 0.06, car, car, 0);
        ellipse(hdc, bx + u * 4.15, by + u * 5.3, u * 0.09, u * 0.09, tone(HX(0x0E0E0E), dim), tone(HX(0x0E0E0E), dim), 0);
        ellipse(hdc, bx + u * 4.55, by + u * 5.3, u * 0.09, u * 0.09, tone(HX(0x0E0E0E), dim), tone(HX(0x0E0E0E), dim), 0);
    } else {            /* two crossed rackets and a ball, in yellow */
        const COLORREF gold = tone(HX(0xDCCB78), dim);
        line_seg(hdc, bx + u * 4.12, by + u * 5.42, bx + u * 4.5, by + u * 5.0, gold, u * 0.07);
        line_seg(hdc, bx + u * 4.5, by + u * 5.42, bx + u * 4.12, by + u * 5.0, gold, u * 0.07);
        ellipse(hdc, bx + u * 4.1, by + u * 4.96, u * 0.13, u * 0.13, NOFILL, gold, u * 0.05);
        ellipse(hdc, bx + u * 4.52, by + u * 4.96, u * 0.13, u * 0.13, NOFILL, gold, u * 0.05);
    }
}

/* The knurled knob, with the index mark where the pot is. */
static void art_knob(HDC hdc, const a2600_control *ctl)
{
    double bx, by, bw, bh, ang;
    const double u = g_unit;
    const int dim = part_dim(ctl), lit = part_lit(ctl), wheel = ctl->kind == A2600_CTL_WHEEL;
    double cx, cy;
    int idx;
    part_box(ctl, &bx, &by, &bw, &bh);
    cx = bx + bw / 2;
    cy = by + bh / 2;
    if (wheel) {
        ang = g_wheel_deg[ctl->port & 1] * PI_DEG / 180.0;
    } else {
        const int value = g_shown.analog[ctl->port & 1][ctl->paddle & 1];
        ang = (value / 32767.0) * A2600_KNOB_TRAVEL_DEG * PI_DEG / 180.0;
    }
    ellipse(hdc, cx + u * 0.08, cy + u * 0.12, u * 2.25, u * 2.25, COL_SHADOW, COL_SHADOW, 0);
    ellipse(hdc, cx, cy, u * 2.2, u * 2.2, tone(HX(0x0E0E0E), dim), tone(HX(0x2A2A2A), dim), 1.0);
    for (idx = 0; idx < 60; idx++) {    /* the knurled rim */
        const double rad = idx * 6.0 * PI_DEG / 180.0;
        line_seg(hdc, cx + u * 2.0 * sin(rad), cy - u * 2.0 * cos(rad), cx + u * 2.2 * sin(rad), cy - u * 2.2 * cos(rad),
                 tone(HX(0x2A2A2A), dim), u * 0.05);
    }
    ellipse(hdc, cx, cy, u * 1.75, u * 1.75, tone(HX(0x1A1A1A), dim), tone(HX(0x2A2A2A), dim), 1.0);
    ellipse(hdc, cx, cy, u * 1.65, u * 1.65, tone(HX(0x232323), dim), tone(HX(0x2E2E2E), dim), 1.0);
    if (wheel) {    /* a tick every 30 degrees: the gray-code positions */
        for (idx = 0; idx < 12; idx++) {
            const double rad = ang + idx * 30.0 * PI_DEG / 180.0;
            ellipse(hdc, cx + u * 1.45 * sin(rad), cy - u * 1.45 * cos(rad), u * 0.06, u * 0.06, tone(HX(0x4A4A4A), dim),
                    tone(HX(0x4A4A4A), dim), 0);
        }
    }
    line_seg(hdc, cx + u * 0.9 * sin(ang), cy - u * 0.9 * cos(ang), cx + u * 1.55 * sin(ang), cy - u * 1.55 * cos(ang),
             tone(HX(0xD8D8D8), dim), u * 0.13);
    if (lit && g_map_state == -2)
        ellipse(hdc, cx, cy, u * 2.34, u * 2.34, NOFILL, COL_HELD_RING, u * 0.1);
}

/* The red button on the paddle's left edge. */
static void art_edge_button(HDC hdc, const a2600_control *ctl)
{
    double bx, by, bw, bh;
    const double u = g_unit;
    const int dim = part_dim(ctl), lit = part_lit(ctl), pressed = lit && g_map_state == -2;
    part_box(ctl, &bx, &by, &bw, &bh);
    rrect(hdc, bx, by + (pressed ? u * 0.06 : 0), bw, bh - (pressed ? u * 0.04 : 0), u * 0.14,
          tone(pressed ? COL_FIRE_DOWN : HX(0xD93A22), dim), tone(COL_FIRE_DOWN, dim), 1.0);
    if (!pressed)
        line_seg(hdc, bx + u * 0.1, by + u * 0.12, bx + bw - u * 0.1, by + u * 0.12, tone(HX(0xE8492C), dim), 1.5);
    if (lit)
        rrect(hdc, bx - u * 0.1, by - u * 0.1, bw + u * 0.2, bh + u * 0.2, u * 0.2, NOFILL, COL_HELD_RING, u * 0.1);
}

/* A turn arrow under the knob: a small triangle, held while turning. */
static void art_turn(HDC hdc, const a2600_control *ctl)
{
    double bx, by, bw, bh;
    const double u = g_unit;
    const int dim = part_dim(ctl), lit = part_lit(ctl);
    const int right = (ctl->target % A2600_ACT_PER_PORT) == A2600_ACT_PADDLE_A_INC ||
                      (ctl->target % A2600_ACT_PER_PORT) == A2600_ACT_PADDLE_B_INC ||
                      (ctl->target % A2600_ACT_PER_PORT) == A2600_ACT_DRIVE_CW;
    POINT tri[3];
    part_box(ctl, &bx, &by, &bw, &bh);
    if (right) {
        tri[0] = apt(bx + u * 0.12, by + u * 0.1);
        tri[1] = apt(bx + bw - u * 0.06, by + bh / 2);
        tri[2] = apt(bx + u * 0.12, by + bh - u * 0.1);
    } else {
        tri[0] = apt(bx + bw - u * 0.12, by + u * 0.1);
        tri[1] = apt(bx + u * 0.06, by + bh / 2);
        tri[2] = apt(bx + bw - u * 0.12, by + bh - u * 0.1);
    }
    poly(hdc, tri, 3, lit ? COL_HELD_RING : tone(HX(0x3E3E3E), dim), lit ? COL_HELD_RING : tone(HX(0x5A5A5A), dim), 1.0);
}

/* ---- the CX50 keypad ---------------------------------------------------------------- */

static void art_keypad_body(HDC hdc, const a2600_control *ctl)
{
    double bx, by, bw, bh;
    const double u = g_unit;
    const int dim = body_dim(ctl->port);
    part_box(ctl, &bx, &by, &bw, &bh);
    rrect(hdc, bx + u * 0.1, by + u * 0.16, bw, bh, u * 0.6, COL_SHADOW, COL_SHADOW, 0);
    rrect(hdc, bx, by, bw, bh, u * 0.6, tone(HX(0x141414), dim), tone(HX(0x0A0A0A), dim), 1.2);
    rrect(hdc, bx + u * 0.45, by + u * 0.45, bw - u * 0.9, bh - u * 0.9, u * 0.35, tone(HX(0x1A1A1A), dim),
          tone(HX(0xD7D7D7), dim), u * 0.07 > 1.0 ? u * 0.07 : 1.0);
}

static void art_key(HDC hdc, const a2600_control *ctl)
{
    double bx, by, bw, bh;
    const double u = g_unit;
    const int dim = part_dim(ctl), lit = part_lit(ctl);
    part_box(ctl, &bx, &by, &bw, &bh);
    ellipse(hdc, bx + bw / 2, by + bh / 2, u * 0.5, u * 0.5, tone(lit ? HX(0x3A2A10) : HX(0x080808), dim), tone(HX(0x2C2C2C), dim), 1.0);
    ellipse(hdc, bx + bw / 2 - u * 0.12, by + bh / 2 - u * 0.16, u * 0.2, u * 0.12, tone(HX(0x2C2C2C), dim), tone(HX(0x2C2C2C), dim), 0);
    if (lit)
        ellipse(hdc, bx + bw / 2, by + bh / 2, u * 0.6, u * 0.6, NOFILL, COL_HELD_RING, u * 0.09);
}

/* ---- the console --------------------------------------------------------------------- */

static void art_console_body(HDC hdc, const a2600_control *ctl)
{
    double bx, by, bw, bh;
    const double u = g_unit;
    const int dim = body_dim(-1);
    part_box(ctl, &bx, &by, &bw, &bh);
    rrect(hdc, bx + u * 0.1, by + u * 0.16, bw, bh, u * 0.4, COL_SHADOW, COL_SHADOW, 0);
    rrect(hdc, bx, by, bw, bh, u * 0.4, tone(COL_PANEL, dim), tone(HX(0x0A0A0A), dim), 1.2);
    rrect(hdc, bx + u * 0.25, by + u * 0.25, bw - u * 0.5, bh - u * 0.5, u * 0.3, NOFILL, tone(COL_ORANGE, dim), 1.0);
}

static void art_slot(HDC hdc, const a2600_control *ctl)
{
    double bx, by, bw, bh;
    const int dim = body_dim(-1);
    part_box(ctl, &bx, &by, &bw, &bh);
    rrect(hdc, bx, by, bw, bh, g_unit * 0.1, tone(HX(0x0A0A0A), dim), tone(HX(0x2A2A2A), dim), 1.5);
    rrect(hdc, bx + g_unit * 0.25, by + g_unit * 0.25, bw - g_unit * 0.5, bh - g_unit * 0.5, g_unit * 0.06, NOFILL,
          tone(HX(0x1C1C1C), dim), 1.0);
}

/* The wood strip: a grain over a brown gradient, with the Atari mark at the
 * right end. */
static void art_wood(HDC hdc, const a2600_control *ctl)
{
    double bx, by, bw, bh;
    const double u = g_unit;
    const int dim = body_dim(-1);
    int idx;
    part_box(ctl, &bx, &by, &bw, &bh);
    grad_rrect(hdc, bx, by, bw, bh, u * 0.2, tone(HX(0x8A5A2B), dim), tone(HX(0x6E4521), dim), tone(HX(0x3A2410), dim), 1.0);
    for (idx = 0; idx < 6; idx++) {
        const double ypos = by + bh * (0.12 + idx * 0.15);
        line_seg(hdc, bx + u * 0.4, ypos, bx + bw - u * 0.4, ypos + (idx % 2 ? u * 0.05 : -u * 0.05),
                 mix_color(HX(0x7A4F26), HX(0x4A2F16), 0.5), 1.0);
    }
    {   /* the Fuji: three bars leaning together */
        const double ctrx = bx + bw - u * 3.9, base = by + bh - u * 0.28, top = by + u * 0.32;
        int bar;
        for (bar = -1; bar <= 1; bar++) {
            POINT pts[4];
            pts[0] = apt(ctrx + bar * u * 0.34 - u * 0.07, base);
            pts[1] = apt(ctrx + bar * u * 0.34 + u * 0.07, base);
            pts[2] = apt(ctrx + bar * u * 0.2 + u * 0.07, top);
            pts[3] = apt(ctrx + bar * u * 0.2 - u * 0.07, top);
            poly(hdc, pts, 4, tone(HX(0xC7C7C7), dim), tone(HX(0xC7C7C7), dim), 0);
        }
    }
}

/* One slide switch: the slot with its silver toggle up or down. POWER is a
 * button (always up, drawn pressed down while held); SELECT and RESET rest up and
 * go down while held; the other three show their latched position. */
static void art_switch(HDC hdc, const a2600_control *ctl)
{
    double bx, by, bw, bh, ctrx, ctry, toggle;
    const double u = g_unit;
    const int dim = part_dim(ctl), picked = part_lit(ctl) && g_map_state >= 0;
    int up;
    part_box(ctl, &bx, &by, &bw, &bh);
    ctrx = bx + bw / 2;
    ctry = by + bh / 2;
    if (ctl->kind == A2600_CTL_SWITCH) {
        up = g_shown.sw[ctl->target - A2600_TARGET_SWITCH(0)] != 0;
    } else if (ctl->target >= A2600_TARGET_SWITCH(0) && ctl->target < A2600_TARGET_SYSACT(0)) {
        up = g_shown.sw[ctl->target - A2600_TARGET_SWITCH(0)] == 0;      /* SELECT / RESET: down while held */
    } else {
        up = g_mouse_action != part_index(ctl);                           /* POWER */
    }
    rrect(hdc, ctrx - u * 0.25, ctry - u * 0.75, u * 0.5, u * 1.5, u * 0.12, tone(HX(0x0A0A0A), dim), tone(HX(0x2A2A2A), dim), 1.0);
    toggle = up ? ctry - u * 0.75 + u * 0.42 : ctry + u * 0.75 - u * 0.42;
    rrect(hdc, ctrx - u * 0.3 + u * 0.05, toggle - u * 0.35 + u * 0.06, u * 0.6, u * 0.7, u * 0.2, COL_SHADOW, COL_SHADOW, 0);
    rrect(hdc, ctrx - u * 0.3, toggle - u * 0.35, u * 0.6, u * 0.7, u * 0.2, tone(HX(0xC9C9C9), dim), tone(HX(0x8E8E8E), dim), 1.0);
    rrect(hdc, ctrx + u * 0.06, toggle - u * 0.3, u * 0.2, u * 0.6, u * 0.1, tone(HX(0x8E8E8E), dim), tone(HX(0x8E8E8E), dim), 0);
    if (ctl->kind == A2600_CTL_BUTTON && ctl->target >= A2600_TARGET_SWITCH(0) && ctl->target < A2600_TARGET_SYSACT(0)) {
        /* the sawtooth arrow under SELECT and RESET: lit while held */
        const COLORREF col = part_lit(ctl) ? HX(0xFFD27A) : tone(HX(0xE08A1F), dim);
        POINT head[3];
        head[0] = apt(ctrx - u * 0.3, by + bh - u * 0.2);
        head[1] = apt(ctrx + u * 0.3, by + bh - u * 0.2);
        head[2] = apt(ctrx, by + bh - u * 0.6);
        poly(hdc, head, 3, col, col, 0);
        line_seg(hdc, ctrx, by + bh - u * 0.2, ctrx, by + bh, col, u * 0.09);
    }
    if (picked)
        rrect(hdc, bx - u * 0.1, by - u * 0.05, bw + u * 0.2, bh + u * 0.1, u * 0.2, NOFILL, COL_HELD_RING, u * 0.1);
}

/* STOP on the wood strip: a small dark button that fires on release. */
static void art_stop(HDC hdc, const a2600_control *ctl)
{
    double bx, by, bw, bh;
    const double u = g_unit;
    const int dim = part_dim(ctl), lit = part_lit(ctl), pressed = g_mouse_action == part_index(ctl);
    part_box(ctl, &bx, &by, &bw, &bh);
    rrect(hdc, bx + u * 0.05, by + u * 0.08, bw, bh, u * 0.18, COL_SHADOW, COL_SHADOW, 0);
    rrect(hdc, bx, by + (pressed ? u * 0.06 : 0), bw, bh - (pressed ? u * 0.04 : 0), u * 0.18,
          tone(pressed ? HX(0x1A1A1A) : HX(0x2B2B2B), dim), tone(HX(0xC9C9C9), dim), 1.0);
    if (lit && g_map_state >= 0)
        rrect(hdc, bx - u * 0.1, by - u * 0.1, bw + u * 0.2, bh + u * 0.2, u * 0.24, NOFILL, COL_HELD_RING, u * 0.1);
}

/* The mat, its brand strip and every device in the layout, in layout order
 * (so a body is drawn before the parts on it). */
static void art_case(HDC hdc)
{
    int idx;
    grad_rrect(hdc, g_case_rc.left, g_case_rc.top, g_case_rc.right - g_case_rc.left,
               g_case_rc.bottom - g_case_rc.top, SCL(14), COL_MAT_TOP, COL_MAT_BOT, COL_MAT_EDGE, 1.5);
    {   /* the Fuji mark: three bars leaning together, then the name (text pass) */
        int bar;
        for (bar = 0; bar < 3; bar++) {
            POINT pts[4];
            const double ctrx = g_brand_rc.left + SCL(10) + (bar - 1) * SCL(5);
            const double lean = -(bar - 1) * SCL(2);
            pts[0] = apt(ctrx - SCL(1), g_brand_rc.bottom - SCL(3));
            pts[1] = apt(ctrx + SCL(1), g_brand_rc.bottom - SCL(3));
            pts[2] = apt(ctrx + lean + SCL(1), g_brand_rc.top + SCL(3));
            pts[3] = apt(ctrx + lean - SCL(1), g_brand_rc.top + SCL(3));
            poly(hdc, pts, 4, COL_ORANGE, COL_ORANGE, 0);
        }
    }
    for (idx = 0; idx < g_numlayout; idx++) {
        const a2600_control *ctl = &g_layout[idx];
        switch (ctl->kind) {
        case A2600_CTL_BODY:
            if (!strcmp(ctl->label, "Joystick")) art_joy_body(hdc, ctl);
            else if (!strcmp(ctl->label, "Paddle A") || !strcmp(ctl->label, "Paddle B")) art_paddle_body(hdc, ctl, 0);
            else if (!strcmp(ctl->label, "Driving")) art_paddle_body(hdc, ctl, 1);
            else if (!strcmp(ctl->label, "Keypad")) art_keypad_body(hdc, ctl);
            else if (!strcmp(ctl->label, "Console")) art_console_body(hdc, ctl);
            else if (!strcmp(ctl->label, "Cartridge slot")) art_slot(hdc, ctl);
            else if (!strcmp(ctl->label, "Wood")) art_wood(hdc, ctl);
            break;
        case A2600_CTL_STICK: art_stick(hdc, ctl); break;
        case A2600_CTL_KNOB:
        case A2600_CTL_WHEEL: art_knob(hdc, ctl); break;
        case A2600_CTL_TURN: art_turn(hdc, ctl); break;
        case A2600_CTL_ROUND_BUTTON:
            if (ctl->target % A2600_ACT_PER_PORT == A2600_ACT_JOY_FIRE) art_fire(hdc, ctl);
            else art_key(hdc, ctl);
            break;
        case A2600_CTL_BUTTON:
            if (ctl->port < 0 && !strcmp(ctl->label, "STOP")) art_stop(hdc, ctl);
            else if (ctl->port < 0) art_switch(hdc, ctl);
            else art_edge_button(hdc, ctl);
            break;
        case A2600_CTL_SWITCH: art_switch(hdc, ctl); break;
        default: break;
        }
    }
}

/* In Map mode and with Show bindings a target shows what drives it. */
static void binding_label(int target, char *out, int outsz)
{
    const a2600_binding bind = a2600session_binding_get(g_session, target);
    out[0] = '\0';
    if (bind.keysym) {
        a2600session_keysym_name(bind.keysym, out, outsz);
        /* the labels are small: "Keypad 5" reads "KP 5" */
        if (strncmp(out, "Keypad ", 7) == 0) {
            memmove(out + 3, out + 7, strlen(out + 7) + 1);
            memcpy(out, "KP ", 3);
        }
    }
    if (!out[0]) snprintf(out, (size_t)outsz, "%s", "\xe2\x80\x94");
}

/* A target's binding, printed on its part (or under a console part's slot). */
static void text_binding_on(HDC hdc, const a2600_control *ctl, const RECT *rect, int dim)
{
    const double u = g_unit;
    char legend[96];
    RECT box = *rect;
    if (!binding_mode() || ctl->target < 0) return;
    binding_label(ctl->target, legend, sizeof legend);
    SelectObject(hdc, g_tiny);
    if (ctl->port < 0) {        /* console: below the slot */
        box.left -= (int)(u * 0.5);
        box.right += (int)(u * 0.5);
        box.top = rect->bottom;
        box.bottom = rect->bottom + (int)(u * 0.55);
        SetTextColor(hdc, g_map_state == ctl->target ? COL_HELD_RING : tone(HX(0xE6DFCF), dim));
    } else {
        box.left -= (int)(u * 0.15);
        box.right += (int)(u * 0.15);
        SetTextColor(hdc, g_map_state == ctl->target ? COL_HELD_RING : RGB(0xFF, 0xFF, 0xFF));
    }
    text_line(hdc, legend, &box, DT_CENTER | DT_END_ELLIPSIS);
}

/* A console switch's printed legend above its slot, and the names of its two
 * positions beside the ends of the slot. */
static void text_console_legend(HDC hdc, const a2600_control *ctl, const RECT *rect, int dim)
{
    const double u = g_unit;
    const int power = ctl->target == A2600_TARGET_SYSACT(A2600_SYSACT_REBOOT_CONFIG);
    const int sw = ctl->target - A2600_TARGET_SWITCH(0);
    const char *topname = NULL, *botname = NULL;
    RECT leg, top, bot;
    const int ctrx = (rect->left + rect->right) / 2, ctry = (rect->top + rect->bottom) / 2;
    if (ctl->port >= 0 || !ctl->legend) return;
    if (power) { topname = "on"; botname = "off"; }
    else if (sw == A2600_SW_COLOR_BW) { topname = "color"; botname = "b-w"; }
    else if (sw == A2600_SW_LEFT_DIFF || sw == A2600_SW_RIGHT_DIFF) { topname = "a"; botname = "b"; }
    leg = *rect;
    leg.left = ctrx - (int)(u * 1.35);
    leg.right = ctrx + (int)(u * 1.35);
    leg.top = ctry - (int)(u * 1.45);
    leg.bottom = ctry - (int)(u * 0.85);
    SelectObject(hdc, g_legend);
    SetTextColor(hdc, power ? tone(HX(0xC8341E), dim) : tone(HX(0xE08A1F), dim));
    {
        const char *space = strchr(ctl->legend, ' ');
        if (space && strlen(ctl->legend) > 8) {
            /* "left difficulty", "game select": two lines, so neighbours do not collide */
            char first[24];
            RECT upper = leg;
            snprintf(first, sizeof first, "%.*s", (int)(space - ctl->legend), ctl->legend);
            upper.bottom = leg.top;
            upper.top = leg.top - (int)(u * 0.5);
            text_line(hdc, first, &upper, DT_CENTER);
            text_line(hdc, space + 1, &leg, DT_CENTER);
        } else {
            text_line(hdc, ctl->legend, &leg, DT_CENTER);
        }
    }
    SetTextColor(hdc, tone(HX(0xE08A1F), dim));
    top = *rect;
    top.left = ctrx + (int)(u * 0.38);
    top.right = top.left + (int)(u * 1.1);
    top.top = ctry - (int)(u * 0.95);
    top.bottom = top.top + (int)(u * 0.5);
    bot = top;
    bot.bottom = ctry + (int)(u * 0.95);
    bot.top = bot.bottom - (int)(u * 0.5);
    if (topname) text_line(hdc, topname, &top, DT_LEFT);
    if (botname) text_line(hdc, botname, &bot, DT_LEFT);
}

/* The labels over the art, at the window's own size. */
static void text_parts(HDC hdc)
{
    int idx;
    RECT brand = g_brand_rc;
    SetBkMode(hdc, TRANSPARENT);
    SelectObject(hdc, g_brand);
    SetTextColor(hdc, COL_BRAND_INK);
    brand.left += SCL(26);
    text_line(hdc, "Atari 2600", &brand, DT_LEFT);
    for (idx = 0; idx < g_numlayout; idx++) {
        const a2600_control *ctl = &g_layout[idx];
        RECT rect;
        const int dim = part_dim(ctl);
        const double u = g_unit;
        char legend[96];
        part_rect(ctl, &rect);
        switch (ctl->kind) {
        case A2600_CTL_CAPTION: {
            char line[96];
            snprintf(line, sizeof line, "%s: %s", ctl->label, a2600_ctrl_type_name(g_view[ctl->port & 1]));
            SelectObject(hdc, g_caption);
            SetTextColor(hdc, COL_CAPTION);
            text_line(hdc, line, &rect, DT_CENTER);
            break;
        }
        case A2600_CTL_BODY:
            if (!strcmp(ctl->label, "Joystick")) {
                SelectObject(hdc, g_tiny);
                SetTextColor(hdc, tone(HX(0x3A3A3A), body_dim(ctl->port)));
                {
                    RECT top = rect;
                    top.left += (int)(u * 3.3);
                    top.right -= (int)(u * 3.3);
                    top.top += (int)(u * 1.25);
                    top.bottom = top.top + (int)(u * 0.5);
                    text_line(hdc, "TOP", &top, DT_CENTER);
                }
            } else if (!strcmp(ctl->label, "Paddle A") || !strcmp(ctl->label, "Paddle B") || !strcmp(ctl->label, "Driving")) {
                RECT plate = rect;
                plate.left += (int)(u * 4.85);
                plate.right = rect.left + (int)(u * 6.45);
                plate.top += (int)(u * 4.3);
                plate.bottom = rect.top + (int)(u * 5.6);
                SelectObject(hdc, g_legend);
                SetTextColor(hdc, tone(HX(0xE6E6E6), body_dim(ctl->port)));
                text_line(hdc, !strcmp(ctl->label, "Driving") ? "driving" : "paddle", &plate, DT_CENTER);
            } else if (!strcmp(ctl->label, "Keypad")) {
                RECT title = rect;
                title.top += (int)(u * 0.9);
                title.bottom = rect.top + (int)(u * 2.3);
                SelectObject(hdc, g_legend);
                SetTextColor(hdc, tone(HX(0x8CCFEA), body_dim(ctl->port)));
                title.bottom = title.top + (int)(u * 0.55);
                text_line(hdc, "Keyboard", &title, DT_CENTER);
                title.top = title.bottom;
                title.bottom = title.top + (int)(u * 0.55);
                text_line(hdc, "Controller", &title, DT_CENTER);
            } else if (!strcmp(ctl->label, "Console")) {
                RECT title = rect;
                title.left += (int)(u * 5.0);
                title.right -= (int)(u * 5.0);
                title.top += (int)(u * 0.4);
                title.bottom = rect.top + (int)(u * 1.3);
                SelectObject(hdc, g_title);
                SetTextColor(hdc, tone(HX(0xE6DFCF), body_dim(-1)));
                text_line(hdc, "video computer system", &title, DT_CENTER);
            } else if (!strcmp(ctl->label, "Wood")) {
                RECT logo = rect;
                logo.left = rect.right - (int)(u * 3.4);
                logo.right = rect.right - (int)(u * 0.6);
                SelectObject(hdc, g_logo);
                SetTextColor(hdc, tone(HX(0xC7C7C7), body_dim(-1)));
                text_line(hdc, "ATARI", &logo, DT_LEFT);
            }
            break;
        case A2600_CTL_ROUND_BUTTON:
            if (ctl->port >= 0 && ctl->target % A2600_ACT_PER_PORT != A2600_ACT_JOY_FIRE) {
                /* a keypad key: its legend printed up and to the left as on the card */
                RECT leg = rect;
                leg.left -= (int)(u * 0.5);
                leg.right = rect.left + (int)(u * 0.2);
                leg.top -= (int)(u * 0.62);
                leg.bottom = rect.top + (int)(u * 0.05);
                SelectObject(hdc, g_key);
                SetTextColor(hdc, part_lit(ctl) ? RGB(0xFF, 0xFF, 0xFF) : tone(HX(0x8CCFEA), dim));
                text_line(hdc, ctl->label, &leg, DT_CENTER);
            }
            text_binding_on(hdc, ctl, &rect, dim);
            break;
        case A2600_CTL_BUTTON:
            if (ctl->port < 0 && !strcmp(ctl->label, "STOP")) {
                if (binding_mode() && ctl->target >= 0) {
                    char legend[96];
                    binding_label(ctl->target, legend, sizeof legend);
                    SelectObject(hdc, g_tiny);
                    SetTextColor(hdc, g_map_state == ctl->target ? COL_HELD_RING : RGB(0xFF, 0xFF, 0xFF));
                    text_line(hdc, legend, &rect, DT_CENTER | DT_END_ELLIPSIS);
                } else {
                    SelectObject(hdc, g_legend);
                    SetTextColor(hdc, tone(HX(0xE6E6E6), dim));
                    text_line(hdc, "STOP", &rect, DT_CENTER);
                }
                break;
            }
            text_console_legend(hdc, ctl, &rect, dim);
            text_binding_on(hdc, ctl, &rect, dim);
            break;
        case A2600_CTL_SWITCH:
            text_console_legend(hdc, ctl, &rect, dim);
            text_binding_on(hdc, ctl, &rect, dim);
            break;
        case A2600_CTL_TURN:
            text_binding_on(hdc, ctl, &rect, dim);
            break;
        case A2600_CTL_STICK:
            if (binding_mode()) {
                /* the four directions' keys, round the ring */
                static const int dirs[4] = { A2600_ACT_JOY_LEFT, A2600_ACT_JOY_RIGHT, A2600_ACT_JOY_UP, A2600_ACT_JOY_DOWN };
                int dir;
                SelectObject(hdc, g_tiny);
                for (dir = 0; dir < 4; dir++) {
                    RECT box;
                    const int half = (int)(u * 0.7);
                    const int midx = (rect.left + rect.right) / 2, midy = (rect.top + rect.bottom) / 2;
                    const int target = A2600_TARGET_PORT(ctl->port, dirs[dir]);
                    int posx = midx, posy = midy;
                    if (dirs[dir] == A2600_ACT_JOY_LEFT) posx = rect.left - (int)(u * 0.55);
                    if (dirs[dir] == A2600_ACT_JOY_RIGHT) posx = rect.right + (int)(u * 0.55);
                    if (dirs[dir] == A2600_ACT_JOY_UP) posy = rect.top - (int)(u * 0.3);
                    if (dirs[dir] == A2600_ACT_JOY_DOWN) posy = rect.bottom + (int)(u * 0.3);
                    SetRect(&box, posx - half, posy - SCL(7), posx + half, posy + SCL(7));
                    binding_label(target, legend, sizeof legend);
                    SetTextColor(hdc, g_map_state == target ? COL_HELD_RING : RGB(0xFF, 0xFF, 0xFF));
                    text_line(hdc, legend, &box, DT_CENTER | DT_END_ELLIPSIS);
                }
            }
            break;
        default:
            break;
        }
    }
}

/* ---- the strip: gamepads, Map ---------------------------------------------------------- */

static HBRUSH accent_brush(void)
{
    static HBRUSH brush;
    if (!brush) brush = CreateSolidBrush(COL_HELD);
    return brush;
}

static void draw_face(HDC hdc, const RECT *bounds, int pushed, int accent, int inactive)
{
    RECT rect = *bounds;
    if (accent) {
        FillRect(hdc, &rect, accent_brush());
        FrameRect(hdc, &rect, (HBRUSH)GetStockObject(GRAY_BRUSH));
        SetTextColor(hdc, COL_ON_HELD);
    } else {
        DrawFrameControl(hdc, &rect, DFC_BUTTON,
                         DFCS_BUTTONPUSH | (pushed ? DFCS_PUSHED : 0) | (inactive ? DFCS_INACTIVE : 0));
        SetTextColor(hdc, GetSysColor(inactive ? COLOR_GRAYTEXT : COLOR_BTNTEXT));
    }
}

static const char *strip_label(int btn)
{
    switch (btn) {
    case SB_MAP: return g_map_state == -2 ? "Map" : "Done";
    case SB_CLEAR: return "Clear";
    case SB_DEFAULTS: return "Defaults";
    case SB_SHOW: return "Show bindings";
    default: return "Help";
    }
}

static void segment_rect(int row, int seg, RECT *rect)
{
    const RECT *rowrc = &g_gp_row_rc[row];
    const int gap = SCL(4), width = (rowrc->right - rowrc->left - (SEGMENTS - 1) * gap) / SEGMENTS;
    const int left = rowrc->left + seg * (width + gap);
    SetRect(rect, left, rowrc->top + SCL(NAME_H), left + width, rowrc->top + SCL(NAME_H) + SCL(SEG_H) - 1);
}

/* One block per gamepad: its name and the port it drives now over the
 * Automatic / Left port / Right port choice, the current one in orange. */
static void paint_gamepads(HDC hdc)
{
    static const char *const segs[SEGMENTS] = { "Automatic", "Left port", "Right port" };
    const int num = a2600session_gamepad_count(g_session);
    int idx, seg;

    SelectObject(hdc, g_bold);
    SetTextColor(hdc, GetSysColor(COLOR_BTNTEXT));
    text_line(hdc, "Gamepads", &g_gp_head_rc, DT_LEFT);
    SelectObject(hdc, g_font);
    SetTextColor(hdc, GetSysColor(COLOR_GRAYTEXT));
    if (num == 0) {
        text_line(hdc, "No gamepads connected - plug one in", &g_gp_row_rc[0], DT_LEFT);
        return;
    }
    if (num > PAD_ROWS) {
        char more[48];
        snprintf(more, sizeof more, "%d more not shown", num - PAD_ROWS);
        text_line(hdc, more, &g_gp_head_rc, DT_RIGHT);
    }
    for (idx = 0; idx < num && idx < PAD_ROWS; idx++) {
        const int assign = a2600session_gamepad_assignment(g_session, idx);
        const int effective = a2600session_gamepad_effective_port(g_session, idx);
        const int chosen = assign < 0 || assign > 1 ? 0 : assign + 1;
        char name[128], line[48];
        RECT name_rc, eff_rc;

        name_rc = g_gp_row_rc[idx];
        name_rc.bottom = name_rc.top + SCL(NAME_H);
        eff_rc = name_rc;
        name_rc.right -= SCL(150);
        eff_rc.left = name_rc.right;
        eff_rc.right -= SCL(6);
        a2600session_gamepad_name(g_session, idx, name, sizeof name);
        SelectObject(hdc, g_font);
        SetTextColor(hdc, GetSysColor(COLOR_BTNTEXT));
        text_line(hdc, name, &name_rc, DT_LEFT | DT_END_ELLIPSIS);
        if (effective >= 0) snprintf(line, sizeof line, "drives the %s port", effective == 0 ? "left" : "right");
        else snprintf(line, sizeof line, "drives nothing");
        SetTextColor(hdc, GetSysColor(COLOR_GRAYTEXT));
        text_line(hdc, line, &eff_rc, DT_RIGHT);
        for (seg = 0; seg < SEGMENTS; seg++) {
            RECT rect;
            segment_rect(idx, seg, &rect);
            draw_face(hdc, &rect, 0, seg == chosen, 0);
            text_line(hdc, segs[seg], &rect, DT_CENTER);
        }
    }
}

/* What the hint line says about the part under an idle pointer. */
static void hover_text(int part, char *out, int outsz)
{
    const a2600_control *ctl;
    out[0] = '\0';
    if (part < 0 || part >= g_numlayout) return;
    ctl = &g_layout[part];
    if (ctl->kind == A2600_CTL_STICK) {
        snprintf(out, (size_t)outsz, "Stick: drag to tilt it. Map: click a side of the ring to rebind that direction.");
        return;
    }
    if (ctl->kind == A2600_CTL_KNOB) {
        snprintf(out, (size_t)outsz, "Paddle knob: drag or scroll to turn it. It follows the analog stick too (Settings).");
        return;
    }
    if (ctl->kind == A2600_CTL_WHEEL) {
        snprintf(out, (size_t)outsz, "Driving wheel: drag round or scroll to turn it, a notch at a time.");
        return;
    }
    if (ctl->target >= 0) {
        const a2600_binding bind = a2600session_binding_get(g_session, ctl->target);
        char key[48], padname[48];
        key[0] = '\0';
        if (bind.keysym) a2600session_keysym_name(bind.keysym, key, sizeof key);
        snprintf(padname, sizeof padname, "%s", bind.button != A2600_PAD_BTN_NONE ? a2600_pad_button_name(bind.button) : "none");
        snprintf(out, (size_t)outsz, "%s   Key: %s   Gamepad: %s", a2600_target_name(ctl->target), key[0] ? key : "none", padname);
    }
}

static void paint_strip(HDC hdc)
{
    int btn;
    char hover[200];
    SelectObject(hdc, g_font);
    for (btn = 0; btn < SB_COUNT; btn++) {
        const int armed = (btn == SB_MAP && g_map_state != -2) || (btn == SB_SHOW && g_show_bindings);
        const int inactive = btn == SB_CLEAR && g_map_state < 0;
        draw_face(hdc, &g_btn_rc[btn], 0, armed, inactive);
        text_line(hdc, strip_label(btn), &g_btn_rc[btn], DT_CENTER);
    }
    SetTextColor(hdc, GetSysColor(COLOR_GRAYTEXT));
    hover_text(g_map_state >= 0 ? -1 : g_hover_part, hover, sizeof hover);
    text_line(hdc, hover[0] ? hover : g_hint, &g_hint_rc, DT_LEFT | DT_END_ELLIPSIS);
}

/* The whole window: the art supersampled, its text, the strips. Only what the
 * update region needs is drawn. */
static void paint_rect(HDC hdc, const RECT *inval)
{
    RECT art;
    const int scale = 2;
    HDC mem;
    HBITMAP bmp;
    void *bits = NULL;
    BITMAPINFO bmi;
    HGDIOBJ old;

    FillRect(hdc, inval, GetSysColorBrush(COLOR_BTNFACE));
    SetBkMode(hdc, TRANSPARENT);

    /* the art: the intersection with the mat, at 2x */
    IntersectRect(&art, inval, &g_case_rc);
    if (!IsRectEmpty(&art)) {
        const int width = art.right - art.left, height = art.bottom - art.top;
        memset(&bmi, 0, sizeof bmi);
        bmi.bmiHeader.biSize = sizeof bmi.bmiHeader;
        bmi.bmiHeader.biWidth = width * scale;
        bmi.bmiHeader.biHeight = -height * scale;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        mem = CreateCompatibleDC(hdc);
        bmp = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
        if (bmp) {
            HBRUSH face = CreateSolidBrush(GetSysColor(COLOR_BTNFACE));
            RECT all = { 0, 0, width * scale, height * scale };
            old = SelectObject(mem, bmp);
            FillRect(mem, &all, face);
            DeleteObject(face);
            SetBkMode(mem, TRANSPARENT);
            g_art_k = scale;
            g_art_x = art.left;
            g_art_y = art.top;
            g_art_rows = height * scale;
            art_case(mem);
            g_art_k = 1;
            SetStretchBltMode(hdc, HALFTONE);
            SetBrushOrgEx(hdc, 0, 0, NULL);
            StretchBlt(hdc, art.left, art.top, width, height, mem, 0, 0, width * scale, height * scale, SRCCOPY);
            SelectObject(mem, old);
            DeleteObject(bmp);
        }
        DeleteDC(mem);
        SelectObject(hdc, g_font);
        text_parts(hdc);
    }
    paint_gamepads(hdc);
    paint_strip(hdc);
}

/* ---- pressing ----------------------------------------------------------------- */

static void invalidate_part(int part)
{
    RECT rect;
    if (!g_panel || part < 0 || part >= g_numlayout) return;
    part_rect(&g_layout[part], &rect);
    InflateRect(&rect, SCL(20), SCL(20));
    InvalidateRect(g_panel, &rect, FALSE);
}

/* The mat: any of its parts changed. */
static void invalidate_view(void)
{
    if (!g_panel) return;
    InvalidateRect(g_panel, &g_case_rc, FALSE);
}

/* The stick, a knob or a wheel is being dragged: update what the pointer holds. */
static void stick_follow(const a2600_control *ctl, POINT pnt)
{
    double bx, by, bw, bh, delx, dely, len;
    const double reach = (double)ctl->w * g_unit / 2;
    part_box(ctl, &bx, &by, &bw, &bh);
    delx = pnt.x - (bx + bw / 2);
    dely = pnt.y - (by + bh / 2);
    len = sqrt(delx * delx + dely * dely);
    g_stick_dx = delx / reach;
    g_stick_dy = dely / reach;
    if (len > reach) { g_stick_dx = delx / len; g_stick_dy = dely / len; }
    a2600session_pointer_set(g_session, ctl->port, a2600session_stick_from_vector((float)delx, (float)dely, (float)reach));
    refresh_held();
}

static void knob_follow(const a2600_control *ctl, POINT pnt)
{
    double bx, by, bw, bh;
    part_box(ctl, &bx, &by, &bw, &bh);
    a2600session_pointer_analog(g_session, ctl->port, ctl->paddle,
                                a2600session_knob_from_vector((float)(pnt.x - (bx + bw / 2)), (float)(pnt.y - (by + bh / 2))), 1);
    refresh_held();
}

/* The wheel turns endlessly: one twist pulse for every notch of drag. */
static void wheel_follow(const a2600_control *ctl, POINT pnt)
{
    double bx, by, bw, bh, delta;
    float now;
    part_box(ctl, &bx, &by, &bw, &bh);
    now = a2600session_vector_angle((float)(pnt.x - (bx + bw / 2)), (float)(pnt.y - (by + bh / 2)));
    delta = now - g_wheel_last;
    if (delta > 180) delta -= 360;
    if (delta < -180) delta += 360;
    g_wheel_last = now;
    g_wheel_deg[ctl->port & 1] += (float)delta;
    g_wheel_acc += (float)delta;
    while (g_wheel_acc >= A2600_WHEEL_STEP_DEG) { a2600session_twist_pulse(g_session, ctl->port, 1); g_wheel_acc -= A2600_WHEEL_STEP_DEG; }
    while (g_wheel_acc <= -A2600_WHEEL_STEP_DEG) { a2600session_twist_pulse(g_session, ctl->port, -1); g_wheel_acc += A2600_WHEEL_STEP_DEG; }
    invalidate_view();
}

/* Lets go of what the pointer holds in this window (only that: the main
 * window releases its own keys). POWER and STOP fire on release inside them. */
static void mouse_release(int fire_action)
{
    const int part = g_mouse_part, action = g_mouse_action, stick = g_mouse_stick, knob = g_mouse_knob;
    const int wheel = g_mouse_wheel;
    g_mouse_part = -1;
    g_mouse_stick = -1;
    g_mouse_knob = -1;
    g_mouse_wheel = -1;
    g_mouse_action = -1;
    g_wheel_acc = 0;
    if (part >= 0 && part < g_numlayout && g_layout[part].target >= 0)
        a2600session_press(g_session, g_layout[part].target, 0);
    if (stick >= 0) a2600session_pointer_set(g_session, stick, 0);
    if (knob >= 0 && knob < g_numlayout)
        a2600session_pointer_analog(g_session, g_layout[knob].port, g_layout[knob].paddle, 0, 0);
    if (action >= 0 && action < g_numlayout && fire_action) {
        const int target = g_layout[action].target;
        if (target >= A2600_TARGET_SYSACT(0))
            a2600session_sysaction_post(g_session, target - A2600_TARGET_SYSACT(0));
    }
    if (part >= 0) invalidate_part(part);
    if (action >= 0) invalidate_part(action);
    if (stick >= 0 || knob >= 0 || wheel >= 0) invalidate_view();
}

static void set_hint(const char *text)
{
    snprintf(g_hint, sizeof g_hint, "%s", text);
    g_hover_part = -1;
    if (g_panel) InvalidateRect(g_panel, &g_hint_rc, FALSE);
}

static const char *idle_hint(void)
{
    return "Drag the stick or a knob, press a button to hold it. Map rebinds.";
}

static void set_map_state(int state)
{
    g_map_state = state;
    if (state < 0) {
        if (g_panel) KillTimer(g_panel, IDT_CAPTURE);
        a2600session_gamepad_capture_cancel(g_session);
    }
    if (state == -2) {
        set_hint(idle_hint());
    } else if (state == -1) {
        set_hint("Click a control (or pick one from the list) to remap. Esc finishes.");
    } else {
        char msg[200];
        mouse_release(0);
        snprintf(msg, sizeof msg, "Press a key or a gamepad button for %s. Esc cancels.", a2600_target_name(state));
        a2600session_gamepad_capture_begin(g_session);
        set_hint(msg);
        if (g_panel) SetTimer(g_panel, IDT_CAPTURE, 50, NULL);
    }
    if (g_combo) {
        EnableWindow(g_combo, state != -2);
        SendMessageA(g_combo, CB_SETCURSEL, (WPARAM)(state >= 0 ? state + 1 : 0), 0);
    }
    if (g_panel) {
        InvalidateRect(g_panel, NULL, FALSE);
        refresh_held();
    }
}

/* After a rebind: what it is now, and whom it was taken from. */
static void bound_hint(const char *what, const char *stolen)
{
    char msg[260];
    if (stolen && stolen[0]) snprintf(msg, sizeof msg, "Bound %s (was %s)", what, stolen);
    else snprintf(msg, sizeof msg, "Bound %s", what);
    set_hint(msg);
}

/* ---- layout --------------------------------------------------------------------- */

/* The layout for what is attached now. The extent is the same for every
 * combination, so only the part table changes, never the window. */
static void fetch_layout(void)
{
    g_view[0] = a2600session_view_type(g_session, 0);
    g_view[1] = a2600session_view_type(g_session, 1);
    g_layout = a2600session_controls_layout(g_view[0], g_view[1], &g_numlayout, &g_layw, &g_layh);
}

/* The mat with the controllers in it, the gamepad rows, the Map row: the window
 * is this size at this DPI. */
static void layout_static(void)
{
    const int left = SCL(MARGIN);
    int ypos = SCL(MARGIN), idx, xpos, content_w;
    static const int widths[SB_COUNT] = { MAP_W, CLEAR_W, DEFAULTS_W, SHOW_W, HELP_W };

    fetch_layout();
    g_unit = (double)UNIT96 * g_dpi / 96.0;
    content_w = (int)ceil((double)g_layw * g_unit) + 2 * SCL(CASE_PAD);
    g_total_w = 2 * left + content_w;

    SetRect(&g_case_rc, left, ypos, left + content_w, 0);
    SetRect(&g_brand_rc, g_case_rc.left + SCL(12), g_case_rc.top + SCL(8), g_case_rc.right - SCL(12),
            g_case_rc.top + SCL(8) + SCL(BRAND_H));
    g_orgx = g_case_rc.left + (content_w - (double)g_layw * g_unit) / 2;
    g_orgy = g_brand_rc.bottom + SCL(8);
    g_case_rc.bottom = (int)ceil(g_orgy + (double)g_layh * g_unit) + SCL(CASE_PAD);
    ypos = g_case_rc.bottom + SCL(10);

    SetRect(&g_gp_head_rc, left, ypos, left + content_w, ypos + SCL(HEAD_H));
    ypos += SCL(HEAD_H);
    for (idx = 0; idx < PAD_ROWS; idx++) {
        SetRect(&g_gp_row_rc[idx], left, ypos, left + content_w, ypos + SCL(NAME_H + SEG_H));
        ypos += SCL(NAME_H + SEG_H + ROW_GAP);
    }
    SetRect(&g_gp_rc, left, g_gp_head_rc.top, left + content_w, ypos);
    ypos += SCL(4);

    xpos = left;
    for (idx = 0; idx < SB_HELP; idx++) {
        SetRect(&g_btn_rc[idx], xpos, ypos, xpos + SCL(widths[idx]), ypos + SCL(BTN_H));
        xpos += SCL(widths[idx]) + SCL(6);
    }
    SetRect(&g_btn_rc[SB_HELP], left + content_w - SCL(HELP_W), ypos, left + content_w, ypos + SCL(BTN_H));
    SetRect(&g_combo_rc, xpos, ypos, g_btn_rc[SB_HELP].left - SCL(6), ypos + SCL(BTN_H));
    ypos += SCL(BTN_H) + SCL(6);
    SetRect(&g_hint_rc, left, ypos, left + content_w, ypos + SCL(20));
    g_total_h = ypos + SCL(20) + SCL(MARGIN);

    if (g_combo) {
        MoveWindow(g_combo, g_combo_rc.left, g_combo_rc.top, g_combo_rc.right - g_combo_rc.left,
                   SCL(BTN_H) + SCL(300), TRUE);
        SendMessageA(g_combo, WM_SETFONT, (WPARAM)g_font, TRUE);
    }
}

/* Exactly the laid-out client size, whatever the frame is at this DPI. */
static void fit_client(void)
{
    RECT wnd, client;
    GetWindowRect(g_panel, &wnd);
    GetClientRect(g_panel, &client);
    if (client.right == g_total_w && client.bottom == g_total_h) return;
    SetWindowPos(g_panel, NULL, 0, 0, (wnd.right - wnd.left) - client.right + g_total_w,
                 (wnd.bottom - wnd.top) - client.bottom + g_total_h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

/* The window's outer size for the laid-out client. */
static void window_size(HWND hwnd, SIZE *dims)
{
    RECT wnd, client;
    if (hwnd && GetWindowRect(hwnd, &wnd) && GetClientRect(hwnd, &client) && client.right > 0) {
        dims->cx = (wnd.right - wnd.left) - client.right + g_total_w;
        dims->cy = (wnd.bottom - wnd.top) - client.bottom + g_total_h;
    } else {
        SetRect(&wnd, 0, 0, g_total_w, g_total_h);
        AdjustWindowRectEx(&wnd, PANEL_STYLE, FALSE, PANEL_EXSTYLE);
        dims->cx = wnd.right - wnd.left;
        dims->cy = wnd.bottom - wnd.top;
    }
}

static void rebuild(void)
{
    mouse_release(0);
    if (g_panel && GetCapture() == g_panel) ReleaseCapture();
    make_fonts();
    layout_static();
    if (g_panel) {
        InvalidateRect(g_panel, NULL, FALSE);
        refresh_held();
    }
}

static void rescale(int dpi)
{
    g_dpi = dpi;
    rebuild();
}

/* ---- polling -------------------------------------------------------------------------- */

/* A port's controller changed (a game swapped it, or Settings did): the parts
 * differ, the window does not. */
static void check_views(void)
{
    const int left = a2600session_view_type(g_session, 0), right = a2600session_view_type(g_session, 1);
    if (left == g_view[0] && right == g_view[1]) return;
    mouse_release(0);
    if (g_panel && GetCapture() == g_panel) ReleaseCapture();
    fetch_layout();
    g_hover_part = -1;
    if (g_map_state >= 0) set_map_state(-1);
    memset(&g_shown, 0, sizeof g_shown);
    if (g_panel) InvalidateRect(g_panel, &g_case_rc, FALSE);
}

/* What the machine sees held, lit. */
static void refresh_held(void)
{
    held_state now;
    int port, paddle, sw;
    if (!g_panel) return;
    check_views();
    memset(&now, 0, sizeof now);
    for (port = 0; port < 2; port++) {
        now.held[port] = a2600session_buttons_held(g_session, port);
        for (paddle = 0; paddle < 2; paddle++) now.analog[port][paddle] = a2600session_analog_get(g_session, port, paddle);
        now.wheel[port] = a2600session_analog_get(g_session, port, -1);
        /* a held turn key spins the wheel on the screen */
        if (g_mouse_wheel < 0) {
            if (now.held[port] & (1u << A2600_ACT_DRIVE_CW)) g_wheel_deg[port] += 4.0f;
            if (now.held[port] & (1u << A2600_ACT_DRIVE_CCW)) g_wheel_deg[port] -= 4.0f;
            if (now.wheel[port]) g_wheel_deg[port] = (float)(now.wheel[port] / 32767.0 * A2600_KNOB_TRAVEL_DEG);
        }
    }
    for (sw = 0; sw < A2600_SW_COUNT; sw++)
        now.sw[sw] = (unsigned char)(sw == A2600_SW_SELECT || sw == A2600_SW_RESET ? a2600session_switch_held(g_session, sw) != 0
                                                                                    : a2600session_switch_get(g_session, sw) != 0);
    if (memcmp(&now, &g_shown, sizeof now) != 0) {
        g_shown = now;
        invalidate_view();
    }
}

/* Hot-plugged pads, auto-assignment changes, a controller swapped. */
static void refresh_pads(void)
{
    pads_state now;
    int idx;
    memset(&now, 0, sizeof now);
    now.generation = a2600session_gamepad_generation(g_session);
    now.views[0] = g_view[0];
    now.views[1] = g_view[1];
    now.count = a2600session_gamepad_count(g_session);
    for (idx = 0; idx < PAD_ROWS && idx < now.count; idx++) {
        now.assign[idx] = a2600session_gamepad_assignment(g_session, idx);
        now.effective[idx] = a2600session_gamepad_effective_port(g_session, idx);
    }
    if (memcmp(&now, &g_pads_shown, sizeof now) != 0) {
        g_pads_shown = now;
        InvalidateRect(g_panel, &g_gp_rc, FALSE);
    }
}

/* ---- keys ------------------------------------------------------------------------------ */

static void hide_panel(void)
{
    set_map_state(-2);
    mouse_release(0);
    if (GetCapture() == g_panel) ReleaseCapture();
    g_swallow_vkey = 0;
    a2600session_release_all(g_session);
    ShowWindow(g_panel, SW_HIDE);
}

/* The app's own keys, never the machine's or Map's. */
static int app_key(WPARAM vkey)
{
    return vkey == VK_F9 || vkey == VK_F11 || vkey == VK_F12;
}

/* A key while Map is armed: 1 if Map took it. Waiting for a target, a key is
 * not typing; waiting for a binding, the key is the binding (not F9, F11 or
 * F12, which are the app's). */
static int map_key(UINT msg, WPARAM wparam, LPARAM lparam)
{
    const int down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;

    if (g_swallow_vkey && wparam == g_swallow_vkey) {
        if (!down) g_swallow_vkey = 0;
        return 1;
    }
    if (g_map_state == -2) return 0;
    if (app_key(wparam) || wparam == VK_F10) return 0;
    if (msg == WM_SYSKEYDOWN && wparam == VK_F4) return 0;
    if (GetKeyState(VK_CONTROL) & 0x8000) return 0;       /* Ctrl chords are the app's */
    if (!down || (lparam & (1 << 30))) return 1;
    if (wparam == VK_ESCAPE) {
        set_map_state(g_map_state >= 0 ? -1 : -2);
        g_swallow_vkey = wparam;
        return 1;
    }
    if (g_map_state >= 0) {
        const int target = g_map_state;
        const uint32_t keysym = a2600_keysym_from_msg(wparam, lparam);
        if (!keysym) return 1;          /* nothing to call it: keep waiting */
        if (a2600session_key_reserved(keysym)) {
            set_hint("F9, F11 and F12 are the app's and cannot be bound.");
            g_swallow_vkey = wparam;
            return 1;
        }
        {
            char stolen[128], name[48];
            stolen[0] = '\0';
            a2600session_binding_set_key(g_session, target, keysym, stolen, sizeof stolen);
            a2600session_keysym_name(keysym, name, sizeof name);
            g_swallow_vkey = wparam;
            set_map_state(-1);          /* stay armed: remapping several in a row is normal */
            bound_hint(name, stolen);
        }
    }
    return 1;
}

/* A key with the window focused: 1 if taken. Anything Map does not take goes
 * to the main window's key path, so a key plays exactly as typed there. */
static int panel_key(UINT msg, WPARAM wparam, LPARAM lparam)
{
    if (map_key(msg, wparam, lparam)) return 1;
    if (g_parent) {
        SendMessageA(g_parent, msg, wparam, lparam);
        return 1;
    }
    return 0;
}

/* ---- the mouse ------------------------------------------------------------------------ */

/* The layout part under a client point, or -1. */
static int part_at(POINT pnt)
{
    if (!g_layout) return -1;
    return a2600session_controls_hit(g_view[0], g_view[1], (float)((pnt.x - g_orgx) / g_unit), (float)((pnt.y - g_orgy) / g_unit));
}

/* The binding target Map picks for a click on a part, or -1 for a part that has none. */
static int map_target_for(const a2600_control *ctl, POINT pnt)
{
    double xpos, ypos, width, height;
    switch (ctl->kind) {
    case A2600_CTL_BUTTON:
    case A2600_CTL_ROUND_BUTTON:
    case A2600_CTL_TURN:
    case A2600_CTL_SWITCH:
        return ctl->target;
    case A2600_CTL_STICK: {
        /* which side of the ring: the larger of the two offsets from its middle */
        double delx, dely;
        part_box(ctl, &xpos, &ypos, &width, &height);
        delx = (pnt.x - (xpos + width / 2)) / width;
        dely = (pnt.y - (ypos + height / 2)) / height;
        if (fabs(delx) > fabs(dely)) return A2600_TARGET_PORT(ctl->port, delx < 0 ? A2600_ACT_JOY_LEFT : A2600_ACT_JOY_RIGHT);
        return A2600_TARGET_PORT(ctl->port, dely < 0 ? A2600_ACT_JOY_UP : A2600_ACT_JOY_DOWN);
    }
    default:
        return -1;
    }
}

static void mouse_down(HWND hwnd, POINT pnt)
{
    int part, row, seg, btn;
    SetFocus(hwnd);
    if (g_hover_part >= 0) {
        g_hover_part = -1;
        InvalidateRect(hwnd, &g_hint_rc, FALSE);
    }

    for (btn = 0; btn < SB_COUNT; btn++) {
        if (!PtInRect(&g_btn_rc[btn], pnt)) continue;
        switch (btn) {
        case SB_MAP: set_map_state(g_map_state == -2 ? -1 : -2); return;
        case SB_CLEAR:
            if (g_map_state >= 0) {
                a2600session_binding_clear(g_session, g_map_state);
                set_hint("Cleared. Press a key or a gamepad button to bind it again");
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return;
        case SB_DEFAULTS:
            a2600session_bindings_reset(g_session);
            set_hint("Every key and button is back to its default");
            InvalidateRect(hwnd, NULL, FALSE);
            return;
        case SB_SHOW:
            g_show_bindings = !g_show_bindings;
            a2600session_set_int(g_session, "controllers_show_bindings", g_show_bindings);
            InvalidateRect(hwnd, NULL, FALSE);
            return;
        default: a2600_help_show(hwnd, "controllers"); return;
        }
    }
    for (row = 0; row < PAD_ROWS && row < a2600session_gamepad_count(g_session); row++)
        for (seg = 0; seg < SEGMENTS; seg++) {
            RECT rect;
            segment_rect(row, seg, &rect);
            if (PtInRect(&rect, pnt)) {
                /* 0 = automatic (connection order), 1 = the left port, 2 = the right */
                a2600session_gamepad_assign(g_session, row, seg - 1);
                refresh_pads();
                InvalidateRect(hwnd, &g_gp_rc, FALSE);
                return;
            }
        }

    part = part_at(pnt);
    if (part < 0) return;
    {
        const a2600_control *ctl = &g_layout[part];
        if (g_map_state != -2) {
            /* armed: picks (or changes) the target */
            const int target = map_target_for(ctl, pnt);
            if (target >= 0) set_map_state(target);
            else if (ctl->kind == A2600_CTL_KNOB || ctl->kind == A2600_CTL_WHEEL)
                set_hint("The knob follows the analog stick (Settings); its turn arrows are the rebindable part.");
            return;
        }
        mouse_release(0);
        switch (ctl->kind) {
        case A2600_CTL_STICK:
            g_mouse_stick = ctl->port;
            stick_follow(ctl, pnt);
            break;
        case A2600_CTL_KNOB:
            g_mouse_knob = part;
            knob_follow(ctl, pnt);
            break;
        case A2600_CTL_WHEEL: {
            double bx, by, bw, bh;
            part_box(ctl, &bx, &by, &bw, &bh);
            g_mouse_wheel = part;
            g_wheel_acc = 0;
            g_wheel_last = a2600session_vector_angle((float)(pnt.x - (bx + bw / 2)), (float)(pnt.y - (by + bh / 2)));
            break;
        }
        case A2600_CTL_BUTTON:
        case A2600_CTL_ROUND_BUTTON:
        case A2600_CTL_TURN:
        case A2600_CTL_SWITCH:
            if (ctl->target >= A2600_TARGET_SYSACT(0)) {
                g_mouse_action = part;       /* fires on release inside it */
            } else {
                g_mouse_part = part;
                a2600session_press(g_session, ctl->target, 1);
            }
            break;
        default:
            return;
        }
        invalidate_part(part);
        SetCapture(hwnd);
        refresh_held();
    }
}

static void mouse_move(HWND hwnd, POINT pnt)
{
    if (g_mouse_stick >= 0) {
        int idx;
        for (idx = 0; idx < g_numlayout; idx++)
            if (g_layout[idx].kind == A2600_CTL_STICK && g_layout[idx].port == g_mouse_stick) stick_follow(&g_layout[idx], pnt);
        return;
    }
    if (g_mouse_knob >= 0 && g_mouse_knob < g_numlayout) {
        knob_follow(&g_layout[g_mouse_knob], pnt);
        return;
    }
    if (g_mouse_wheel >= 0 && g_mouse_wheel < g_numlayout) {
        wheel_follow(&g_layout[g_mouse_wheel], pnt);
        return;
    }
    if (g_mouse_part >= 0) {
        /* dragged off what it holds: let go, as a button does */
        if (g_mouse_part >= g_numlayout || part_at(pnt) != g_mouse_part) {
            mouse_release(0);
            if (GetCapture() == hwnd) ReleaseCapture();
            refresh_held();
        }
        return;
    }
    if (g_mouse_action >= 0) {
        if (part_at(pnt) != g_mouse_action) {
            mouse_release(0);
            if (GetCapture() == hwnd) ReleaseCapture();
        }
        return;
    }
    {   /* idle: the hint line says what drives the part under the pointer */
        const int part = part_at(pnt);
        if (!g_tracking) {
            TRACKMOUSEEVENT track;
            memset(&track, 0, sizeof track);
            track.cbSize = sizeof track;
            track.dwFlags = TME_LEAVE;
            track.hwndTrack = hwnd;
            g_tracking = TrackMouseEvent(&track) != 0;
        }
        if (part != g_hover_part) {
            g_hover_part = part;
            InvalidateRect(hwnd, &g_hint_rc, FALSE);
        }
    }
}

static void mouse_up(HWND hwnd)
{
    if (g_mouse_part >= 0 || g_mouse_stick >= 0 || g_mouse_knob >= 0 || g_mouse_wheel >= 0 || g_mouse_action >= 0) {
        mouse_release(1);
        refresh_held();
    }
    if (GetCapture() == hwnd) ReleaseCapture();
}

/* The wheel over a knob nudges it; over the driving wheel it notches it. */
static void mouse_wheel(HWND hwnd, int delta, POINT screen)
{
    POINT pnt = screen;
    int part;
    ScreenToClient(hwnd, &pnt);
    if (g_map_state != -2) return;
    part = part_at(pnt);
    if (part < 0) return;
    if (g_layout[part].kind == A2600_CTL_KNOB) {
        int value = a2600session_analog_get(g_session, g_layout[part].port, g_layout[part].paddle) + (int)((long)delta * 2048 / WHEEL_DELTA);
        if (value > 32767) value = 32767;
        if (value < -32767) value = -32767;
        a2600session_pointer_analog(g_session, g_layout[part].port, g_layout[part].paddle, value, 1);
        a2600session_pointer_analog(g_session, g_layout[part].port, g_layout[part].paddle, value, 0);
        refresh_held();
    } else if (g_layout[part].kind == A2600_CTL_WHEEL) {
        const int dir = delta > 0 ? -1 : 1;
        a2600session_twist_pulse(g_session, g_layout[part].port, dir);
        g_wheel_deg[g_layout[part].port & 1] += dir * A2600_WHEEL_STEP_DEG;
        invalidate_view();
    }
}

/* ---- the window ----------------------------------------------------------------------- */

static LRESULT CALLBACK panel_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT paint;
        HDC hdc = BeginPaint(hwnd, &paint);
        /* Off-screen first: the held highlights repaint at 25 Hz. */
        RECT inval = paint.rcPaint;
        HDC mem;
        HBITMAP bmp;
        HGDIOBJ oldbmp, oldfont;
        const int width = inval.right - inval.left, height = inval.bottom - inval.top;
        if (width <= 0 || height <= 0) { EndPaint(hwnd, &paint); return 0; }
        mem = CreateCompatibleDC(hdc);
        bmp = CreateCompatibleBitmap(hdc, width, height);
        oldbmp = SelectObject(mem, bmp);
        oldfont = SelectObject(mem, g_font);
        SetViewportOrgEx(mem, -inval.left, -inval.top, NULL);
        paint_rect(mem, &inval);
        SetViewportOrgEx(mem, 0, 0, NULL);
        BitBlt(hdc, inval.left, inval.top, width, height, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldfont);
        SelectObject(mem, oldbmp);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &paint);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;

    case WM_LBUTTONDOWN: {
        POINT pnt;
        pnt.x = GET_X_LPARAM(lparam);
        pnt.y = GET_Y_LPARAM(lparam);
        mouse_down(hwnd, pnt);
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT pnt;
        pnt.x = GET_X_LPARAM(lparam);
        pnt.y = GET_Y_LPARAM(lparam);
        mouse_move(hwnd, pnt);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        POINT pnt;
        pnt.x = GET_X_LPARAM(lparam);
        pnt.y = GET_Y_LPARAM(lparam);
        mouse_wheel(hwnd, GET_WHEEL_DELTA_WPARAM(wparam), pnt);
        return 0;
    }
    case WM_MOUSELEAVE:
        g_tracking = 0;
        if (g_hover_part >= 0) {
            g_hover_part = -1;
            InvalidateRect(hwnd, &g_hint_rc, FALSE);
        }
        return 0;
    case WM_LBUTTONUP:
        /* released wherever the mouse ended up: capture means this arrives
         * even if the pointer left the part */
        mouse_up(hwnd);
        return 0;
    case WM_CAPTURECHANGED:
        /* lost the mouse some other way (Alt+Tab mid-press): let go */
        if ((HWND)lparam != hwnd) {
            mouse_release(0);
            refresh_held();
        }
        return 0;

    case WM_COMMAND:
        if (LOWORD(wparam) == IDC_CTRL_TARGETS && HIWORD(wparam) == CBN_SELCHANGE) {
            const int sel = (int)SendMessageA(g_combo, CB_GETCURSEL, 0, 0);
            if (sel > 0 && g_map_state != -2) set_map_state(sel - 1);
            SetFocus(hwnd);
        }
        return 0;

    case WM_TIMER:
        if (wparam == IDT_CAPTURE) {
            int button;
            if (g_map_state < 0) {
                KillTimer(hwnd, IDT_CAPTURE);
            } else if (a2600session_gamepad_capture_poll(g_session, &button)) {
                char stolen[128];
                const int target = g_map_state;
                stolen[0] = '\0';
                a2600session_binding_set_button(g_session, target, button, stolen, sizeof stolen);
                set_map_state(-1);   /* stay armed: remapping several in a row is normal */
                bound_hint(a2600_pad_button_name(button), stolen);
            }
        } else if (wparam == IDT_HELD) {
            refresh_held();
        } else if (wparam == IDT_PADS) {
            refresh_pads();
        }
        return 0;

    /* WM_SYSKEY* too: Alt chords are the main window's (the difficulty switches) */
    case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_KEYUP: case WM_SYSKEYUP:
        if (panel_key(msg, wparam, lparam)) return 0;
        break;
    case WM_CHAR:
        return 0;           /* typing arrives as keys, not characters */
    case WM_SYSCHAR:
        return 0;           /* Alt+letter is not a menu mnemonic here */

    case WM_ACTIVATE:
        if (LOWORD(wparam) == WA_INACTIVE) {
            mouse_release(0);
            if (GetCapture() == hwnd) ReleaseCapture();
            g_swallow_vkey = 0;     /* its release goes elsewhere */
            a2600session_release_all(g_session);
            refresh_held();
        }
        break;                      /* DefWindowProc gives the window the focus */
    case WM_SHOWWINDOW:
        if (wparam) {
            refresh_held();
            SetTimer(hwnd, IDT_HELD, 40, NULL);
            SetTimer(hwnd, IDT_PADS, 250, NULL);
        } else {
            KillTimer(hwnd, IDT_HELD);
            KillTimer(hwnd, IDT_PADS);
        }
        break;
    case WM_GETMINMAXINFO:
        /* Fixed: the size limits say so too, so neither maximizing nor a
         * compositor (Wine passes them on) makes it any other size. */
        if (g_total_w) {
            MINMAXINFO *limits = (MINMAXINFO *)lparam;
            SIZE dims;
            window_size(hwnd, &dims);
            limits->ptMinTrackSize.x = limits->ptMaxTrackSize.x = limits->ptMaxSize.x = dims.cx;
            limits->ptMinTrackSize.y = limits->ptMaxTrackSize.y = limits->ptMaxSize.y = dims.cy;
            return 0;
        }
        break;
    case WM_DPICHANGED: {
        const RECT *suggested = (const RECT *)lparam;
        rescale(HIWORD(wparam));
        SetWindowPos(hwnd, NULL, suggested->left, suggested->top, suggested->right - suggested->left,
                     suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        fit_client();
        return 0;
    }
    case WM_CLOSE:
        /* Hide, do not destroy: a remap and the position survive closing it. */
        hide_panel();
        return 0;
    case WM_DESTROY:
        /* with the main window: the next toggle starts afresh */
        mouse_release(0);
        set_map_state(-2);
        g_panel = NULL;
        g_combo = NULL;
        free_fonts();
        break;
    default:
        break;
    }
    return DefWindowProcA(hwnd, msg, wparam, lparam);
}

static int create_panel(void)
{
    static int registered;
    HINSTANCE inst = GetModuleHandleA(NULL);
    RECT want;
    int idx;

    g_show_bindings = a2600session_get_int(g_session, "controllers_show_bindings", 0) != 0;

    if (!registered) {
        WNDCLASSEXA wcls;
        memset(&wcls, 0, sizeof wcls);
        wcls.cbSize = sizeof wcls;
        wcls.lpfnWndProc = panel_proc;      /* no CS_DBLCLKS: fast clicks stay press/release pairs */
        wcls.hInstance = inst;
        wcls.hCursor = LoadCursor(NULL, IDC_ARROW);
        wcls.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wcls.lpszClassName = PANEL_CLASS;
        registered = RegisterClassExA(&wcls) != 0;
    }
    g_dpi = a2600_window_dpi(g_parent);
    make_fonts();
    layout_static();

    SetRect(&want, 0, 0, g_total_w, g_total_h);
    /* A tool window with a caption and no thick frame or maximize box: it is
     * exactly the size of its contents, and stays off the taskbar. */
    AdjustWindowRectEx(&want, PANEL_STYLE, FALSE, PANEL_EXSTYLE);
    g_panel = CreateWindowExA(PANEL_EXSTYLE, PANEL_CLASS, "Controllers", PANEL_STYLE,
                              CW_USEDEFAULT, CW_USEDEFAULT, want.right - want.left, want.bottom - want.top,
                              g_parent, NULL, inst, NULL);
    if (!g_panel) return 0;
    /* the target list: every binding target, for the ones the picture has no part for */
    g_combo = CreateWindowExA(0, "COMBOBOX", "", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST,
                              g_combo_rc.left, g_combo_rc.top, g_combo_rc.right - g_combo_rc.left, SCL(BTN_H) + SCL(300),
                              g_panel, (HMENU)(INT_PTR)IDC_CTRL_TARGETS, inst, NULL);
    if (g_combo) {
        SendMessageA(g_combo, CB_ADDSTRING, 0, (LPARAM)"Pick a target to remap...");
        for (idx = 0; idx < A2600_TARGET_COUNT; idx++)
            SendMessageA(g_combo, CB_ADDSTRING, 0, (LPARAM)a2600_target_name(idx));
        SendMessageA(g_combo, CB_SETCURSEL, 0, 0);
        SendMessageA(g_combo, WM_SETFONT, (WPARAM)g_font, TRUE);
        EnableWindow(g_combo, FALSE);
    }
    a2600_help_set_context(g_panel, "controllers", 0);    /* F1 is the console's SELECT here */
    rescale(a2600_window_dpi(g_panel));    /* placed on another monitor than the main window */
    fit_client();
    memset(&g_pads_shown, 0, sizeof g_pads_shown);
    memset(&g_shown, 0, sizeof g_shown);
    g_hover_part = -1;
    set_map_state(-2);
    return 1;
}

static void show_panel(void)
{
    refresh_pads();
    ShowWindow(g_panel, SW_SHOW);
    SetForegroundWindow(g_panel);
}

void a2600_controllers_window_show(HWND parent, a2600session *session)
{
    g_session = session;
    if (parent)
        g_parent = parent;
    if (!g_panel && !create_panel())
        return;
    if (!IsWindowVisible(g_panel))
        show_panel();
    else
        SetForegroundWindow(g_panel);
}

void a2600_controllers_window_toggle(HWND parent, a2600session *session)
{
    g_session = session;
    if (parent)
        g_parent = parent;
    if (!g_panel && !create_panel())
        return;
    if (IsWindowVisible(g_panel))
        hide_panel();
    else
        show_panel();
}

int a2600_controllers_window_visible(void)
{
    return g_panel && IsWindowVisible(g_panel);
}

void a2600_controllers_window_refresh(void)
{
    if (!g_panel)
        return;
    if (IsWindowVisible(g_panel)) {
        refresh_pads();
        InvalidateRect(g_panel, NULL, FALSE);
    }
}
