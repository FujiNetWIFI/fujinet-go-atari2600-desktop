/*
 * window.c -- the main window: the display, a header-bar menu, keyboard
 * capture, the console switches and the FujiNet status.
 *
 * Keyboard events are translated by hardware keycode (evdev, via the
 * session's HID table) rather than by GDK keyval, so a binding names the
 * physical key whatever Shift is doing and whatever layout is active -- the
 * same path the Qt, Win32 and AppKit frontends take, which is what lets one
 * tested table serve all four.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "window.h"

#include "display.h"
#include "fujilog.h"
#include "prefs.h"
#include "debugger/dbg_window.h"
#include "keypad/keypad_window.h"

#include <string.h>

struct _A2600Window {
    AdwApplicationWindow parent_instance;

    a2600session *session;
    GtkWidget *display;
    GtkWidget *toast_overlay;
    GtkWidget *status;          /* the FujiNet link indicator */
    GtkWidget *status_dot;
    guint status_id;
    guint sysact_id;
    gboolean sysact_down[A2600_SYSACT_COUNT];
    gboolean fullscreen;
};

G_DEFINE_FINAL_TYPE(A2600Window, a2600_window, ADW_TYPE_APPLICATION_WINDOW)

void a2600_window_toast(A2600Window *self, const char *text)
{
    adw_toast_overlay_add_toast(ADW_TOAST_OVERLAY(self->toast_overlay),
                                adw_toast_new(text));
}

void a2600_install_accent_css(void)
{
    static gboolean done;
    GtkCssProvider *css;
    char buf[512];
    if (done) return;
    done = TRUE;
    g_snprintf(buf, sizeof buf,
        ".a2600-accent { background: #%06x; color: #000000; }\n"
        ".a2600-accent:hover { background: #%06x; }\n"
        ".a2600-accent-text { color: #%06x; font-weight: bold; }\n"
        ".a2600-dot { border-radius: 6px; min-width: 12px; min-height: 12px; }\n"
        ".a2600-dot-on { background: #%06x; }\n"
        ".a2600-dot-off { background: #808080; }\n",
        A2600SESSION_ACCENT_RGB, A2600SESSION_ACCENT_RGB,
        A2600SESSION_ACCENT_RGB, A2600SESSION_ACCENT_RGB);
    css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css, buf);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
        GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);
}

/* ---- input ---------------------------------------------------------------- */

static guint32 keysym_of(guint keyval, guint keycode)
{
    /* the hardware key first; a keyval only for keys evdev has no HID
     * usage for (rare: media keys) */
    guint32 k = keycode >= 8 ? a2600session_keysym_from_evdev(keycode - 8) : 0;
    return k ? k : keyval;
}

static void run_sysaction(A2600Window *self, int sa)
{
    switch (sa) {
    case A2600_SYSACT_REBOOT_CONFIG:
        a2600session_sysaction(self->session, sa);
        a2600_window_toast(self, "Back to the FujiNet CONFIG client");
        break;
    case A2600_SYSACT_PAUSE:
        a2600_debugger_show(GTK_WINDOW(self), self->session);
        a2600session_sysaction(self->session, sa);
        break;
    default:
        break;
    }
}

