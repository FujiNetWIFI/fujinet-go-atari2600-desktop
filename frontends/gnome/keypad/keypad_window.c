/*
 * A2600KeypadWindow -- both keyboard controllers (the 12-key "keypad" of
 * Star Raiders, BASIC Programming and the Kid's Controller games) side by
 * side, each 1 2 3 / 4 5 6 / 7 8 9 / * 0 #, with a fire button row for the
 * joystick underneath, the console switches, and Map mode.
 *
 * Every control is pressed with a raw GtkGestureClick (press AND release),
 * not GtkButton's "clicked": the machine samples the keypad once per frame,
 * so a value present only for the instant of a click falls between frames
 * and a polling game never sees it. A button on screen is HELD for as long
 * as the mouse button is down, exactly like the plastic one.
 *
 * The gesture's "cancel" is wired to the same release path as "released":
 * dragging off a button must not leave the machine believing a key is
 * still down forever.
 *
 * SINGLETON, hidden rather than destroyed, so a remap survives closing it.
 *
 * MAP MODE: the Map button arms a two-step rebind -- click any control to
 * pick the target, then press a keyboard key OR a gamepad button. While
 * armed, the same gestures pick the target instead of injecting input, the
 * key handler intercepts the next keystroke, and a timer polls the session's
 * gamepad capture. Rebinding steals the key from whatever held it.
 *
 * The keys the ports' keypads are currently bound to are painted in the
 * accent colour while held, and the Map target while armed.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "keypad_window.h"

#include "../window.h"

/* ---- singleton state ------------------------------------------------------ */

static GtkWidget *g_window;
static a2600session *g_session;

/* Map mode: -2 idle, -1 armed and waiting for a target, >= 0 waiting for a
 * key or pad button to bind to that target. */
static int g_map_state = -2;
static GtkWidget *g_map_button;
static GtkWidget *g_map_hint;
static guint g_capture_timer;
static GtkWidget *g_type_label[2];

typedef struct {
    GtkWidget *button;
    int target;
} control;

static control g_controls[A2600_TARGET_COUNT];
static int g_ncontrols;

static void refresh_labels(void);

/* ---- pressing ------------------------------------------------------------- */

static void press_target(int target, int down)
{
    if (target >= A2600_TARGET_SYSACT(0)) {
        /* System actions fire on release, like a real button: pressing and
         * dragging off should not reboot the console. */
        if (!down)
            a2600session_sysaction(g_session, target - A2600_TARGET_SYSACT(0));
        return;
    }
    a2600session_press(g_session, target, down);
}

static void stop_capture(void)
{
    if (g_capture_timer) {
        g_source_remove(g_capture_timer);
        g_capture_timer = 0;
    }
    a2600session_gamepad_capture_cancel(g_session);
}

static gboolean capture_tick(gpointer d)
{
    int button;
    (void)d;
    if (g_map_state < 0) { g_capture_timer = 0; return G_SOURCE_REMOVE; }
    if (a2600session_gamepad_capture_poll(g_session, &button)) {
        char stolen[128], msg[200];
        a2600session_binding_set_button(g_session, g_map_state, button, stolen, sizeof stolen);
        if (stolen[0]) {
            g_snprintf(msg, sizeof msg, "Bound %s (was %s)", a2600_pad_button_name(button), stolen);
            gtk_label_set_text(GTK_LABEL(g_map_hint), msg);
        }
        g_map_state = -1;
        refresh_labels();
        gtk_button_set_label(GTK_BUTTON(g_map_button), "Cancel");
        g_capture_timer = 0;
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static void set_map_state(int state)
{
    g_map_state = state;
    if (state == -2) {
        stop_capture();
        gtk_button_set_label(GTK_BUTTON(g_map_button), "Map");
        gtk_widget_remove_css_class(g_map_button, "a2600-accent");
        gtk_label_set_text(GTK_LABEL(g_map_hint), "");
    } else if (state == -1) {
        stop_capture();
        gtk_button_set_label(GTK_BUTTON(g_map_button), "Cancel");
        gtk_widget_add_css_class(g_map_button, "a2600-accent");
        gtk_label_set_text(GTK_LABEL(g_map_hint), "Click a control to remap");
    } else {
        char msg[128];
        g_snprintf(msg, sizeof msg, "Press a key or gamepad button for %s",
                   a2600_target_name(state));
        gtk_label_set_text(GTK_LABEL(g_map_hint), msg);
        a2600session_gamepad_capture_begin(g_session);
        if (!g_capture_timer)
            g_capture_timer = g_timeout_add(50, capture_tick, NULL);
    }
    refresh_labels();
}

static void on_pressed(GtkGestureClick *g, int n, double x, double y, gpointer user_data)
{
    int target = GPOINTER_TO_INT(user_data);
    GtkWidget *w = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(g));
    (void)n; (void)x; (void)y;

    if (g_map_state == -1) { set_map_state(target); return; }
    if (g_map_state >= 0) return;
    gtk_widget_add_css_class(w, "a2600-accent");
    press_target(target, 1);
}

static void on_released(GtkGestureClick *g, int n, double x, double y, gpointer user_data)
{
    int target = GPOINTER_TO_INT(user_data);
    GtkWidget *w = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(g));
    (void)n; (void)x; (void)y;
    if (g_map_state != -2) return;
    gtk_widget_remove_css_class(w, "a2600-accent");
    press_target(target, 0);
}

