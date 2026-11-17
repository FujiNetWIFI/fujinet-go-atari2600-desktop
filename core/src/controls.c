/*
 * controls -- the layout of the on-screen controllers, for every toolkit.
 *
 * One table (a2600session_controls_layout) says what the Controllers window
 * draws and where, in layout units (1.0 = a keypad key's width) from the
 * top-left; the toolkits draw the art from it and ask a2600session_controls_hit
 * what a click landed on, so the shapes cannot drift apart between GTK, Qt,
 * AppKit and Win32. It has a controller view per port, following what is
 * plugged in (joystick, paddles, driving controller, keypad), over a console
 * strip. Every view has the same extent, so the window never changes size when
 * a game swaps a port's controller.
 *
 * The CX40 joystick and the CX30 paddles are the same hardware on the Atari
 * 800 (they plug into its controller ports), so their geometry here is
 * self-contained -- the joystick view (JOY_*) and the paddle view (PADDLE_*)
 * are written to be lifted whole into the 800 desktop's layout.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <math.h>
#include <pthread.h>
#include <string.h>

#include "session_internal.h"

/* ---- geometry (layout units) ------------------------------------------------------ */

/* Every device is placed from DESIGN-atari.md's own boxes, in the units that
 * document uses (a CX40's case is 8.0 x 8.0, a CX30 paddle 7.0 x 6.0, the CX50
 * 5.6 x 8.0, the console panel 24.0 x 5.2 over a 1.6 wood strip), so a frontend
 * scales one factor for the whole window. The coordinates below are therefore
 * the design's, device-local, plus the device's offset in its port's view.
 *
 * Each port's view is PORT_W x PORT_H: wide enough for a pair of paddles, tall
 * enough for the keypad. Every device is centred in it, so swapping the
 * controller a game plugs in never moves the console or resizes the window. */
#define CAPTION_H 0.9f
#define PORT_W   14.8f
#define PORT_H    8.0f
#define PORT_GAP  1.0f
#define CONSOLE_W 24.0f
#define PANEL_H    5.2f
#define WOOD_H     1.6f
#define CONSOLE_H (PANEL_H + WOOD_H)
#define CONSOLE_GAP 0.6f
#define LAYOUT_W (2.0f * PORT_W + PORT_GAP)
#define LAYOUT_H (CAPTION_H + PORT_H + CONSOLE_GAP + CONSOLE_H + 0.3f)

#define MAX_CONTROLS 56

typedef struct {
    a2600_control ctl[MAX_CONTROLS];
    int n;
} layout;

static layout g_layout[A2600_CTRL_COUNT][A2600_CTRL_COUNT];
static int g_built[A2600_CTRL_COUNT][A2600_CTRL_COUNT];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static void add(layout *lay, int kind, const char *label, const char *legend,
                int port, int target, int paddle,
                float x, float y, float w, float h)
{
    a2600_control *c;
    if (lay->n >= MAX_CONTROLS) return;
    c = &lay->ctl[lay->n++];
    c->kind = kind;
    c->label = label;
    c->legend = legend;
    c->port = port;
    c->target = target;
    c->paddle = paddle;
    c->x = x; c->y = y; c->w = w; c->h = h;
}

/* A controller view's types: AUTO (not yet detected) is drawn as a joystick, the
 * most common thing in a port. */
static int view_type(int type)
{
    return type >= A2600_CTRL_JOYSTICK && type < A2600_CTRL_COUNT ? type : A2600_CTRL_JOYSTICK;
}

/* The CX40 joystick (DESIGN-atari.md section 2), seen from above in an 8.0 x
 * 8.0 case: the round stick region is the boot's ring (centre (4.5, 4.5), r 2.1:
 * press and drag, a quarter of the reach is nothing, then eight 45-degree
 * sectors) and the one red fire button is a round button in the case's top-left
 * corner (centre (1.7, 1.7), r 0.95). The same case, ring and button are the
 * Atari 800's joystick: lift this function whole. (ox, oy) is the case's
 * top-left. */