static gboolean on_key_pressed(GtkEventControllerKey *ctrl, guint keyval,
                               guint keycode, GdkModifierType state,
                               gpointer user_data)
{
    A2600Window *self = user_data;
    guint32 keysym;
    int sa;
    (void)ctrl;

    /* The window's own keys, deliberately not bindable: they are how you
     * reach the panels that do the binding. */
    if (keyval == GDK_KEY_F9) {
        a2600_keypad_window_toggle(GTK_WINDOW(self), self->session);
        return TRUE;
    }
    if (keyval == GDK_KEY_F12) {
        a2600_debugger_show(GTK_WINDOW(self), self->session);
        return TRUE;
    }
    if (keyval == GDK_KEY_F11) {
        gtk_widget_activate_action(GTK_WIDGET(self), "win.fullscreen", NULL);
        return TRUE;
    }
    if ((state & GDK_CONTROL_MASK) || (state & GDK_ALT_MASK))
        return FALSE;   /* menu accelerators */

    keysym = keysym_of(keyval, keycode);
    /* A system action is checked BEFORE the machine keys, so Escape always
     * gets back to CONFIG whatever else the key table says. Leading edge
     * only: GTK4 has no repeat flag. */
    sa = a2600session_key_sysaction(self->session, keysym);
    if (sa >= 0) {
        if (!self->sysact_down[sa]) {
            self->sysact_down[sa] = TRUE;
            run_sysaction(self, sa);
        }
        return TRUE;
    }
    return a2600session_key(self->session, keysym, 1) ? TRUE : FALSE;
}

static gboolean on_key_released(GtkEventControllerKey *ctrl, guint keyval,
                                guint keycode, GdkModifierType state,
                                gpointer user_data)
{
    A2600Window *self = user_data;
    guint32 keysym;
    int sa;
    (void)ctrl; (void)state;

    if (keyval == GDK_KEY_F9 || keyval == GDK_KEY_F11 || keyval == GDK_KEY_F12)
        return TRUE;
    keysym = keysym_of(keyval, keycode);
    sa = a2600session_key_sysaction(self->session, keysym);
    if (sa >= 0) {
        self->sysact_down[sa] = FALSE;
        return TRUE;
    }
    return a2600session_key(self->session, keysym, 0) ? TRUE : FALSE;
}

/* Losing focus with keys held would leave the machine believing they are
 * still down. */
static void on_focus_leave(GtkEventControllerFocus *ctrl, gpointer user_data)
{
    A2600Window *self = user_data;
    (void)ctrl;
    a2600session_release_all(self->session);
    memset(self->sysact_down, 0, sizeof self->sysact_down);
}

/* The gamepad thread cannot call into GTK; it posts system actions and this
 * timer takes them. */
static gboolean sysact_drain_tick(gpointer user_data)
{
    A2600Window *self = user_data;
    int sa;
    while (a2600session_sysaction_take(self->session, &sa))
        run_sysaction(self, sa);
    return G_SOURCE_CONTINUE;
}

/* ---- status --------------------------------------------------------------- */

static gboolean update_status(gpointer user_data)
{
    A2600Window *self = user_data;
    char text[160], st[128];
    gboolean on = FALSE;

    if (!a2600session_is_running(self->session)) {
        g_snprintf(text, sizeof text, "Stopped");
    } else if (a2600session_cart_link_up(self->session) < 0) {
        const char *cart = a2600session_cart_path(self->session);
        const char *slash = cart ? strrchr(cart, '/') : NULL;
        g_snprintf(text, sizeof text, "Local cartridge: %s",
                   slash ? slash + 1 : (cart && *cart ? cart : "?"));
    } else if (a2600session_cart_booted_game(self->session)) {
        g_snprintf(text, sizeof text, "FujiNet: booted a game");
        on = TRUE;
    } else if (a2600session_cart_link_up(self->session) == 1) {
        g_snprintf(text, sizeof text, "FujiNet connected");
        on = TRUE;
    } else {
        a2600session_cart_status(self->session, st, sizeof st);
        g_snprintf(text, sizeof text, "FujiNet: %s", st);
    }
    gtk_label_set_text(GTK_LABEL(self->status), text);
    if (on) {
        gtk_widget_add_css_class(self->status_dot, "a2600-dot-on");
        gtk_widget_remove_css_class(self->status_dot, "a2600-dot-off");
    } else {
        gtk_widget_add_css_class(self->status_dot, "a2600-dot-off");
        gtk_widget_remove_css_class(self->status_dot, "a2600-dot-on");
    }
    return G_SOURCE_CONTINUE;
}

