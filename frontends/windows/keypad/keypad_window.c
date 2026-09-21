/*
 * The Win32 keypad panel: both keyboard controllers side by side, each a
 * 3x4 keypad over its port's fire buttons, then the console switches and
 * the Map row.
 *
 * Buttons are driven by WM_LBUTTONDOWN/WM_LBUTTONUP on the panel window
 * rather than by BN_CLICKED: a keypad key on this machine is HELD, and a
 * game polls it, so a value present only for the instant of a click falls
 * between frames. The mouse is captured on press and released on
 * button-up wherever that happens, so dragging off a button cannot strand
 * the machine with a key held forever.
 *
 * The window is a tool window of fixed size: the keys are a block of
 * fixed-size buttons, and there is nothing to gain by stretching them.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "keypad_window.h"

/* GET_X_LPARAM / GET_Y_LPARAM live here, not in windows.h. */
#include <windowsx.h>

#include <stdio.h>
#include <string.h>

#include "../key_forward.h"

#define PAD_CLASS "FujiNetGoAtari2600Keypad"

#define KEY_W   56
#define KEY_H   40
#define GAP      6
#define WIDE_W 118
#define PAD_W  (3 * WIDE_W + 2 * GAP)      /* a controller box's inner width */
#define GRID_W (3 * KEY_W + 2 * GAP)       /* the keypad block's width */
#define MARGIN  12
#define HEAD_H  22                          /* "Left Port" */
#define TYPE_H  20                          /* "Joystick attached" */

#define IDT_CAPTURE 1
#define IDT_TYPES   2

typedef struct {
    RECT rc;
    int target;
    char face[20];
} pad_button;

static HWND g_panel;
static a2600session *g_session;
static pad_button g_btn[A2600_TARGET_COUNT];
static int g_nbtn;
static int g_held = -1;          /* button index under the captured mouse */
static int g_map_state = -2;     /* -2 idle, -1 armed, >=0 awaiting a key/button */
static RECT g_map_rc, g_defaults_rc, g_hint_rc;
static RECT g_head_rc[2], g_type_rc[2];
static int g_detected[2];
static char g_hint[160];
static HBRUSH g_accent_brush;
static HFONT g_bold;

static void add_button(const char *face, int target, int x, int y, int w)
{
    pad_button *b;
    if (g_nbtn >= A2600_TARGET_COUNT) return;
    b = &g_btn[g_nbtn++];
    SetRect(&b->rc, x, y, x + w, y + KEY_H);
    b->target = target;
    snprintf(b->face, sizeof b->face, "%s", face);
}

static const char *const kFace[12] = { "1", "2", "3", "4", "5", "6", "7", "8", "9", "*", "0", "#" };

/* One controller: heading, attached-type line, the centred 3x4 block, and
 * the fire row. Returns the y below it. */
static int build_controller(int port, int x0, int y0)
{
    const int gx = x0 + (PAD_W - GRID_W) / 2;   /* centre the block over the fire row */
    int y = y0, i;

    SetRect(&g_head_rc[port], x0, y, x0 + PAD_W, y + HEAD_H);
    y += HEAD_H;
    SetRect(&g_type_rc[port], x0, y, x0 + PAD_W, y + TYPE_H);
    y += TYPE_H + GAP;

    for (i = 0; i < 12; i++)
        add_button(kFace[i], A2600_TARGET_PORT(port, A2600_ACT_KEY_1 + i),
                   gx + (i % 3) * (KEY_W + GAP), y + (i / 3) * (KEY_H + GAP), KEY_W);
    y += 4 * (KEY_H + GAP) + GAP;

    add_button("Fire", A2600_TARGET_PORT(port, A2600_ACT_JOY_FIRE), x0, y, WIDE_W);
    add_button("Paddle A", A2600_TARGET_PORT(port, A2600_ACT_PADDLE_A_FIRE), x0 + WIDE_W + GAP, y, WIDE_W);
    add_button("Paddle B", A2600_TARGET_PORT(port, A2600_ACT_PADDLE_B_FIRE), x0 + 2 * (WIDE_W + GAP), y, WIDE_W);
    return y + KEY_H;
}

static int g_total_w, g_total_h;