static void add_joystick(layout *lay, int port, float ox, float oy)
{
    add(lay, A2600_CTL_BODY, "Joystick", NULL, port, -1, 0, ox, oy, 8.0f, 8.0f);
    add(lay, A2600_CTL_STICK, "STICK", NULL, port, -1, 0, ox + 2.4f, oy + 2.4f, 4.2f, 4.2f);
    add(lay, A2600_CTL_ROUND_BUTTON, "FIRE", NULL, port,
        A2600_TARGET_PORT(port, A2600_ACT_JOY_FIRE), 0, ox + 0.75f, oy + 0.75f, 1.9f, 1.9f);
}

/* One CX30 paddle (section 3) in a 7.0 x 6.0 box: the ribbed knob (centre
 * (3.2, 2.9), r 2.2: drag to turn it, the wheel nudges it), the red button on
 * the left edge, and the two small turn arrows under the knob that hold the
 * DEC / INC targets. `dec` is the paddle's DEC action: INC follows it, then FIRE.
 * A pair is two of these side by side with a 0.8 gap. The CX20 driving
 * controller (section 4) has the same body: add_driving_wheel reuses it. */
static void add_paddle_body(layout *lay, int port, int paddle, int kind, const char *name,
                            int dec, float ox, float oy)
{
    const int wheel = kind == A2600_CTL_WHEEL;
    add(lay, A2600_CTL_BODY, name, NULL, port, -1, paddle, ox, oy, 7.0f, 6.0f);
    add(lay, kind, wheel ? "WHEEL" : "KNOB", NULL, port, -1, paddle, ox + 1.0f, oy + 0.7f, 4.4f, 4.4f);
    add(lay, A2600_CTL_BUTTON, "FIRE", NULL, port, A2600_TARGET_PORT(port, dec + 2), paddle,
        ox, oy + 2.2f, 0.9f, 1.6f);
    add(lay, A2600_CTL_TURN, wheel ? "CCW" : "<", wheel ? "counter-clockwise" : "turn left", port,
        A2600_TARGET_PORT(port, dec), paddle, ox + 1.8f, oy + 4.9f, 0.8f, 0.9f);
    add(lay, A2600_CTL_TURN, wheel ? "CW" : ">", wheel ? "clockwise" : "turn right", port,
        A2600_TARGET_PORT(port, dec + 1), paddle, ox + 2.6f, oy + 4.9f, 0.8f, 0.9f);
}

static void add_paddles(layout *lay, int port, float ox, float oy)
{
    add_paddle_body(lay, port, 0, A2600_CTL_KNOB, "Paddle A", A2600_ACT_PADDLE_A_DEC, ox, oy);
    add_paddle_body(lay, port, 1, A2600_CTL_KNOB, "Paddle B", A2600_ACT_PADDLE_B_DEC, ox + 7.8f, oy);
}

/* The driving controller: the paddle's body, whose knob turns endlessly (a
 * Gray-code rotary), CCW and CW arrows where the paddle has its turn arrows, and
 * one fire button. */
static void add_driving(layout *lay, int port, float ox, float oy)
{
    add_paddle_body(lay, port, -1, A2600_CTL_WHEEL, "Driving", A2600_ACT_DRIVE_CCW, ox, oy);
}

/* The CX50 keypad controller (section 5) in a 5.6 x 8.0 box (the Kid's
 * Controller and the Video Touch Pad share its matrix): twelve round keys of r
 * 0.5, 1 2 3 / 4 5 6 / 7 8 9 / * 0 #, the first centred at (1.45, 3.1), pitch
 * 1.35 across and 1.2 down. */
static void add_keypad(layout *lay, int port, float ox, float oy)
{
    static const char *const k_labels[12] = {
        "1", "2", "3", "4", "5", "6", "7", "8", "9", "*", "0", "#",
    };
    int i;
    add(lay, A2600_CTL_BODY, "Keypad", NULL, port, -1, 0, ox, oy, 5.6f, 8.0f);
    for (i = 0; i < 12; i++)
        add(lay, A2600_CTL_ROUND_BUTTON, k_labels[i], NULL, port,
            A2600_TARGET_PORT(port, A2600_ACT_KEY_1 + i), 0,
            ox + 0.95f + (float)(i % 3) * 1.35f, oy + 2.6f + (float)(i / 3) * 1.2f, 1.0f, 1.0f);
}

