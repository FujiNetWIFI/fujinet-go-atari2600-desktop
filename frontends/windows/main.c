/*
 * FujiNet Go Atari 2600 -- the Windows (Win32 + GDI) frontend.
 *
 * No toolkit: a plain window, a menu bar, and StretchDIBits. That is enough
 * for a 160-pixel-wide framebuffer, and it keeps the artifact a folder you
 * copy rather than a runtime hunt.
 *
 * Two Windows-specific things are load bearing:
 *
 *   DwmFlush() on a present thread is this platform's frame clock. There is
 *   no GdkFrameClock here, and a plain timer would beat against the panel.
 *
 *   WM_ACTIVATE releases every held key. Alt-tabbing away mid-jump and coming
 *   back to a character walking into a wall is the classic symptom of not
 *   doing this, and Windows is where it happens most, because the WM eats the
 *   key-up.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "a2600session.h"
#include "debugger/dbg_window.h"
#include "keypad/keypad_window.h"
#include "key_forward.h"
#include "resource.h"

#define WIN_CLASS "FujiNetGoAtari2600"
#define APP_TITLE "FujiNet Go Atari 2600"

static a2600session *g_session;
static HWND g_hwnd;
static uint32_t *g_fb;
static int g_fb_height;
static uint64_t g_serial;
static BITMAPINFO g_bmi;
static CRITICAL_SECTION g_fb_lock;
static volatile LONG g_running = 1;
static HANDLE g_present_thread;
static int g_tv_aspect = 1, g_smooth = 0, g_fullscreen = 0;
static WINDOWPLACEMENT g_placement;
static int g_sysact_down[A2600_SYSACT_COUNT];

/* ---- the present thread: this platform's frame clock ---------------------- */

static DWORD WINAPI present_thread(LPVOID arg)
{
    (void)arg;
    while (InterlockedCompareExchange(&g_running, 1, 1)) {
        LARGE_INTEGER t, f;
        int h = 0;
        /* Blocks until the compositor's next vblank; falls through at once
         * if the DWM is off, and the session's wall-clock pacing takes over
         * -- which is the whole reason notify_vsync is advisory. */
        DwmFlush();
        QueryPerformanceCounter(&t);
        QueryPerformanceFrequency(&f);
        a2600session_notify_vsync(g_session,
                                  (int64_t)(t.QuadPart * 1000000000LL / f.QuadPart));
        EnterCriticalSection(&g_fb_lock);
        if (a2600session_copy_frame(g_session, g_fb, &h, &g_serial)) {
            g_fb_height = h;
            LeaveCriticalSection(&g_fb_lock);
            InvalidateRect(g_hwnd, NULL, FALSE);
        } else {
            LeaveCriticalSection(&g_fb_lock);
        }
    }
    return 0;
}

/* ---- painting ------------------------------------------------------------- */

static void paint(HDC dc)
{
    RECT rc;
    double want, w, h, sw, sh;
    int fbh;

    GetClientRect(g_hwnd, &rc);
    w = rc.right - rc.left;
    h = rc.bottom - rc.top;
    FillRect(dc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
    if (w <= 0 || h <= 0) return;

    EnterCriticalSection(&g_fb_lock);
    fbh = g_fb_height;
    if (fbh <= 0) { LeaveCriticalSection(&g_fb_lock); return; }

    /* A television showed whatever lines the game drew inside its 4:3
     * screen, with each TIA pixel about twice as wide as it is tall. */
    want = g_tv_aspect ? (4.0 / 3.0) : (double)A2600SESSION_FB_WIDTH / (double)fbh;
    if (w / h > want) { sh = h; sw = sh * want; }
    else              { sw = w; sh = sw / want; }

    /* Stella's pixels are 0x00RRGGBB: exactly a 32-bit BI_RGB DIB. */
    g_bmi.bmiHeader.biHeight = -fbh;   /* top-down */
    SetStretchBltMode(dc, g_smooth ? HALFTONE : COLORONCOLOR);
    StretchDIBits(dc, (int)((w - sw) / 2), (int)((h - sh) / 2), (int)sw, (int)sh,
                  0, 0, A2600SESSION_FB_WIDTH, fbh, g_fb, &g_bmi, DIB_RGB_COLORS, SRCCOPY);
    LeaveCriticalSection(&g_fb_lock);
}

/* ---- helpers -------------------------------------------------------------- */

static void restart_session(void)
{
    a2600session_start_opts o;
    a2600session_settings_flush(g_session);
    a2600session_default_opts(g_session, &o);
    a2600session_stop(g_session);
    if (a2600session_start(g_session, &o) != 0)
        MessageBoxA(g_hwnd, a2600session_last_error(g_session), "Could not start",
                    MB_ICONWARNING | MB_OK);
}

static void run_sysaction(int sa)
{
    switch (sa) {
    case A2600_SYSACT_REBOOT_CONFIG: a2600session_sysaction(g_session, sa); break;
    case A2600_SYSACT_PAUSE:
        a2600_debugger_show(g_hwnd, g_session);
        a2600session_sysaction(g_session, sa);
        break;
    default: break;
    }
}

static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '\\');
    const char *t = strrchr(p, '/');
    if (t && (!s || t > s)) s = t;
    return s ? s + 1 : p;
}