static void layout(void)
{
    const int x1 = MARGIN + PAD_W + 2 * MARGIN;
    int y, cx;
    static const struct { const char *face; int target; } console[6] = {
        { "Select",           A2600_TARGET_SWITCH(A2600_SW_SELECT) },
        { "Reset",            A2600_TARGET_SWITCH(A2600_SW_RESET) },
        { "Color / B&W",      A2600_TARGET_SWITCH(A2600_SW_COLOR_BW) },
        { "Left Diff",        A2600_TARGET_SWITCH(A2600_SW_LEFT_DIFF) },
        { "Right Diff",       A2600_TARGET_SWITCH(A2600_SW_RIGHT_DIFF) },
        { "Reboot to CONFIG", A2600_TARGET_SYSACT(A2600_SYSACT_REBOOT_CONFIG) },
    };
    int i;

    g_nbtn = 0;
    y = build_controller(0, MARGIN, MARGIN);
    build_controller(1, x1, MARGIN);
    g_total_w = x1 + PAD_W + MARGIN;

    /* Console row, centred. */
    y += MARGIN + 4;
    cx = (g_total_w - (6 * WIDE_W + 5 * GAP)) / 2;
    for (i = 0; i < 6; i++)
        add_button(console[i].face, console[i].target, cx + i * (WIDE_W + GAP), y, WIDE_W);
    y += KEY_H + MARGIN;

    SetRect(&g_map_rc, MARGIN, y, MARGIN + 70, y + 30);
    SetRect(&g_defaults_rc, MARGIN + 76, y, MARGIN + 76 + 84, y + 30);
    SetRect(&g_hint_rc, MARGIN + 76 + 84 + 10, y, g_total_w - MARGIN, y + 30);
    g_total_h = y + 30 + MARGIN;
}

static void press_target(int target, int down)
{
    if (target >= A2600_TARGET_SYSACT(0)) {
        /* System actions fire on release, like a real button: pressing and
         * dragging off must not reboot the machine. */
        if (!down) a2600session_sysaction(g_session, target - A2600_TARGET_SYSACT(0));
        return;
    }
    a2600session_press(g_session, target, down);
}

static int hit(int x, int y)
{
    POINT p = { x, y };
    int i;
    for (i = 0; i < g_nbtn; i++)
        if (PtInRect(&g_btn[i].rc, p)) return i;
    return -1;
}

static void set_map_state(int state)
{
    g_map_state = state;
    if (state == -2) {
        KillTimer(g_panel, IDT_CAPTURE);
        a2600session_gamepad_capture_cancel(g_session);
        g_hint[0] = '\0';
    } else if (state == -1) {
        KillTimer(g_panel, IDT_CAPTURE);
        a2600session_gamepad_capture_cancel(g_session);
        snprintf(g_hint, sizeof g_hint, "Click a control to remap");
    } else {
        snprintf(g_hint, sizeof g_hint, "Press a key or gamepad button for %s", a2600_target_name(state));
        a2600session_gamepad_capture_begin(g_session);
        SetTimer(g_panel, IDT_CAPTURE, 50, NULL);
    }
    InvalidateRect(g_panel, NULL, TRUE);
}

static void button_label(const pad_button *b, char *out, int outsz)
{
    if (g_map_state != -2) {
        const a2600_binding bind = a2600session_binding_get(g_session, b->target);
        char key[32];
        a2600session_keysym_name(bind.keysym, key, sizeof key);
        if (bind.button != A2600_PAD_BTN_NONE)
            snprintf(out, outsz, "%s\n%s", key[0] ? key : "-", a2600_pad_button_name(bind.button));
        else
            snprintf(out, outsz, "%s", key[0] ? key : "-");
    } else {
        snprintf(out, outsz, "%s", b->face);
    }
}