/* ---- cartridges ------------------------------------------------------------ */

static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

static void open_cart_path(A2600Window *self, const char *path)
{
    char msg[1200];
    if (a2600session_load_cart(self->session, path) != 0) {
        a2600_window_toast(self, a2600session_last_error(self->session));
        return;
    }
    g_snprintf(msg, sizeof msg, "Running %s", base_name(path));
    a2600_window_toast(self, msg);
}

/* A dropped file: a cartridge runs; anything else goes where it belongs. */
static void load_media(A2600Window *self, const char *path)
{
    char dest[1024];

    if (a2600session_media_is_cartridge(path)) {
        open_cart_path(self, path);
        return;
    }
    if (a2600session_import_media(self->session, path, dest, sizeof dest) != 0) {
        a2600_window_toast(self, a2600session_last_error(self->session));
        return;
    }
    a2600_window_toast(self, "Copied to FujiNet's SD folder \xe2\x80\x94 mount it "
                             "from the CONFIG client");
}

static GtkFileDialog *cart_dialog(const char *title)
{
    GtkFileDialog *dlg = gtk_file_dialog_new();
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    GtkFileFilter *carts = gtk_file_filter_new();
    GtkFileFilter *all = gtk_file_filter_new();

    gtk_file_filter_set_name(carts, "Atari 2600 cartridges");
    gtk_file_filter_add_pattern(carts, "*.a26");
    gtk_file_filter_add_pattern(carts, "*.bin");
    gtk_file_filter_add_pattern(carts, "*.rom");
    gtk_file_filter_add_pattern(carts, "*.fuji");
    gtk_file_filter_set_name(all, "All files");
    gtk_file_filter_add_pattern(all, "*");
    g_list_store_append(filters, carts);
    g_list_store_append(filters, all);
    gtk_file_dialog_set_title(dlg, title);
    gtk_file_dialog_set_filters(dlg, G_LIST_MODEL(filters));
    g_object_unref(carts);
    g_object_unref(all);
    g_object_unref(filters);
    return dlg;
}

static void on_cart_chosen(GObject *src, GAsyncResult *res, gpointer user_data)
{
    A2600Window *self = user_data;
    g_autoptr(GFile) file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, NULL);
    g_autofree char *path = NULL;
    if (!file) return;
    path = g_file_get_path(file);
    if (path) open_cart_path(self, path);
}

static void action_open(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    A2600Window *self = user_data;
    GtkFileDialog *dlg = cart_dialog("Open Cartridge");
    (void)a; (void)p;
    gtk_file_dialog_open(dlg, GTK_WINDOW(self), NULL, on_cart_chosen, self);
    g_object_unref(dlg);
}

static void on_sd_chosen(GObject *src, GAsyncResult *res, gpointer user_data)
{
    A2600Window *self = user_data;
    g_autoptr(GFile) file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, NULL);
    g_autofree char *path = NULL;
    char dest[1024], msg[1200];
    if (!file) return;
    path = g_file_get_path(file);
    if (!path) return;
    if (a2600session_import_cart_to_sd(self->session, path, dest, sizeof dest) != 0) {
        a2600_window_toast(self, a2600session_last_error(self->session));
        return;
    }
    g_snprintf(msg, sizeof msg, "%s is on the SD host \xe2\x80\x94 boot it from "
               "the CONFIG client", base_name(dest));
    a2600_window_toast(self, msg);
}

static void action_import_sd(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    A2600Window *self = user_data;
    GtkFileDialog *dlg = cart_dialog("Import Cartridge to SD");
    (void)a; (void)p;
    gtk_file_dialog_open(dlg, GTK_WINDOW(self), NULL, on_sd_chosen, self);
    g_object_unref(dlg);
}

static void action_eject(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    A2600Window *self = user_data;
    (void)a; (void)p;
    a2600session_eject(self->session);
    a2600_window_toast(self, "Cartridge ejected \xe2\x80\x94 back to CONFIG");
}