static void update_title(void)
{
    char title[256], st[128];
    if (!a2600session_is_running(g_session))
        snprintf(title, sizeof title, APP_TITLE " \xe2\x80\x94 stopped");
    else if (a2600session_cart_link_up(g_session) < 0)
        snprintf(title, sizeof title, APP_TITLE " \xe2\x80\x94 %s",
                 base_name(a2600session_cart_path(g_session)));
    else if (a2600session_cart_booted_game(g_session))
        snprintf(title, sizeof title, APP_TITLE " \xe2\x80\x94 FujiNet: booted a game");
    else if (a2600session_cart_link_up(g_session) == 1)
        snprintf(title, sizeof title, APP_TITLE " \xe2\x80\x94 FujiNet connected");
    else {
        a2600session_cart_status(g_session, st, sizeof st);
        snprintf(title, sizeof title, APP_TITLE " \xe2\x80\x94 FujiNet: %s", st);
    }
    {
        /* UTF-8 -> UTF-16 for the em dash. */
        wchar_t wtitle[256];
        MultiByteToWideChar(CP_UTF8, 0, title, -1, wtitle, 256);
        SetWindowTextW(g_hwnd, wtitle);
    }
}

/* ---- menu ----------------------------------------------------------------- */

static void build_menu(HWND hwnd)
{
    HMENU bar = CreateMenu();
    HMENU machine = CreatePopupMenu();
    HMENU view = CreatePopupMenu();
    HMENU fuji = CreatePopupMenu();
    HMENU help = CreatePopupMenu();

    AppendMenuA(machine, MF_STRING, IDM_OPEN, "&Open Cartridge...\tCtrl+O");
    AppendMenuA(machine, MF_STRING, IDM_EJECT, "&Eject Cartridge");
    AppendMenuA(machine, MF_STRING, IDM_IMPORT_SD, "&Import Cartridge to SD...");
    AppendMenuA(machine, MF_STRING, IDM_REBOOT_CONFIG, "Reboot to &CONFIG\tCtrl+R");
    AppendMenuA(machine, MF_SEPARATOR, 0, NULL);
    AppendMenuA(machine, MF_STRING, IDM_SELECT, "&Select\tF1");
    AppendMenuA(machine, MF_STRING, IDM_RESET, "&Reset\tF2");
    AppendMenuA(machine, MF_STRING | MF_CHECKED, IDM_COLOR, "Colo&r (B&&W when off)\tF3");
    AppendMenuA(machine, MF_STRING, IDM_LEFT_DIFF, "Left Difficulty &A\tAlt+L");
    AppendMenuA(machine, MF_STRING, IDM_RIGHT_DIFF, "Right Difficulty A\tAlt+R");
    AppendMenuA(machine, MF_SEPARATOR, 0, NULL);
    AppendMenuA(machine, MF_STRING, IDM_SETTINGS, "&Settings...");
    AppendMenuA(machine, MF_SEPARATOR, 0, NULL);
    AppendMenuA(machine, MF_STRING, IDM_EXIT, "E&xit");

    AppendMenuA(view, MF_STRING, IDM_KEYPAD, "&Keypads\tF9");
    AppendMenuA(view, MF_STRING, IDM_DEBUGGER, "&Debugger\tF12");
    AppendMenuA(view, MF_SEPARATOR, 0, NULL);
    AppendMenuA(view, MF_STRING | MF_CHECKED, IDM_TV_ASPECT, "&TV Aspect (4:3)");
    AppendMenuA(view, MF_STRING, IDM_SMOOTH, "&Smooth Scaling");
    AppendMenuA(view, MF_STRING, IDM_FULLSCREEN, "&Fullscreen\tF11");

    AppendMenuA(fuji, MF_STRING, IDM_FUJINET_CONFIG, "&Configuration");
    AppendMenuA(fuji, MF_STRING, IDM_FUJINET_LOG, "Console &Log");

    AppendMenuA(help, MF_STRING, IDM_ABOUT, "&About " APP_TITLE);

    AppendMenuA(bar, MF_POPUP, (UINT_PTR)machine, "&Machine");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)view, "&View");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)fuji, "&FujiNet");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)help, "&Help");
    SetMenu(hwnd, bar);
}

static void open_cart(const char *path)
{
    if (a2600session_load_cart(g_session, path) != 0)
        MessageBoxA(g_hwnd, a2600session_last_error(g_session), "Could not open",
                    MB_ICONWARNING | MB_OK);
}

static void load_media(const char *path)
{
    char dest[1024];
    if (a2600session_media_is_cartridge(path)) { open_cart(path); return; }
    if (a2600session_import_media(g_session, path, dest, sizeof dest) != 0) {
        MessageBoxA(g_hwnd, a2600session_last_error(g_session), "Import failed",
                    MB_ICONWARNING | MB_OK);
        return;
    }
    MessageBoxA(g_hwnd, "Copied to FujiNet's SD folder. Mount it from the CONFIG client.",
                "Imported", MB_ICONINFORMATION | MB_OK);
}