static void draw_button(HDC dc, const RECT *rc, const char *label, int pushed, int accent)
{
    RECT r = *rc, text;
    int h;

    if (accent) {
        FillRect(dc, &r, g_accent_brush);
        FrameRect(dc, &r, (HBRUSH)GetStockObject(GRAY_BRUSH));
        SetTextColor(dc, RGB(0, 0, 0));
    } else {
        DrawFrameControl(dc, &r, DFC_BUTTON, DFCS_BUTTONPUSH | (pushed ? DFCS_PUSHED : 0));
        SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
    }
    /* Vertically centred, word-broken on the newline a Map-mode label
     * carries between its key and its pad button. */
    text = r;
    InflateRect(&text, -3, 0);
    h = DrawTextA(dc, label, -1, &text, DT_CENTER | DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);
    text.left = r.left + 3;
    text.right = r.right - 3;
    text.top = r.top + ((r.bottom - r.top) - h) / 2 + (pushed ? 1 : 0);
    text.bottom = r.bottom;
    DrawTextA(dc, label, -1, &text, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
}

static void paint_panel(HDC dc)
{
    HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    HGDIOBJ old = SelectObject(dc, font);
    RECT client;
    int i, port;

    GetClientRect(g_panel, &client);
    FillRect(dc, &client, (HBRUSH)(COLOR_BTNFACE + 1));
    SetBkMode(dc, TRANSPARENT);

    for (port = 0; port < 2; port++) {
        char line[64];
        SelectObject(dc, g_bold);
        SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
        DrawTextA(dc, port ? "Right Port" : "Left Port", -1, &g_head_rc[port], DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, g_detected[port] == A2600_CTRL_KEYPAD ? g_bold : font);
        SetTextColor(dc, g_detected[port] == A2600_CTRL_KEYPAD ? RGB(0xC0, 0x60, 0x00) : GetSysColor(COLOR_GRAYTEXT));
        snprintf(line, sizeof line, "%s attached", a2600_ctrl_type_name(g_detected[port]));
        DrawTextA(dc, line, -1, &g_type_rc[port], DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    SelectObject(dc, font);

    for (i = 0; i < g_nbtn; i++) {
        char label[64];
        int held = (i == g_held) && g_map_state == -2;
        int accent = held || (g_map_state >= 0 && g_btn[i].target == g_map_state);
        button_label(&g_btn[i], label, sizeof label);
        draw_button(dc, &g_btn[i].rc, label, held, accent);
    }

    draw_button(dc, &g_map_rc, g_map_state == -2 ? "Map" : "Cancel", 0, g_map_state != -2);
    draw_button(dc, &g_defaults_rc, "Defaults", 0, 0);

    if (g_hint[0]) {
        SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
        DrawTextA(dc, g_hint, -1, &g_hint_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    SelectObject(dc, old);
}

static void refresh_types(void)
{
    int port, changed = 0;
    for (port = 0; port < 2; port++) {
        int det = a2600session_detected_port_type(g_session, port);
        if (det != g_detected[port]) { g_detected[port] = det; changed = 1; }
    }
    if (changed) InvalidateRect(g_panel, NULL, TRUE);
}

static LRESULT CALLBACK pad_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        paint_panel(dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;

    case WM_LBUTTONDOWN: {
        const int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        POINT p = { x, y };
        int i = hit(x, y);

        if (PtInRect(&g_map_rc, p)) { set_map_state(g_map_state == -2 ? -1 : -2); return 0; }
        if (PtInRect(&g_defaults_rc, p)) {
            a2600session_bindings_reset(g_session);
            InvalidateRect(hwnd, NULL, TRUE);
            return 0;
        }
        if (i < 0) return 0;

        if (g_map_state == -1) { set_map_state(g_btn[i].target); return 0; }
        if (g_map_state >= 0) return 0;

        g_held = i;
        SetCapture(hwnd);
        press_target(g_btn[i].target, 1);
        InvalidateRect(hwnd, &g_btn[i].rc, FALSE);
        return 0;
    }
    case WM_LBUTTONUP:
        /* Release wherever the mouse ended up: capture means this arrives
         * even if the pointer left the button. */
        if (g_held >= 0) {
            RECT r = g_btn[g_held].rc;
            int t = g_btn[g_held].target;
            g_held = -1;
            ReleaseCapture();
            press_target(t, 0);
            InvalidateRect(hwnd, &r, FALSE);
        }
        return 0;

    case WM_TIMER:
        if (wp == IDT_CAPTURE && g_map_state >= 0) {
            int button;
            if (a2600session_gamepad_capture_poll(g_session, &button)) {
                char stolen[128];
                a2600session_binding_set_button(g_session, g_map_state, button, stolen, sizeof stolen);
                set_map_state(-1);
                if (stolen[0])
                    snprintf(g_hint, sizeof g_hint, "Bound %s (was %s)", a2600_pad_button_name(button), stolen);
                InvalidateRect(hwnd, NULL, TRUE);
            }
        } else if (wp == IDT_TYPES) {
            refresh_types();
        }
        return 0;

    case WM_KEYDOWN: case WM_SYSKEYDOWN: {
        const uint32_t ks = a2600_keysym_from_msg(wp, lp);
        int sa;
        if (lp & (1 << 30)) return 0;   /* auto-repeat */
        if (g_map_state >= 0) {
            if (ks) {
                char stolen[128], name[32];
                a2600session_binding_set_key(g_session, g_map_state, ks, stolen, sizeof stolen);
                a2600session_keysym_name(ks, name, sizeof name);
                set_map_state(-1);   /* stay armed: remapping several in a row is normal */
                if (stolen[0]) snprintf(g_hint, sizeof g_hint, "Bound %s (was %s)", name, stolen);
                InvalidateRect(hwnd, NULL, TRUE);
            }
            return 0;
        }
        if (g_map_state == -1) return 0;
        if (wp == VK_F9) { ShowWindow(hwnd, SW_HIDE); return 0; }
        if (!ks) break;
        sa = a2600session_key_sysaction(g_session, ks);
        if (sa >= 0) { a2600session_sysaction(g_session, sa); return 0; }
        if (a2600session_key(g_session, ks, 1)) return 0;
        break;
    }
    case WM_KEYUP: case WM_SYSKEYUP: {
        const uint32_t ks = a2600_keysym_from_msg(wp, lp);
        if (g_map_state != -2) return 0;
        if (ks && a2600session_key(g_session, ks, 0)) return 0;
        break;
    }
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) a2600session_release_all(g_session);
        return 0;
    case WM_SHOWWINDOW:
        if (wp) { refresh_types(); SetTimer(hwnd, IDT_TYPES, 1000, NULL); }
        else KillTimer(hwnd, IDT_TYPES);
        break;
    case WM_CLOSE:
        /* Hide, do not destroy: the window's position survives closing it. */
        set_map_state(-2);
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    default: break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

void a2600_keypad_window_toggle(HWND parent, a2600session *session)
{
    g_session = session;

    if (!g_panel) {
        WNDCLASSEXA wc;
        RECT want;
        LOGFONTA lf;

        layout();
        g_accent_brush = CreateSolidBrush(RGB((A2600SESSION_ACCENT_RGB >> 16) & 0xff,
                                              (A2600SESSION_ACCENT_RGB >> 8) & 0xff,
                                              A2600SESSION_ACCENT_RGB & 0xff));
        GetObjectA(GetStockObject(DEFAULT_GUI_FONT), sizeof lf, &lf);
        lf.lfWeight = FW_BOLD;
        g_bold = CreateFontIndirectA(&lf);

        memset(&wc, 0, sizeof wc);
        wc.cbSize = sizeof wc;
        wc.lpfnWndProc = pad_proc;
        wc.hInstance = GetModuleHandle(NULL);
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = PAD_CLASS;
        RegisterClassExA(&wc);

        SetRect(&want, 0, 0, g_total_w, g_total_h);
        /* A tool window with a caption and no thick frame or maximize box:
         * it is exactly the size of its controls. WS_EX_TOOLWINDOW also
         * keeps it off the taskbar. */
        AdjustWindowRectEx(&want, WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_TOOLWINDOW);
        g_panel = CreateWindowExA(WS_EX_TOOLWINDOW, PAD_CLASS, "Keypads",
                                  WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT,
                                  want.right - want.left, want.bottom - want.top,
                                  parent, NULL, wc.hInstance, NULL);
        if (!g_panel) return;
        g_detected[0] = g_detected[1] = -1;
    }

    if (IsWindowVisible(g_panel)) {
        set_map_state(-2);
        ShowWindow(g_panel, SW_HIDE);
    } else {
        ShowWindow(g_panel, SW_SHOW);
        SetForegroundWindow(g_panel);
    }
}

int a2600_keypad_pretranslate(MSG *msg)
{
    /* The panel has no child controls, so its own window proc sees every
     * key; nothing to steal here. Kept for symmetry with the debugger. */
    (void)msg;
    return 0;
}