static void on_cancelled(GtkGesture *g, GdkEventSequence *seq, gpointer user_data)
{
    int target = GPOINTER_TO_INT(user_data);
    GtkWidget *w = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(g));
    (void)seq;
    if (g_map_state != -2) return;
    gtk_widget_remove_css_class(w, "a2600-accent");
    press_target(target, 0);
}

static GtkWidget *control_button(const char *label, int target, int wide)
{
    GtkWidget *b = gtk_button_new_with_label(label);
    GtkGesture *g = gtk_gesture_click_new();

    gtk_widget_set_size_request(b, wide ? 110 : 56, 44);
    g_signal_connect(g, "pressed", G_CALLBACK(on_pressed), GINT_TO_POINTER(target));
    g_signal_connect(g, "released", G_CALLBACK(on_released), GINT_TO_POINTER(target));
    g_signal_connect(g, "cancel", G_CALLBACK(on_cancelled), GINT_TO_POINTER(target));
    gtk_widget_add_controller(b, GTK_EVENT_CONTROLLER(g));
    gtk_widget_set_focusable(b, FALSE);
    g_object_set_data_full(G_OBJECT(b), "face", g_strdup(label), g_free);

    if (g_ncontrols < A2600_TARGET_COUNT) {
        g_controls[g_ncontrols].button = b;
        g_controls[g_ncontrols].target = target;
        g_ncontrols++;
    }
    return b;
}

/* In Map mode each control shows what is bound to it, so choosing what to
 * change does not require remembering the whole table. */
static void refresh_labels(void)
{
    int i;
    for (i = 0; i < g_ncontrols; i++) {
        int t = g_controls[i].target;
        GtkWidget *b = g_controls[i].button;
        if (g_map_state != -2) {
            a2600_binding bind = a2600session_binding_get(g_session, t);
            char key[32], text[80];
            a2600session_keysym_name(bind.keysym, key, sizeof key);
            if (bind.button != A2600_PAD_BTN_NONE)
                g_snprintf(text, sizeof text, "%s / %s", *key ? key : "\xe2\x80\x94",
                           a2600_pad_button_name(bind.button));
            else
                g_snprintf(text, sizeof text, "%s", *key ? key : "\xe2\x80\x94");
            gtk_button_set_label(GTK_BUTTON(b), text);
            if (t == g_map_state) gtk_widget_add_css_class(b, "a2600-accent");
            else gtk_widget_remove_css_class(b, "a2600-accent");
        } else {
            gtk_widget_remove_css_class(b, "a2600-accent");
            gtk_button_set_label(GTK_BUTTON(b), g_object_get_data(G_OBJECT(b), "face"));
        }
    }
}

/* ---- one controller ------------------------------------------------------- */

static const char *const keypad_face[12] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "*", "0", "#"
};