static int pick_cart(const char *title, char *path, DWORD pathsz)
{
    OPENFILENAMEA ofn;
    memset(&ofn, 0, sizeof ofn);
    path[0] = '\0';
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFilter = "Atari 2600 cartridges\0*.a26;*.bin;*.rom;*.fuji\0All files\0*.*\0\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = pathsz;
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    return GetOpenFileNameA(&ofn) ? 1 : 0;
}

/* ---- settings window -------------------------------------------------------
 *
 * Same keys and defaults as the other frontends' Preferences, so a machine
 * configured in one comes up the same in another. Controller types and the
 * analog switches apply live; the TV format and the host options restart
 * the session when the window closes.
 */

static HWND g_settings_window;
static int g_settings_dirty;
static HWND g_pad_list, g_pad_port, g_port_note[2];
static unsigned g_pad_generation;

static void settings_apply_checkbox(HWND hwnd, int id, const char *key, int def, int live)
{
    int on = SendMessageA(GetDlgItem(hwnd, id), BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (a2600session_get_int(g_session, key, def) != on) {
        a2600session_set_int(g_session, key, on);
        if (live)
            a2600session_set_analog(g_session,
                a2600session_get_int(g_session, "analog_joystick", 1),
                a2600session_get_int(g_session, "analog_paddle", 1),
                a2600session_get_int(g_session, "analog_driving", 1));
        else
            g_settings_dirty = 1;
    }
}

static void update_port_notes(void)
{
    int port;
    for (port = 0; port < 2; port++) {
        char text[96];
        if (a2600session_port_type(g_session, port) == A2600_CTRL_AUTO)
            snprintf(text, sizeof text, "Auto: Stella attached %s",
                     a2600_ctrl_type_name(a2600session_detected_port_type(g_session, port)));
        else
            snprintf(text, sizeof text, "Forced for every cartridge");
        if (g_port_note[port]) SetWindowTextA(g_port_note[port], text);
    }
}

static void refresh_pad_list(void)
{
    int i, n, sel;
    if (!g_pad_list) return;
    sel = (int)SendMessageA(g_pad_list, LB_GETCURSEL, 0, 0);
    SendMessageA(g_pad_list, LB_RESETCONTENT, 0, 0);
    n = a2600session_gamepad_count(g_session);
    if (n == 0) {
        SendMessageA(g_pad_list, LB_ADDSTRING, 0, (LPARAM)"(no gamepads connected)");
    }
    for (i = 0; i < n; i++) {
        char name[128], line[200];
        int eff = a2600session_gamepad_effective_port(g_session, i);
        a2600session_gamepad_name(g_session, i, name, sizeof name);
        snprintf(line, sizeof line, "%s  [%s port]", name,
                 eff == 0 ? "left" : eff == 1 ? "right" : "no");
        SendMessageA(g_pad_list, LB_ADDSTRING, 0, (LPARAM)line);
    }
    if (sel >= 0 && sel < n) SendMessageA(g_pad_list, LB_SETCURSEL, (WPARAM)sel, 0);
}

static LRESULT CALLBACK settings_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_SET_TV:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                int sel = (int)SendMessageA(GetDlgItem(hwnd, IDC_SET_TV), CB_GETCURSEL, 0, 0);
                if (a2600session_get_int(g_session, "tv_format", 0) != sel) {
                    a2600session_set_int(g_session, "tv_format", sel);
                    g_settings_dirty = 1;
                }
            }
            return 0;
        case IDC_SET_PORT0:
        case IDC_SET_PORT1:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                int port = LOWORD(wp) == IDC_SET_PORT1;
                int sel = (int)SendMessageA(GetDlgItem(hwnd, LOWORD(wp)), CB_GETCURSEL, 0, 0);
                a2600session_set_port_type(g_session, port, sel);
                update_port_notes();
            }
            return 0;
        case IDC_SET_AN_JOY: settings_apply_checkbox(hwnd, IDC_SET_AN_JOY, "analog_joystick", 1, 1); return 0;
        case IDC_SET_AN_PAD: settings_apply_checkbox(hwnd, IDC_SET_AN_PAD, "analog_paddle", 1, 1); return 0;
        case IDC_SET_AN_DRV: settings_apply_checkbox(hwnd, IDC_SET_AN_DRV, "analog_driving", 1, 1); return 0;
        case IDC_SET_PAD_LIST:
            if (HIWORD(wp) == LBN_SELCHANGE) {
                int sel = (int)SendMessageA(g_pad_list, LB_GETCURSEL, 0, 0);
                SendMessageA(g_pad_port, CB_SETCURSEL,
                             (WPARAM)(a2600session_gamepad_assignment(g_session, sel) + 1), 0);
            }
            return 0;
        case IDC_SET_PAD_PORT:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                int sel = (int)SendMessageA(g_pad_list, LB_GETCURSEL, 0, 0);
                int choice = (int)SendMessageA(g_pad_port, CB_GETCURSEL, 0, 0);
                if (sel >= 0 && sel < a2600session_gamepad_count(g_session))
                    a2600session_gamepad_assign(g_session, sel, choice - 1);
                refresh_pad_list();
            }
            return 0;
        case IDC_SET_FUJINET: settings_apply_checkbox(hwnd, IDC_SET_FUJINET, "enable_fujinet", 1, 0); return 0;
        case IDC_SET_AUDIO: settings_apply_checkbox(hwnd, IDC_SET_AUDIO, "enable_audio", 1, 0); return 0;
        case IDC_SET_GAMEPAD: settings_apply_checkbox(hwnd, IDC_SET_GAMEPAD, "enable_gamepad", 1, 0); return 0;
        default: break;
        }
        break;
    case WM_HSCROLL:
        if ((HWND)lp == GetDlgItem(hwnd, IDC_SET_VOLUME))
            a2600session_set_volume(g_session, (int)SendMessageA((HWND)lp, TBM_GETPOS, 0, 0));
        return 0;
    case WM_TIMER:
        if (wp == IDT_SETTINGS_PADS) {
            unsigned gen = a2600session_gamepad_generation(g_session);
            if (gen != g_pad_generation) { g_pad_generation = gen; refresh_pad_list(); }
            update_port_notes();
        }
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, IDT_SETTINGS_PADS);
        g_settings_window = NULL;
        g_pad_list = g_pad_port = NULL;
        g_port_note[0] = g_port_note[1] = NULL;
        if (g_settings_dirty) {
            g_settings_dirty = 0;
            restart_session();
        }
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static HWND settings_checkbox(HWND parent, HINSTANCE inst, const char *text, int id, int x, int y, int w, int checked)
{
    HWND h = CreateWindowExA(0, "BUTTON", text, WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                             x, y, w, 22, parent, (HMENU)(INT_PTR)id, inst, NULL);
    SendMessageA(h, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageA(h, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    return h;
}

static HWND settings_label(HWND parent, HINSTANCE inst, const char *text, int x, int y, int w, int h, int id)
{
    HWND l = CreateWindowExA(0, "STATIC", text, WS_CHILD | WS_VISIBLE, x, y, w, h, parent,
                             (HMENU)(INT_PTR)id, inst, NULL);
    SendMessageA(l, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    return l;
}

static HWND settings_combo(HWND parent, HINSTANCE inst, int id, int x, int y, int w,
                           const char *(*names)(int), int sel)
{
    HWND c = CreateWindowExA(0, "COMBOBOX", "", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                             x, y, w, 200, parent, (HMENU)(INT_PTR)id, inst, NULL);
    int i;
    for (i = 0; names(i); i++) SendMessageA(c, CB_ADDSTRING, 0, (LPARAM)names(i));
    SendMessageA(c, CB_SETCURSEL, (WPARAM)sel, 0);
    SendMessageA(c, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    return c;
}

static void show_settings(HINSTANCE inst)
{
    HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    HWND h;
    int y = 12;

    if (g_settings_window) { SetForegroundWindow(g_settings_window); return; }
    {
        static int registered;
        if (!registered) {
            WNDCLASSA wc;
            memset(&wc, 0, sizeof wc);
            wc.lpfnWndProc = settings_proc;
            wc.hInstance = inst;
            wc.hCursor = LoadCursor(NULL, IDC_ARROW);
            wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
            wc.lpszClassName = "A2600SettingsWindow";
            RegisterClassA(&wc);
            registered = 1;
        }
    }
    g_settings_window = CreateWindowA("A2600SettingsWindow", "Settings",
        WS_OVERLAPPEDWINDOW & ~(WS_MAXIMIZEBOX | WS_THICKFRAME),
        CW_USEDEFAULT, CW_USEDEFAULT, 460, 640, NULL, NULL, inst, NULL);

    settings_label(g_settings_window, inst, "Machine (applied by restarting the session)", 16, y, 420, 18, 0); y += 22;
    settings_label(g_settings_window, inst, "TV format:", 16, y + 3, 90, 18, 0);
    settings_combo(g_settings_window, inst, IDC_SET_TV, 110, y, 200, a2600_tv_format_name,
                   a2600session_get_int(g_session, "tv_format", A2600_TV_AUTO));
    y += 36;

    settings_label(g_settings_window, inst, "Controllers (applied immediately)", 16, y, 420, 18, 0); y += 22;
    settings_label(g_settings_window, inst, "Left port:", 16, y + 3, 90, 18, 0);
    settings_combo(g_settings_window, inst, IDC_SET_PORT0, 110, y, 200, a2600_ctrl_type_name,
                   a2600session_port_type(g_session, 0));
    g_port_note[0] = settings_label(g_settings_window, inst, "", 320, y + 3, 130, 18, IDC_SET_PORT0_NOTE);
    y += 28;
    settings_label(g_settings_window, inst, "Right port:", 16, y + 3, 90, 18, 0);
    settings_combo(g_settings_window, inst, IDC_SET_PORT1, 110, y, 200, a2600_ctrl_type_name,
                   a2600session_port_type(g_session, 1));
    g_port_note[1] = settings_label(g_settings_window, inst, "", 320, y + 3, 130, 18, IDC_SET_PORT1_NOTE);
    y += 32;
    update_port_notes();

    settings_label(g_settings_window, inst, "Analog sticks drive:", 16, y, 420, 18, 0); y += 22;
    settings_checkbox(g_settings_window, inst, "Joystick", IDC_SET_AN_JOY, 16, y, 120,
                      a2600session_get_int(g_session, "analog_joystick", 1));
    settings_checkbox(g_settings_window, inst, "Paddles", IDC_SET_AN_PAD, 150, y, 120,
                      a2600session_get_int(g_session, "analog_paddle", 1));
    settings_checkbox(g_settings_window, inst, "Driving", IDC_SET_AN_DRV, 290, y, 120,
                      a2600session_get_int(g_session, "analog_driving", 1));
    y += 32;

    settings_label(g_settings_window, inst, "Gamepads (select one, then choose its port):", 16, y, 420, 18, 0); y += 22;
    g_pad_list = CreateWindowExA(WS_EX_CLIENTEDGE, "LISTBOX", "",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY, 16, y, 300, 70,
        g_settings_window, (HMENU)(INT_PTR)IDC_SET_PAD_LIST, inst, NULL);
    SendMessageA(g_pad_list, WM_SETFONT, (WPARAM)font, TRUE);
    g_pad_port = CreateWindowExA(0, "COMBOBOX", "", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
        326, y, 118, 200, g_settings_window, (HMENU)(INT_PTR)IDC_SET_PAD_PORT, inst, NULL);
    SendMessageA(g_pad_port, CB_ADDSTRING, 0, (LPARAM)"Automatic");
    SendMessageA(g_pad_port, CB_ADDSTRING, 0, (LPARAM)"Left port");
    SendMessageA(g_pad_port, CB_ADDSTRING, 0, (LPARAM)"Right port");
    SendMessageA(g_pad_port, CB_SETCURSEL, 0, 0);
    SendMessageA(g_pad_port, WM_SETFONT, (WPARAM)font, TRUE);
    g_pad_generation = a2600session_gamepad_generation(g_session);
    refresh_pad_list();
    y += 80;

    settings_label(g_settings_window, inst, "Volume:", 16, y + 3, 90, 18, 0);
    h = CreateWindowExA(0, TRACKBAR_CLASSA, "", WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_AUTOTICKS,
                        110, y, 300, 28, g_settings_window, (HMENU)(INT_PTR)IDC_SET_VOLUME, inst, NULL);
    SendMessageA(h, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
    SendMessageA(h, TBM_SETTICFREQ, 10, 0);
    SendMessageA(h, TBM_SETPOS, TRUE, (LPARAM)a2600session_get_int(g_session, "volume", 100));
    y += 40;

    settings_label(g_settings_window, inst, "Host (applied by restarting the session)", 16, y, 420, 18, 0); y += 22;
    settings_checkbox(g_settings_window, inst, "Enable FujiNet", IDC_SET_FUJINET, 16, y, 200,
                      a2600session_get_int(g_session, "enable_fujinet", 1)); y += 26;
    settings_checkbox(g_settings_window, inst, "Audio", IDC_SET_AUDIO, 16, y, 200,
                      a2600session_get_int(g_session, "enable_audio", 1)); y += 26;
    settings_checkbox(g_settings_window, inst, "Gamepads", IDC_SET_GAMEPAD, 16, y, 200,
                      a2600session_get_int(g_session, "enable_gamepad", 1));

    SetTimer(g_settings_window, IDT_SETTINGS_PADS, 1000, NULL);
    ShowWindow(g_settings_window, SW_SHOW);
}

/* ---- FujiNet console log --------------------------------------------------- */

static HWND g_log_window;
static HWND g_log_edit;

static void log_refresh(void)
{
    static char buf[128 * 1024];
    int n;
    DWORD first, last, lines;
    if (!g_log_edit) return;

    first = (DWORD)SendMessageA(g_log_edit, EM_GETFIRSTVISIBLELINE, 0, 0);
    lines = (DWORD)SendMessageA(g_log_edit, EM_GETLINECOUNT, 0, 0);
    {
        RECT rc;
        HDC dc = GetDC(g_log_edit);
        TEXTMETRICA tm;
        int visible = 1;
        GetClientRect(g_log_edit, &rc);
        if (dc) {
            HFONT of = (HFONT)SelectObject(dc, (HGDIOBJ)SendMessageA(g_log_edit, WM_GETFONT, 0, 0));
            if (GetTextMetricsA(dc, &tm) && tm.tmHeight > 0)
                visible = (rc.bottom - rc.top) / tm.tmHeight;
            SelectObject(dc, of);
            ReleaseDC(g_log_edit, dc);
        }
        last = first + (DWORD)(visible > 0 ? visible : 1);
    }
    n = a2600session_fujinet_copy_log(g_session, buf, sizeof buf);
    SetWindowTextA(g_log_edit, n > 0 ? buf : "(no FujiNet output yet)");
    if (last >= lines) {
        int len = GetWindowTextLengthA(g_log_edit);
        SendMessageA(g_log_edit, EM_SETSEL, (WPARAM)len, (LPARAM)len);
        SendMessageA(g_log_edit, EM_SCROLLCARET, 0, 0);
    }
}

static LRESULT CALLBACK log_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SIZE: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        if (g_log_edit) MoveWindow(g_log_edit, 0, 0, rc.right - rc.left, rc.bottom - rc.top, TRUE);
        return 0;
    }
    case WM_TIMER:
        if (wp == IDT_LOG_REFRESH) log_refresh();
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, IDT_LOG_REFRESH);
        g_log_window = NULL;
        g_log_edit = NULL;
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static void show_fujinet_log(HINSTANCE inst)
{
    RECT rc;
    if (g_log_window) { SetForegroundWindow(g_log_window); return; }
    {
        static int registered;
        if (!registered) {
            WNDCLASSA wc;
            memset(&wc, 0, sizeof wc);
            wc.lpfnWndProc = log_proc;
            wc.hInstance = inst;
            wc.hCursor = LoadCursor(NULL, IDC_ARROW);
            wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
            wc.lpszClassName = "A2600FujiNetLogWindow";
            RegisterClassA(&wc);
            registered = 1;
        }
    }
    g_log_window = CreateWindowA("A2600FujiNetLogWindow", "FujiNet Console Log", WS_OVERLAPPEDWINDOW,
                                 CW_USEDEFAULT, CW_USEDEFAULT, 860, 600, NULL, NULL, inst, NULL);
    GetClientRect(g_log_window, &rc);
    g_log_edit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
        0, 0, rc.right - rc.left, rc.bottom - rc.top, g_log_window, (HMENU)(INT_PTR)IDC_LOG_EDIT, inst, NULL);
    SendMessageA(g_log_edit, WM_SETFONT, (WPARAM)GetStockObject(ANSI_FIXED_FONT), TRUE);
    SetTimer(g_log_window, IDT_LOG_REFRESH, 1000, NULL);
    log_refresh();
    ShowWindow(g_log_window, SW_SHOW);
}

/* ---- window --------------------------------------------------------------- */

static void toggle_fullscreen(HWND hwnd)
{
    DWORD style = GetWindowLong(hwnd, GWL_STYLE);
    if (!g_fullscreen) {
        MONITORINFO mi;
        memset(&mi, 0, sizeof mi);
        mi.cbSize = sizeof mi;
        g_placement.length = sizeof g_placement;
        GetWindowPlacement(hwnd, &g_placement);
        if (GetMonitorInfo(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi)) {
            SetWindowLong(hwnd, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
            SetMenu(hwnd, NULL);
            SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                         mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                         SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
            g_fullscreen = 1;
        }
    } else {
        SetWindowLong(hwnd, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        build_menu(hwnd);
        SetWindowPlacement(hwnd, &g_placement);
        SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        g_fullscreen = 0;
    }
}

static void toggle_switch_item(HWND hwnd, int id, int sw)
{
    HMENU m = GetMenu(hwnd);
    int on = !a2600session_switch_get(g_session, sw);
    a2600session_switch_set(g_session, sw, on);
    if (m) CheckMenuItem(m, id, MF_BYCOMMAND | (on ? MF_CHECKED : MF_UNCHECKED));
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        paint(dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;

    case WM_KEYDOWN: case WM_SYSKEYDOWN: {
        uint32_t ks;
        int sa;
        if (wp == VK_F9) { a2600_keypad_window_toggle(hwnd, g_session); return 0; }
        if (wp == VK_F12) { a2600_debugger_show(hwnd, g_session); return 0; }
        if (wp == VK_F11) { toggle_fullscreen(hwnd); return 0; }
        if (msg == WM_SYSKEYDOWN) {
            if (wp == 'L') { toggle_switch_item(hwnd, IDM_LEFT_DIFF, A2600_SW_LEFT_DIFF); return 0; }
            if (wp == 'R') { toggle_switch_item(hwnd, IDM_RIGHT_DIFF, A2600_SW_RIGHT_DIFF); return 0; }
            break;
        }
        if (GetKeyState(VK_CONTROL) & 0x8000) {
            if (wp == 'O') { PostMessage(hwnd, WM_COMMAND, IDM_OPEN, 0); return 0; }
            if (wp == 'R') { PostMessage(hwnd, WM_COMMAND, IDM_REBOOT_CONFIG, 0); return 0; }
            break;
        }
        if (lp & (1 << 30)) return 0;  /* auto-repeat: the key is already held */
        ks = a2600_keysym_from_msg(wp, lp);
        if (!ks) break;
        sa = a2600session_key_sysaction(g_session, ks);
        if (sa >= 0) {
            if (!g_sysact_down[sa]) { g_sysact_down[sa] = 1; run_sysaction(sa); }
            return 0;
        }
        if (a2600session_key(g_session, ks, 1)) return 0;
        break;
    }
    case WM_KEYUP: case WM_SYSKEYUP: {
        uint32_t ks = a2600_keysym_from_msg(wp, lp);
        int sa;
        if (!ks) break;
        sa = a2600session_key_sysaction(g_session, ks);
        if (sa >= 0) { g_sysact_down[sa] = 0; return 0; }
        if (a2600session_key(g_session, ks, 0)) return 0;
        break;
    }
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) {
            a2600session_release_all(g_session);
            memset(g_sysact_down, 0, sizeof g_sysact_down);
        }
        return 0;

    case WM_TIMER:
        if (wp == IDT_STATUS) update_title();
        else if (wp == IDT_SYSACT) {
            int sa;
            while (a2600session_sysaction_take(g_session, &sa)) run_sysaction(sa);
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_OPEN: {
            char path[MAX_PATH];
            if (pick_cart("Open Cartridge", path, sizeof path)) open_cart(path);
            return 0;
        }
        case IDM_EJECT: a2600session_eject(g_session); return 0;
        case IDM_IMPORT_SD: {
            char path[MAX_PATH], dest[1024], msg[1200];
            if (!pick_cart("Import Cartridge to SD", path, sizeof path)) return 0;
            if (a2600session_import_cart_to_sd(g_session, path, dest, sizeof dest) != 0) {
                MessageBoxA(hwnd, a2600session_last_error(g_session), "Import failed", MB_ICONWARNING | MB_OK);
                return 0;
            }
            snprintf(msg, sizeof msg, "%s is on the SD host. Boot it from the CONFIG client.", base_name(dest));
            MessageBoxA(hwnd, msg, "Imported", MB_ICONINFORMATION | MB_OK);
            return 0;
        }
        case IDM_REBOOT_CONFIG: run_sysaction(A2600_SYSACT_REBOOT_CONFIG); return 0;
        case IDM_SELECT: a2600session_switch_pulse(g_session, A2600_SW_SELECT); return 0;
        case IDM_RESET: a2600session_switch_pulse(g_session, A2600_SW_RESET); return 0;
        case IDM_COLOR: toggle_switch_item(hwnd, IDM_COLOR, A2600_SW_COLOR_BW); return 0;
        case IDM_LEFT_DIFF: toggle_switch_item(hwnd, IDM_LEFT_DIFF, A2600_SW_LEFT_DIFF); return 0;
        case IDM_RIGHT_DIFF: toggle_switch_item(hwnd, IDM_RIGHT_DIFF, A2600_SW_RIGHT_DIFF); return 0;
        case IDM_KEYPAD: a2600_keypad_window_toggle(hwnd, g_session); return 0;
        case IDM_DEBUGGER: a2600_debugger_show(hwnd, g_session); return 0;
        case IDM_FULLSCREEN: toggle_fullscreen(hwnd); return 0;
        case IDM_TV_ASPECT: {
            HMENU m = GetMenu(hwnd);
            g_tv_aspect = !g_tv_aspect;
            if (m) CheckMenuItem(m, IDM_TV_ASPECT, MF_BYCOMMAND | (g_tv_aspect ? MF_CHECKED : MF_UNCHECKED));
            a2600session_set_int(g_session, "tv_aspect", g_tv_aspect);
            InvalidateRect(hwnd, NULL, TRUE);
            return 0;
        }
        case IDM_SMOOTH: {
            HMENU m = GetMenu(hwnd);
            g_smooth = !g_smooth;
            if (m) CheckMenuItem(m, IDM_SMOOTH, MF_BYCOMMAND | (g_smooth ? MF_CHECKED : MF_UNCHECKED));
            a2600session_set_int(g_session, "smooth", g_smooth);
            InvalidateRect(hwnd, NULL, TRUE);
            return 0;
        }
        case IDM_SETTINGS: show_settings((HINSTANCE)GetWindowLongPtr(hwnd, GWLP_HINSTANCE)); return 0;
        case IDM_FUJINET_LOG: show_fujinet_log((HINSTANCE)GetWindowLongPtr(hwnd, GWLP_HINSTANCE)); return 0;
        case IDM_FUJINET_CONFIG:
            if (!a2600session_fujinet_running(g_session)) {
                MessageBoxA(hwnd, "FujiNet is not running.", "FujiNet", MB_ICONINFORMATION | MB_OK);
                return 0;
            }
            ShellExecuteA(hwnd, "open", a2600session_fujinet_webui_url(g_session), NULL, NULL, SW_SHOWNORMAL);
            return 0;
        case IDM_ABOUT:
            MessageBoxA(hwnd,
                APP_TITLE " " A2600_VERSION_STRING "\n\n"
                "An Atari 2600 with a built-in FujiNet.\n"
                "The emulator is Stella (GPL-2.0-or-later) by Bradford W. Mott,\n"
                "Stephen Anthony and the Stella Team, with the FujiNet cartridge.\n\n"
                "Copyright (C) 2026 Thomas Cherryhomes -- GPL-3.0-or-later\n"
                "https://fujinet.online/",
                "About " APP_TITLE, MB_ICONINFORMATION | MB_OK);
            return 0;
        case IDM_EXIT: PostMessage(hwnd, WM_CLOSE, 0, 0); return 0;
        default: break;
        }
        break;

    case WM_DROPFILES: {
        char path[MAX_PATH];
        HDROP drop = (HDROP)wp;
        if (DragQueryFileA(drop, 0, path, sizeof path)) load_media(path);
        DragFinish(drop);
        return 0;
    }

    case WM_DESTROY:
        KillTimer(hwnd, IDT_STATUS);
        KillTimer(hwnd, IDT_SYSACT);
        PostQuitMessage(0);
        return 0;
    default: break;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmdline, int show)
{
    WNDCLASSEX wc;
    MSG msg;
    a2600session_start_opts opts;
    INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_BAR_CLASSES | ICC_TAB_CLASSES | ICC_STANDARD_CLASSES };
    (void)prev;

    InitCommonControlsEx(&icc);
    InitializeCriticalSection(&g_fb_lock);

    g_session = a2600session_new(NULL);
    if (!g_session) {
        MessageBoxA(NULL, "Could not create the session (unusable config or data directories?)",
                    APP_TITLE, MB_ICONERROR | MB_OK);
        return 1;
    }
    g_fb = calloc((size_t)A2600SESSION_FB_WIDTH * A2600SESSION_FB_MAX_HEIGHT, sizeof *g_fb);
    if (!g_fb) return 1;

    memset(&g_bmi, 0, sizeof g_bmi);
    g_bmi.bmiHeader.biSize = sizeof g_bmi.bmiHeader;
    g_bmi.bmiHeader.biWidth = A2600SESSION_FB_WIDTH;
    g_bmi.bmiHeader.biHeight = -A2600SESSION_FB_MAX_HEIGHT;
    g_bmi.bmiHeader.biPlanes = 1;
    g_bmi.bmiHeader.biBitCount = 32;
    g_bmi.bmiHeader.biCompression = BI_RGB;

    g_tv_aspect = a2600session_get_int(g_session, "tv_aspect", 1);
    g_smooth = a2600session_get_int(g_session, "smooth", 0);

    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = WIN_CLASS;
    wc.hIcon = LoadIcon(inst, MAKEINTRESOURCE(IDI_APPICON));
    wc.hIconSm = wc.hIcon;
    RegisterClassEx(&wc);

    g_hwnd = CreateWindowEx(WS_EX_ACCEPTFILES, WIN_CLASS, APP_TITLE, WS_OVERLAPPEDWINDOW,
                            CW_USEDEFAULT, CW_USEDEFAULT, 976, 780, NULL, NULL, inst, NULL);
    if (!g_hwnd) return 1;
    build_menu(g_hwnd);
    CheckMenuItem(GetMenu(g_hwnd), IDM_TV_ASPECT, MF_BYCOMMAND | (g_tv_aspect ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(GetMenu(g_hwnd), IDM_SMOOTH, MF_BYCOMMAND | (g_smooth ? MF_CHECKED : MF_UNCHECKED));
    ShowWindow(g_hwnd, show);

    a2600session_default_opts(g_session, &opts);
    if (cmdline && *cmdline) opts.cart_path = cmdline;
    if (a2600session_start(g_session, &opts) != 0)
        MessageBoxA(g_hwnd, a2600session_last_error(g_session), APP_TITLE, MB_ICONWARNING | MB_OK);

    SetTimer(g_hwnd, IDT_STATUS, 1000, NULL);
    SetTimer(g_hwnd, IDT_SYSACT, 100, NULL);
    update_title();

    {
        const char *env = getenv("A2600_OPEN_KEYPAD");
        if (env && *env && *env != '0') a2600_keypad_window_toggle(g_hwnd, g_session);
        env = getenv("A2600_OPEN_DEBUGGER");
        if (env && *env && *env != '0') a2600_debugger_show(g_hwnd, g_session);
        env = getenv("A2600_OPEN_SETTINGS");
        if (env && *env && *env != '0') show_settings(inst);
    }

    g_present_thread = CreateThread(NULL, 0, present_thread, NULL, 0, NULL);

    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        /* Before TranslateMessage, so the sub-windows' keys reach them
         * regardless of which child control has the focus. */
        if (a2600_debugger_pretranslate(&msg)) continue;
        if (a2600_keypad_pretranslate(&msg)) continue;
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    InterlockedExchange(&g_running, 0);
    if (g_present_thread) {
        WaitForSingleObject(g_present_thread, 2000);
        CloseHandle(g_present_thread);
    }
    a2600session_stop(g_session);
    a2600session_free(g_session);
    free(g_fb);
    DeleteCriticalSection(&g_fb_lock);
    return (int)msg.wParam;
}