/* The six-switch console (section 6): a 24.0 x 5.2 panel over a 1.6 wood strip.
 * The slide switches sit at x = 2.0 (power), 4.4 (tv type), 6.6 (left
 * difficulty), 17.2 (right difficulty), 19.9 (game select) and 22.4 (game
 * reset); each pressable part is the switch's slot with room round it. Power is
 * drawn fixed ON, but pressing it is a power-cycle (the session's Reboot to
 * CONFIG, the same as on a FujiNet cartridge). The STOP button on the wood strip
 * is the session's Pause (stop in the debugger), which the real console has no
 * switch for. */
static void add_console(layout *lay, float ox, float oy)
{
    static const struct {
        int kind, target;
        float cx;
        const char *label, *legend;
    } k_switch[] = {
        { A2600_CTL_BUTTON, A2600_TARGET_SYSACT(A2600_SYSACT_REBOOT_CONFIG), 2.0f, "POWER", "power" },
        { A2600_CTL_SWITCH, A2600_TARGET_SWITCH(A2600_SW_COLOR_BW), 4.4f, "COLOR / B&W", "tv type" },
        { A2600_CTL_SWITCH, A2600_TARGET_SWITCH(A2600_SW_LEFT_DIFF), 6.6f, "A / B", "left difficulty" },
        { A2600_CTL_SWITCH, A2600_TARGET_SWITCH(A2600_SW_RIGHT_DIFF), 17.2f, "A / B", "right difficulty" },
        { A2600_CTL_BUTTON, A2600_TARGET_SWITCH(A2600_SW_SELECT), 19.9f, "SELECT", "game select" },
        { A2600_CTL_BUTTON, A2600_TARGET_SWITCH(A2600_SW_RESET), 22.4f, "RESET", "game reset" },
    };
    size_t i;

    add(lay, A2600_CTL_BODY, "Console", NULL, -1, -1, 0, ox, oy, CONSOLE_W, PANEL_H);
    add(lay, A2600_CTL_BODY, "Cartridge slot", NULL, -1, -1, 0, ox + 8.6f, oy + 1.6f, 6.8f, 2.4f);
    add(lay, A2600_CTL_BODY, "Wood", NULL, -1, -1, 0, ox, oy + PANEL_H, CONSOLE_W, WOOD_H);
    for (i = 0; i < sizeof k_switch / sizeof k_switch[0]; i++)
        add(lay, k_switch[i].kind, k_switch[i].label, k_switch[i].legend, -1,
            k_switch[i].target, 0, ox + k_switch[i].cx - 0.7f, oy + 1.9f, 1.4f, 2.2f);
    add(lay, A2600_CTL_BUTTON, "STOP", "stop in debugger", -1,
        A2600_TARGET_SYSACT(A2600_SYSACT_PAUSE), 0, ox + 0.5f, oy + PANEL_H + 0.25f, 3.2f, 1.1f);
}

static void build(layout *lay, int left, int right)
{
    static const char *const k_caption[2] = { "Left controller", "Right controller" };
    int port;
    memset(lay, 0, sizeof *lay);
    for (port = 0; port < 2; port++) {
        const float ox = (float)port * (PORT_W + PORT_GAP);
        const float oy = CAPTION_H;
        add(lay, A2600_CTL_CAPTION, k_caption[port], NULL, port, -1, 0, ox, 0.0f, PORT_W, CAPTION_H);
        /* each device centred in its port's PORT_W x PORT_H view */
        switch (port ? right : left) {
        case A2600_CTRL_PADDLES: add_paddles(lay, port, ox, oy + 1.0f); break;
        case A2600_CTRL_DRIVING: add_driving(lay, port, ox + 3.9f, oy + 1.0f); break;
        case A2600_CTRL_KEYPAD:  add_keypad(lay, port, ox + 4.6f, oy); break;
        default:                 add_joystick(lay, port, ox + 3.4f, oy); break;
        }
    }
    add_console(lay, (LAYOUT_W - CONSOLE_W) * 0.5f, CAPTION_H + PORT_H + CONSOLE_GAP);
}