static gboolean on_drop(GtkDropTarget *t, const GValue *value, double x,
                        double y, gpointer user_data)
{
    A2600Window *self = user_data;
    g_autofree char *path = NULL;
    (void)t; (void)x; (void)y;
    if (!G_VALUE_HOLDS(value, G_TYPE_FILE)) return FALSE;
    path = g_file_get_path(G_FILE(g_value_get_object(value)));
    if (!path) return FALSE;
    load_media(self, path);
    return TRUE;
}

/* ---- switches and actions -------------------------------------------------- */

static void action_select(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    A2600Window *self = user_data;
    (void)a; (void)p;
    a2600session_switch_pulse(self->session, A2600_SW_SELECT);
}

static void action_reset(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    A2600Window *self = user_data;
    (void)a; (void)p;
    a2600session_switch_pulse(self->session, A2600_SW_RESET);
}

static void toggle_switch_action(GSimpleAction *a, A2600Window *self, int sw)
{
    gboolean on = !g_variant_get_boolean(g_action_get_state(G_ACTION(a)));
    g_simple_action_set_state(a, g_variant_new_boolean(on));
    a2600session_switch_set(self->session, sw, on);
}

static void action_color(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    (void)p;
    toggle_switch_action(a, user_data, A2600_SW_COLOR_BW);
}

static void action_left_diff(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    (void)p;
    toggle_switch_action(a, user_data, A2600_SW_LEFT_DIFF);
}

static void action_right_diff(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    (void)p;
    toggle_switch_action(a, user_data, A2600_SW_RIGHT_DIFF);
}

static void action_reboot_config(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    (void)a; (void)p;
    run_sysaction(user_data, A2600_SYSACT_REBOOT_CONFIG);
}

static void action_keypad(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    A2600Window *self = user_data;
    (void)a; (void)p;
    a2600_keypad_window_toggle(GTK_WINDOW(self), self->session);
}

static void action_debugger(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    A2600Window *self = user_data;
    (void)a; (void)p;
    a2600_debugger_show(GTK_WINDOW(self), self->session);
}

static void action_fullscreen(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    A2600Window *self = user_data;
    (void)a; (void)p;
    self->fullscreen = !self->fullscreen;
    if (self->fullscreen) gtk_window_fullscreen(GTK_WINDOW(self));
    else gtk_window_unfullscreen(GTK_WINDOW(self));
}

static void action_fujinet_config(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    A2600Window *self = user_data;
    (void)a; (void)p;
    if (!a2600session_fujinet_running(self->session)) {
        a2600_window_toast(self, "FujiNet is not running");
        return;
    }
    a2600_fujiconfig_show(GTK_WINDOW(self), self->session);
}

static void action_fujinet_log(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    A2600Window *self = user_data;
    (void)a; (void)p;
    a2600_fujilog_show(GTK_WINDOW(self), self->session);
}

/* Stop, re-read the settings store, start. Preferences hands this to
 * a2600_prefs_show() as its close callback. */
static void restart_session(A2600Window *self)
{
    a2600session_start_opts o;
    a2600session_settings_flush(self->session);
    a2600session_default_opts(self->session, &o);
    a2600session_stop(self->session);
    if (a2600session_start(self->session, &o) != 0) {
        a2600_window_toast(self, a2600session_last_error(self->session));
        return;
    }
    a2600_window_toast(self, "Machine options applied (session restarted)");
}

static void action_prefs(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    A2600Window *self = user_data;
    (void)a; (void)p;
    a2600_prefs_show(self, self->session, restart_session);
}