static GtkWidget *build_controller(int port)
{
    GtkWidget *frame = gtk_frame_new(NULL);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    GtkWidget *pad = gtk_grid_new();
    GtkWidget *fires = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *title;
    char label[32];
    int i;

    g_snprintf(label, sizeof label, "%s Port", port ? "Right" : "Left");
    title = gtk_label_new(label);
    gtk_widget_add_css_class(title, "heading");
    g_type_label[port] = gtk_label_new("");
    gtk_widget_add_css_class(g_type_label[port], "dim-label");

    gtk_grid_set_row_spacing(GTK_GRID(pad), 6);
    gtk_grid_set_column_spacing(GTK_GRID(pad), 6);
    for (i = 0; i < 12; i++)
        gtk_grid_attach(GTK_GRID(pad),
                        control_button(keypad_face[i],
                                       A2600_TARGET_PORT(port, A2600_ACT_KEY_1 + i), 0),
                        i % 3, i / 3, 1, 1);

    gtk_box_append(GTK_BOX(fires),
        control_button("Fire", A2600_TARGET_PORT(port, A2600_ACT_JOY_FIRE), 1));
    gtk_box_append(GTK_BOX(fires),
        control_button("Paddle A", A2600_TARGET_PORT(port, A2600_ACT_PADDLE_A_FIRE), 1));
    gtk_box_append(GTK_BOX(fires),
        control_button("Paddle B", A2600_TARGET_PORT(port, A2600_ACT_PADDLE_B_FIRE), 1));

    gtk_widget_set_halign(pad, GTK_ALIGN_CENTER);
    gtk_widget_set_halign(fires, GTK_ALIGN_CENTER);

    gtk_box_append(GTK_BOX(box), title);
    gtk_box_append(GTK_BOX(box), g_type_label[port]);
    gtk_box_append(GTK_BOX(box), pad);
    gtk_box_append(GTK_BOX(box), fires);
    gtk_widget_set_margin_top(box, 10);
    gtk_widget_set_margin_bottom(box, 10);
    gtk_widget_set_margin_start(box, 12);
    gtk_widget_set_margin_end(box, 12);
    gtk_frame_set_child(GTK_FRAME(frame), box);
    return frame;
}

static gboolean type_tick(gpointer d)
{
    int port;
    (void)d;
    if (!g_window || !gtk_widget_get_visible(g_window)) return G_SOURCE_CONTINUE;
    for (port = 0; port < 2; port++) {
        int det = a2600session_detected_port_type(g_session, port);
        char text[64];
        g_snprintf(text, sizeof text, "%s attached", a2600_ctrl_type_name(det));
        gtk_label_set_text(GTK_LABEL(g_type_label[port]), text);
        if (det == A2600_CTRL_KEYPAD) gtk_widget_add_css_class(g_type_label[port], "a2600-accent-text");
        else gtk_widget_remove_css_class(g_type_label[port], "a2600-accent-text");
    }
    return G_SOURCE_CONTINUE;
}

/* ---- keyboard ------------------------------------------------------------- */

static gboolean on_key_pressed(GtkEventControllerKey *c, guint keyval,
                               guint code, GdkModifierType st, gpointer d)
{
    guint32 keysym = code >= 8 ? a2600session_keysym_from_evdev(code - 8) : 0;
    (void)c; (void)st; (void)d;
    if (!keysym) keysym = keyval;

    if (g_map_state >= 0) {
        char stolen[128], msg[200], name[32];
        a2600session_binding_set_key(g_session, g_map_state, keysym, stolen, sizeof stolen);
        a2600session_keysym_name(keysym, name, sizeof name);
        set_map_state(-1);   /* stay armed: remapping several in a row is normal */
        if (stolen[0]) {
            g_snprintf(msg, sizeof msg, "Bound %s (was %s)", name, stolen);
            gtk_label_set_text(GTK_LABEL(g_map_hint), msg);
        }
        return TRUE;
    }
    if (g_map_state == -1) return TRUE;
    if (keyval == GDK_KEY_F9) {
        a2600_keypad_window_toggle(NULL, g_session);
        return TRUE;
    }
    /* Otherwise behave exactly like the main window, so typing works
     * whichever window has focus. */
    {
        int sa = a2600session_key_sysaction(g_session, keysym);
        if (sa >= 0) { a2600session_sysaction(g_session, sa); return TRUE; }
    }
    return a2600session_key(g_session, keysym, 1) ? TRUE : FALSE;
}

static gboolean on_key_released(GtkEventControllerKey *c, guint keyval,
                                guint code, GdkModifierType st, gpointer d)
{
    guint32 keysym = code >= 8 ? a2600session_keysym_from_evdev(code - 8) : 0;
    (void)c; (void)st; (void)d;
    if (!keysym) keysym = keyval;
    if (g_map_state != -2) return TRUE;
    return a2600session_key(g_session, keysym, 0) ? TRUE : FALSE;
}