const a2600_control *a2600session_controls_layout(int left_type, int right_type, int *n,
                                                  float *width, float *height)
{
    const int lt = view_type(left_type), rt = view_type(right_type);
    layout *lay;

    pthread_mutex_lock(&g_lock);
    if (!g_built[lt][rt]) {
        build(&g_layout[lt][rt], lt, rt);
        g_built[lt][rt] = 1;
    }
    lay = &g_layout[lt][rt];
    pthread_mutex_unlock(&g_lock);
    if (n) *n = lay->n;
    if (width) *width = LAYOUT_W;
    if (height) *height = LAYOUT_H;
    return lay->ctl;
}

/* The shapes round parts have: an ellipse inside the box. Everything else is its
 * rectangle. */
static int is_round(int kind)
{
    return kind == A2600_CTL_STICK || kind == A2600_CTL_KNOB || kind == A2600_CTL_WHEEL
        || kind == A2600_CTL_ROUND_BUTTON;
}

int a2600session_controls_hit(int left_type, int right_type, float x, float y)
{
    int n, i;
    const a2600_control *c = a2600session_controls_layout(left_type, right_type, &n, NULL, NULL);

    /* the topmost: later entries are drawn over earlier ones */
    for (i = n - 1; i >= 0; i--) {
        if (c[i].kind == A2600_CTL_BODY || c[i].kind == A2600_CTL_CAPTION) continue;
        if (x < c[i].x || x >= c[i].x + c[i].w || y < c[i].y || y >= c[i].y + c[i].h)
            continue;
        if (is_round(c[i].kind)) {
            const float rx = c[i].w * 0.5f, ry = c[i].h * 0.5f;
            const float dx = (x - (c[i].x + rx)) / rx, dy = (y - (c[i].y + ry)) / ry;
            if (dx * dx + dy * dy > 1.0f) continue;
        }
        return i;
    }
    return -1;
}

/* ---- dragging ---------------------------------------------------------------------- */

unsigned a2600session_stick_from_vector(float dx, float dy, float radius)
{
    const float ax = fabsf(dx), ay = fabsf(dy);
    unsigned mask = 0;
    /* tan(22.5 degrees): the edge of a 45-degree sector */
    const float edge = 0.41421356f;

    if (radius <= 0.f || ax * ax + ay * ay < (0.25f * radius) * (0.25f * radius))
        return 0;
    if (ay <= ax * edge)       mask = dx < 0.f ? 1u << A2600_ACT_JOY_LEFT : 1u << A2600_ACT_JOY_RIGHT;
    else if (ax <= ay * edge)  mask = dy < 0.f ? 1u << A2600_ACT_JOY_UP : 1u << A2600_ACT_JOY_DOWN;
    else {
        mask = dx < 0.f ? 1u << A2600_ACT_JOY_LEFT : 1u << A2600_ACT_JOY_RIGHT;
        mask |= dy < 0.f ? 1u << A2600_ACT_JOY_UP : 1u << A2600_ACT_JOY_DOWN;
    }
    return mask;
}

float a2600session_vector_angle(float dx, float dy)
{
    return atan2f(dx, -dy) * (180.0f / 3.14159265358979f);
}

int a2600session_knob_from_vector(float dx, float dy)
{
    float deg, value;
    if (dx == 0.f && dy == 0.f) return 0;
    deg = a2600session_vector_angle(dx, dy);
    value = deg / A2600_KNOB_TRAVEL_DEG;
    if (value < -1.f) value = -1.f;
    if (value > 1.f) value = 1.f;
    return (int)lroundf(value * 32767.0f);
}

int a2600session_view_type(a2600session *s, int port)
{
    int type = a2600session_port_type(s, port);
    if (type == A2600_CTRL_AUTO)
        type = a2600session_detected_port_type(s, port);
    return view_type(type);
}