static void action_about(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    A2600Window *self = user_data;
    AdwDialog *about;
    (void)a; (void)p;
    about = adw_about_dialog_new();
    adw_about_dialog_set_application_name(ADW_ABOUT_DIALOG(about), "FujiNet Go Atari 2600");
    adw_about_dialog_set_application_icon(ADW_ABOUT_DIALOG(about), a2600_icon_name());
    adw_about_dialog_set_version(ADW_ABOUT_DIALOG(about), A2600_VERSION_STRING);
    adw_about_dialog_set_developer_name(ADW_ABOUT_DIALOG(about), "Thomas Cherryhomes");
    adw_about_dialog_set_website(ADW_ABOUT_DIALOG(about), "https://fujinet.online/");
    adw_about_dialog_set_issue_url(ADW_ABOUT_DIALOG(about),
        "https://github.com/FujiNetWIFI/fujinet-go-atari2600-desktop/issues");
    adw_about_dialog_set_license_type(ADW_ABOUT_DIALOG(about), GTK_LICENSE_GPL_3_0);
    adw_about_dialog_set_comments(ADW_ABOUT_DIALOG(about),
        "An Atari 2600 with a built-in FujiNet. The emulator is Stella "
        "(GPL-2.0-or-later) by Bradford W. Mott, Stephen Anthony and the "
        "Stella Team, with the FujiNet cartridge.");
    adw_dialog_present(about, GTK_WIDGET(self));
}

static void action_aspect(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    A2600Window *self = user_data;
    gboolean tv = !g_variant_get_boolean(g_action_get_state(G_ACTION(a)));
    (void)p;
    g_simple_action_set_state(a, g_variant_new_boolean(tv));
    a2600_display_set_tv_aspect(A2600_DISPLAY(self->display), tv);
    a2600session_set_int(self->session, "tv_aspect", tv ? 1 : 0);
}

static void action_smooth(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    A2600Window *self = user_data;
    gboolean sm = !g_variant_get_boolean(g_action_get_state(G_ACTION(a)));
    (void)p;
    g_simple_action_set_state(a, g_variant_new_boolean(sm));
    a2600_display_set_smooth(A2600_DISPLAY(self->display), sm);
    a2600session_set_int(self->session, "smooth", sm ? 1 : 0);
}

static const GActionEntry win_actions[] = {
    { "open", action_open, NULL, NULL, NULL, { 0 } },
    { "eject", action_eject, NULL, NULL, NULL, { 0 } },
    { "import-sd", action_import_sd, NULL, NULL, NULL, { 0 } },
    { "select", action_select, NULL, NULL, NULL, { 0 } },
    { "reset", action_reset, NULL, NULL, NULL, { 0 } },
    { "color", action_color, NULL, "true", NULL, { 0 } },
    { "left-diff", action_left_diff, NULL, "false", NULL, { 0 } },
    { "right-diff", action_right_diff, NULL, "false", NULL, { 0 } },
    { "reboot-config", action_reboot_config, NULL, NULL, NULL, { 0 } },
    { "keypad", action_keypad, NULL, NULL, NULL, { 0 } },
    { "debugger", action_debugger, NULL, NULL, NULL, { 0 } },
    { "fullscreen", action_fullscreen, NULL, NULL, NULL, { 0 } },
    { "tv-aspect", action_aspect, NULL, "true", NULL, { 0 } },
    { "smooth", action_smooth, NULL, "false", NULL, { 0 } },
    { "fujinet-config", action_fujinet_config, NULL, NULL, NULL, { 0 } },
    { "fujinet-log", action_fujinet_log, NULL, NULL, NULL, { 0 } },
    { "prefs", action_prefs, NULL, NULL, NULL, { 0 } },
    { "about", action_about, NULL, NULL, NULL, { 0 } },
};

/* ---- construction --------------------------------------------------------- */