/* ---- the window ----------------------------------------------------------- */

static void on_map_clicked(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    set_map_state(g_map_state == -2 ? -1 : -2);
}

static void on_defaults_clicked(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    a2600session_bindings_reset(g_session);
    refresh_labels();
}

static gboolean on_close(GtkWindow *w, gpointer d)
{
    (void)d;
    set_map_state(-2);
    gtk_widget_set_visible(GTK_WIDGET(w), FALSE);
    return TRUE;
}

static void build_window(GtkWindow *parent)
{
    GtkWidget *root, *ports, *system, *maprow, *toolbar, *header, *defaults, *systitle;
    GtkEventController *keys;

    g_window = adw_window_new();
    gtk_window_set_title(GTK_WINDOW(g_window), "Keypads");
    gtk_window_set_transient_for(GTK_WINDOW(g_window), parent);
    gtk_window_set_destroy_with_parent(GTK_WINDOW(g_window), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(g_window), FALSE);
    g_signal_connect(g_window, "close-request", G_CALLBACK(on_close), NULL);

    g_ncontrols = 0;

    root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(root, 12);
    gtk_widget_set_margin_bottom(root, 12);
    gtk_widget_set_margin_start(root, 12);
    gtk_widget_set_margin_end(root, 12);

    ports = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_box_append(GTK_BOX(ports), build_controller(0));
    gtk_box_append(GTK_BOX(ports), build_controller(1));
    gtk_box_append(GTK_BOX(root), ports);

    systitle = gtk_label_new("Console");
    gtk_widget_add_css_class(systitle, "heading");
    gtk_box_append(GTK_BOX(root), systitle);
    system = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(system, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(system), control_button("Select", A2600_TARGET_SWITCH(A2600_SW_SELECT), 1));
    gtk_box_append(GTK_BOX(system), control_button("Reset", A2600_TARGET_SWITCH(A2600_SW_RESET), 1));
    gtk_box_append(GTK_BOX(system), control_button("Color / B&W", A2600_TARGET_SWITCH(A2600_SW_COLOR_BW), 1));
    gtk_box_append(GTK_BOX(system), control_button("Left Diff", A2600_TARGET_SWITCH(A2600_SW_LEFT_DIFF), 1));
    gtk_box_append(GTK_BOX(system), control_button("Right Diff", A2600_TARGET_SWITCH(A2600_SW_RIGHT_DIFF), 1));
    gtk_box_append(GTK_BOX(system), control_button("Reboot to CONFIG",
                   A2600_TARGET_SYSACT(A2600_SYSACT_REBOOT_CONFIG), 1));
    gtk_box_append(GTK_BOX(root), system);

    maprow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    g_map_button = gtk_button_new_with_label("Map");
    g_signal_connect(g_map_button, "clicked", G_CALLBACK(on_map_clicked), NULL);
    defaults = gtk_button_new_with_label("Defaults");
    g_signal_connect(defaults, "clicked", G_CALLBACK(on_defaults_clicked), NULL);
    g_map_hint = gtk_label_new("");
    gtk_widget_add_css_class(g_map_hint, "dim-label");
    gtk_widget_set_hexpand(g_map_hint, TRUE);
    gtk_box_append(GTK_BOX(maprow), g_map_button);
    gtk_box_append(GTK_BOX(maprow), defaults);
    gtk_box_append(GTK_BOX(maprow), g_map_hint);
    gtk_box_append(GTK_BOX(root), maprow);

    header = adw_header_bar_new();
    toolbar = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), root);
    adw_window_set_content(ADW_WINDOW(g_window), toolbar);

    keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key_pressed), NULL);
    g_signal_connect(keys, "key-released", G_CALLBACK(on_key_released), NULL);
    gtk_widget_add_controller(g_window, keys);

    g_timeout_add(1000, type_tick, NULL);
    set_map_state(-2);
}

void a2600_keypad_window_toggle(GtkWindow *parent, a2600session *session)
{
    g_session = session;
    if (!g_window)
        build_window(parent);

    if (gtk_widget_get_visible(g_window)) {
        set_map_state(-2);
        gtk_widget_set_visible(g_window, FALSE);
    } else {
        type_tick(NULL);
        gtk_window_present(GTK_WINDOW(g_window));
    }
}

gboolean a2600_keypad_window_is_visible(void)
{
    return g_window && gtk_widget_get_visible(g_window);
}