static GMenu *build_menu(void)
{
    GMenu *menu = g_menu_new();
    GMenu *cart = g_menu_new();
    GMenu *sw = g_menu_new();
    GMenu *view = g_menu_new();
    GMenu *fuji = g_menu_new();
    GMenu *app = g_menu_new();

    g_menu_append(cart, "_Open Cartridge...", "win.open");
    g_menu_append(cart, "_Eject Cartridge", "win.eject");
    g_menu_append(cart, "_Import Cartridge to SD...", "win.import-sd");
    g_menu_append(cart, "Reboot to _CONFIG", "win.reboot-config");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(cart));

    g_menu_append(sw, "_Select (F1)", "win.select");
    g_menu_append(sw, "_Reset (F2)", "win.reset");
    g_menu_append(sw, "Colo_r (F3)", "win.color");
    g_menu_append(sw, "Left Difficulty _A", "win.left-diff");
    g_menu_append(sw, "Right Difficulty A", "win.right-diff");
    g_menu_append_section(menu, "Console Switches", G_MENU_MODEL(sw));

    g_menu_append(view, "_Keypads (F9)", "win.keypad");
    g_menu_append(view, "_Debugger (F12)", "win.debugger");
    g_menu_append(view, "_TV Aspect (4:3)", "win.tv-aspect");
    g_menu_append(view, "_Smooth Scaling", "win.smooth");
    g_menu_append(view, "_Fullscreen (F11)", "win.fullscreen");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(view));

    g_menu_append(fuji, "FujiNet _Configuration", "win.fujinet-config");
    g_menu_append(fuji, "Console _Log", "win.fujinet-log");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(fuji));

    g_menu_append(app, "_Preferences", "win.prefs");
    g_menu_append(app, "_About FujiNet Go Atari 2600", "win.about");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(app));

    g_object_unref(cart);
    g_object_unref(sw);
    g_object_unref(view);
    g_object_unref(fuji);
    g_object_unref(app);
    return menu;
}

static void a2600_window_dispose(GObject *object)
{
    A2600Window *self = A2600_WINDOW(object);
    if (self->status_id) {
        g_source_remove(self->status_id);
        self->status_id = 0;
    }
    if (self->sysact_id) {
        g_source_remove(self->sysact_id);
        self->sysact_id = 0;
    }
    G_OBJECT_CLASS(a2600_window_parent_class)->dispose(object);
}

static void a2600_window_class_init(A2600WindowClass *klass)
{
    G_OBJECT_CLASS(klass)->dispose = a2600_window_dispose;
}

static void a2600_window_init(A2600Window *self)
{
    (void)self;
}

GtkWidget *a2600_window_new(AdwApplication *app, a2600session *session)
{
    A2600Window *self = g_object_new(A2600_TYPE_WINDOW, "application", app, NULL);
    GtkWidget *box, *header, *menu_button, *toolbar, *status_box;
    GtkEventController *keys, *focus;
    g_autoptr(GMenu) menu = NULL;

    self->session = session;
    a2600_install_accent_css();

    gtk_window_set_title(GTK_WINDOW(self), "FujiNet Go Atari 2600");
    gtk_window_set_icon_name(GTK_WINDOW(self), a2600_icon_name());
    /* 320x228 at 3x: the TIA's line doubled to 4:3 width, three times over. */
    gtk_window_set_default_size(GTK_WINDOW(self), 960, 720 + 46);

    g_action_map_add_action_entries(G_ACTION_MAP(self), win_actions,
                                    G_N_ELEMENTS(win_actions), self);
    {
        static const char *const prefs_accels[] = { "<Control>comma", NULL };
        static const char *const open_accels[] = { "<Control>o", NULL };
        static const char *const reboot_accels[] = { "<Control>r", NULL };
        static const char *const ldiff_accels[] = { "<Alt>l", NULL };
        static const char *const rdiff_accels[] = { "<Alt>r", NULL };
        gtk_application_set_accels_for_action(GTK_APPLICATION(app), "win.prefs", prefs_accels);
        gtk_application_set_accels_for_action(GTK_APPLICATION(app), "win.open", open_accels);
        gtk_application_set_accels_for_action(GTK_APPLICATION(app), "win.reboot-config", reboot_accels);
        gtk_application_set_accels_for_action(GTK_APPLICATION(app), "win.left-diff", ldiff_accels);
        gtk_application_set_accels_for_action(GTK_APPLICATION(app), "win.right-diff", rdiff_accels);
    }

    header = adw_header_bar_new();
    menu = build_menu();
    menu_button = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(menu_button), "open-menu-symbolic");
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(menu_button), G_MENU_MODEL(menu));
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), menu_button);

    status_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    self->status_dot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(self->status_dot, "a2600-dot");
    gtk_widget_add_css_class(self->status_dot, "a2600-dot-off");
    gtk_widget_set_valign(self->status_dot, GTK_ALIGN_CENTER);
    self->status = gtk_label_new("Starting...");
    gtk_widget_add_css_class(self->status, "dim-label");
    gtk_box_append(GTK_BOX(status_box), self->status_dot);
    gtk_box_append(GTK_BOX(status_box), self->status);
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), status_box);

    self->display = a2600_display_new(session);
    a2600_display_set_tv_aspect(A2600_DISPLAY(self->display),
        a2600session_get_int(session, "tv_aspect", 1) != 0);
    a2600_display_set_smooth(A2600_DISPLAY(self->display),
        a2600session_get_int(session, "smooth", 0) != 0);
    {
        GAction *a = g_action_map_lookup_action(G_ACTION_MAP(self), "tv-aspect");
        g_simple_action_set_state(G_SIMPLE_ACTION(a),
            g_variant_new_boolean(a2600session_get_int(session, "tv_aspect", 1) != 0));
        a = g_action_map_lookup_action(G_ACTION_MAP(self), "smooth");
        g_simple_action_set_state(G_SIMPLE_ACTION(a),
            g_variant_new_boolean(a2600session_get_int(session, "smooth", 0) != 0));
    }

    {
        GtkDropTarget *drop = gtk_drop_target_new(G_TYPE_FILE, GDK_ACTION_COPY);
        g_signal_connect(drop, "drop", G_CALLBACK(on_drop), self);
        gtk_widget_add_controller(self->display, GTK_EVENT_CONTROLLER(drop));
    }

    self->toast_overlay = adw_toast_overlay_new();
    adw_toast_overlay_set_child(ADW_TOAST_OVERLAY(self->toast_overlay), self->display);

    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    toolbar = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), self->toast_overlay);
    gtk_box_append(GTK_BOX(box), toolbar);
    gtk_widget_set_vexpand(toolbar, TRUE);
    adw_application_window_set_content(ADW_APPLICATION_WINDOW(self), box);

    /* Capture on the WINDOW so input works no matter what has focus. */
    keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key_pressed), self);
    g_signal_connect(keys, "key-released", G_CALLBACK(on_key_released), self);
    gtk_widget_add_controller(GTK_WIDGET(self), keys);

    focus = gtk_event_controller_focus_new();
    g_signal_connect(focus, "leave", G_CALLBACK(on_focus_leave), self);
    gtk_widget_add_controller(GTK_WIDGET(self), focus);

    self->status_id = g_timeout_add_seconds(1, update_status, self);
    self->sysact_id = g_timeout_add(100, sysact_drain_tick, self);
    update_status(self);

    /* A2600_OPEN_KEYPAD=1 / A2600_OPEN_DEBUGGER=1 open those windows at
     * launch, following the family's convention: the way in when the app
     * misbehaves before the menu is reachable. */
    {
        const char *env = g_getenv("A2600_OPEN_KEYPAD");
        if (env && *env && *env != '0')
            a2600_keypad_window_toggle(GTK_WINDOW(self), session);
        env = g_getenv("A2600_OPEN_DEBUGGER");
        if (env && *env && *env != '0')
            a2600_debugger_show(GTK_WINDOW(self), session);
    }
    return GTK_WIDGET(self);
}
